#include "spi.h"
#include "edi/edi_log.h"
extern "C" {
#include "edi/crc16.h"
}

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>

namespace fs = std::filesystem;

static const size_t kMaxCarouselBytes = 64u << 20;   // drop stray bodies beyond this

// UTF-8 path string (u8string() is std::u8string in C++20)
static std::string pathStr(const fs::path &p)
{
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

static bool crcOk(const uint8_t *p, size_t len)
{
    if (len < 2) return false;
    uint16_t stored = (uint16_t)((p[len - 2] << 8) | p[len - 1]);
    return (uint16_t)(crc16(0xFFFF, p, len - 2) ^ 0xFFFF) == stored;
}

// ---------------------------------------------------------------------------
// Image header sizes

bool imageSize(const uint8_t *d, size_t n, int &w, int &h)
{
    auto be16 = [&](size_t i) { return (d[i] << 8) | d[i + 1]; };
    if (n >= 24 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') {
        w = (int)(((uint32_t)d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19]);
        h = (int)(((uint32_t)d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23]);
        return true;
    }
    if (n >= 10 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F') {
        w = d[6] | (d[7] << 8);
        h = d[8] | (d[9] << 8);
        return true;
    }
    if (n >= 26 && d[0] == 'B' && d[1] == 'M') {
        w = (int)(d[18] | (d[19] << 8) | (d[20] << 16) | ((uint32_t)d[21] << 24));
        h = std::abs((int)(d[22] | (d[23] << 8) | (d[24] << 16) | ((uint32_t)d[25] << 24)));
        return true;
    }
    if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8) {   // JPEG: find the SOFn marker
        size_t i = 2;
        while (i + 9 < n) {
            if (d[i] != 0xFF) { i++; continue; }
            uint8_t m = d[i + 1];
            if (m == 0xFF) { i++; continue; }
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                h = be16(i + 5);
                w = be16(i + 7);
                return true;
            }
            if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
            i += 2 + be16(i + 2);
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// MOT carousel (EN 301 234), directory mode and header mode

class MotCarousel {
public:
    using ObjectCallback = std::function<void(const std::string &, int, int, bool, const Bytes &)>;
    explicit MotCarousel(ObjectCallback cb) : cb_(std::move(cb)) {}

    void dataGroup(const uint8_t *dg, size_t len);
    int objects() const { return (int)entries_.size(); }
    int complete() const { return (int)done_.size(); }
    bool hasDirectory() const { return dirParsed_; }

private:
    struct Segments {
        std::map<int, Bytes> seg;
        int last = -1;
        size_t bytes = 0;
        bool complete() const
        {
            if (last < 0 || (int)seg.size() < last + 1) return false;
            for (int i = 0; i <= last; i++)
                if (!seg.count(i)) return false;
            return true;
        }
        Bytes join() const
        {
            Bytes b;
            for (int i = 0; i <= last; i++) b.insert(b.end(), seg.at(i).begin(), seg.at(i).end());
            return b;
        }
        // returns false if the segment differs from one already held
        bool add(int num, bool isLast, const uint8_t *p, size_t n)
        {
            auto it = seg.find(num);
            if (it != seg.end()) return it->second.size() == n && memcmp(it->second.data(), p, n) == 0;
            seg[num].assign(p, p + n);
            bytes += n;
            if (isLast) last = num;
            return true;
        }
    };
    struct Entry {
        std::string name;
        int ctype = 0, csub = 0;
        bool compressed = false;
        uint32_t bodySize = 0;
        bool operator==(const Entry &o) const
        {
            return name == o.name && ctype == o.ctype && csub == o.csub && bodySize == o.bodySize;
        }
    };

    static bool parseHeader(const uint8_t *h, size_t len, Entry &e, size_t &headerSize);
    void parseDirectory(const Bytes &d);
    void tryComplete(uint16_t tid);
    size_t storedBytes() const;

    ObjectCallback cb_;
    int dirTid_ = -1;
    Segments dir_;
    bool dirParsed_ = false;
    std::map<uint16_t, Entry> entries_;
    std::map<uint16_t, Segments> bodies_, headers_;
    std::set<uint16_t> done_;
};

// MOT header (EN 301 234 §6.1): core + extension parameters
bool MotCarousel::parseHeader(const uint8_t *h, size_t len, Entry &e, size_t &headerSize)
{
    if (len < 7) return false;
    e.bodySize = ((uint32_t)h[0] << 20) | ((uint32_t)h[1] << 12) | ((uint32_t)h[2] << 4) | (h[3] >> 4);
    headerSize = ((size_t)(h[3] & 0x0F) << 9) | ((size_t)h[4] << 1) | (h[5] >> 7);
    e.ctype = (h[5] >> 1) & 0x3F;
    e.csub = ((h[5] & 0x01) << 8) | h[6];
    if (headerSize < 7 || headerSize > len) return false;

    size_t i = 7;
    while (i < headerSize) {
        uint8_t pli = h[i] >> 6, pid = h[i] & 0x3F;
        i++;
        size_t dlen = 0;
        if (pli == 1) dlen = 1;
        else if (pli == 2) dlen = 4;
        else if (pli == 3) {
            if (i >= headerSize) return false;
            if (h[i] & 0x80) {
                if (i + 1 >= headerSize) return false;
                dlen = ((size_t)(h[i] & 0x7F) << 8) | h[i + 1];
                i += 2;
            } else {
                dlen = h[i] & 0x7F;
                i++;
            }
        }
        if (i + dlen > headerSize) return false;
        if (pid == 0x0C && dlen >= 1) e.name.assign((const char *)h + i + 1, dlen - 1);   // ContentName (charset byte first)
        if (pid == 0x11) e.compressed = true;                                              // CompressionType
        i += dlen;
    }
    return true;
}

// MOT directory (EN 301 234 §7.2.3)
void MotCarousel::parseDirectory(const Bytes &d)
{
    if (d.size() < 13) return;
    size_t dirSize = ((size_t)(d[0] & 0x3F) << 24) | ((size_t)d[1] << 16) | ((size_t)d[2] << 8) | d[3];
    int nObjects = (d[4] << 8) | d[5];
    size_t extLen = ((size_t)d[11] << 8) | d[12];
    if (dirSize > d.size()) dirSize = d.size();

    std::map<uint16_t, Entry> entries;
    size_t i = 13 + extLen;
    for (int n = 0; n < nObjects && i + 2 + 7 <= dirSize; n++) {
        uint16_t tid = (uint16_t)((d[i] << 8) | d[i + 1]);
        Entry e;
        size_t hs = 0;
        if (!parseHeader(&d[i + 2], dirSize - i - 2, e, hs)) break;
        entries[tid] = e;
        i += 2 + hs;
    }

    // keep objects that are unchanged, forget the rest
    for (auto it = done_.begin(); it != done_.end();) {
        auto o = entries_.find(*it), nw = entries.find(*it);
        if (o == entries_.end() || nw == entries.end() || !(o->second == nw->second)) it = done_.erase(it);
        else ++it;
    }
    for (auto it = bodies_.begin(); it != bodies_.end();) {
        if (!entries.count(it->first)) it = bodies_.erase(it);
        else ++it;
    }
    entries_ = std::move(entries);
    dirParsed_ = true;
    edi_log("SPI: MOT directory with %zu objects", entries_.size());
    for (const auto &e : entries_) tryComplete(e.first);
}

void MotCarousel::tryComplete(uint16_t tid)
{
    if (done_.count(tid)) return;
    auto e = entries_.find(tid);
    auto b = bodies_.find(tid);
    if (e == entries_.end() || b == bodies_.end() || !b->second.complete()) return;
    Bytes body = b->second.join();
    if (body.size() != e->second.bodySize) {   // stale or corrupt: collect again
        bodies_.erase(b);
        return;
    }
    done_.insert(tid);
    bodies_.erase(b);   // the decoder keeps what it needs
    cb_(e->second.name, e->second.ctype, e->second.csub, e->second.compressed, body);
}

size_t MotCarousel::storedBytes() const
{
    size_t n = 0;
    for (const auto &b : bodies_) n += b.second.bytes;
    return n;
}

// MSC data group (EN 300 401 §5.3.3) carrying a MOT segment
void MotCarousel::dataGroup(const uint8_t *dg, size_t len)
{
    if (len < 2) return;
    bool extFlag = (dg[0] >> 7) & 1, crcFlag = (dg[0] >> 6) & 1;
    bool segFlag = (dg[0] >> 5) & 1, uaFlag = (dg[0] >> 4) & 1;
    int type = dg[0] & 0x0F;
    size_t end = len;
    if (crcFlag) {
        if (!crcOk(dg, len)) return;
        end -= 2;
    }
    size_t off = 2 + (extFlag ? 2 : 0);
    bool last = false;
    int segNum = 0;
    if (segFlag) {
        if (off + 2 > end) return;
        last = dg[off] >> 7;
        segNum = ((dg[off] & 0x7F) << 8) | dg[off + 1];
        off += 2;
    }
    int tid = -1;
    if (uaFlag) {
        if (off + 1 > end) return;
        bool tidFlag = (dg[off] >> 4) & 1;
        size_t li = dg[off] & 0x0F;
        off++;
        if (off + li > end) return;
        if (tidFlag && li >= 2) tid = (dg[off] << 8) | dg[off + 1];
        off += li;
    }
    if (tid < 0 || off + 2 > end) return;
    size_t segSize = ((size_t)(dg[off] & 0x1F) << 8) | dg[off + 1];
    off += 2;
    if (off + segSize > end) return;
    const uint8_t *seg = dg + off;

    switch (type) {
    case 6:   // MOT directory (uncompressed)
        if (tid != dirTid_) {
            dirTid_ = tid;
            dir_ = Segments();
        }
        if (!dir_.complete()) {
            dir_.add(segNum, last, seg, segSize);
            if (dir_.complete()) parseDirectory(dir_.join());
        }
        break;
    case 3: { // MOT header (header mode)
        Segments &h = headers_[(uint16_t)tid];
        if (!h.add(segNum, last, seg, segSize)) h = Segments(), h.add(segNum, last, seg, segSize);
        if (h.complete()) {
            Entry e;
            size_t hs;
            Bytes hb = h.join();
            if (parseHeader(hb.data(), hb.size(), e, hs) && !entries_.count((uint16_t)tid)) {
                entries_[(uint16_t)tid] = e;
                tryComplete((uint16_t)tid);
            }
        }
        break;
    }
    case 4: { // MOT body
        if (done_.count((uint16_t)tid)) break;
        Segments &b = bodies_[(uint16_t)tid];
        if (!b.add(segNum, last, seg, segSize)) {   // object changed under this TId
            b = Segments();
            b.add(segNum, last, seg, segSize);
        }
        tryComplete((uint16_t)tid);
        if (storedBytes() > kMaxCarouselBytes) {
            for (auto it = bodies_.begin(); it != bodies_.end();)
                it = entries_.count(it->first) ? std::next(it) : bodies_.erase(it);
        }
        break;
    }
    case 7:
        break;    // compressed directory: not supported
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Packet mode (EN 300 401 §5.3.2)

struct SpiDecoder::Packets {
    std::map<uint16_t, MotCarousel *> addr;
    std::map<uint16_t, Bytes> dg;
    std::map<uint16_t, int> cont;   // expected continuity index
};

SpiDecoder::SpiDecoder() {}
SpiDecoder::~SpiDecoder() {}

void SpiDecoder::addComponent(uint8_t subch, uint16_t address)
{
    auto key = std::make_pair(subch, address);
    if (carousels_.count(key)) return;
    auto c = std::make_unique<MotCarousel>([this](const std::string &n, int t, int s, bool z, const Bytes &b) {
        onObject(n, t, s, z, b);
    });
    auto &p = subch_[subch];
    if (!p) p.reset(new Packets);
    p->addr[address] = c.get();
    carousels_[key] = std::move(c);
    edi_log("SPI: decoding sub-channel %u, packet address %u", subch, address);
}

bool SpiDecoder::hasSubchannel(uint8_t subch) const { return subch_.count(subch) != 0; }

void SpiDecoder::feed(uint8_t subch, const uint8_t *mst, size_t len)
{
    auto it = subch_.find(subch);
    if (it == subch_.end()) return;
    Packets &pk = *it->second;

    size_t pos = 0;
    while (pos + 24 <= len) {
        const uint8_t *p = mst + pos;
        const size_t plen = (size_t)((p[0] >> 6) + 1) * 24;
        if (pos + plen > len) break;
        pos += plen;
        if (!crcOk(p, plen)) continue;

        const uint16_t address = (uint16_t)(((p[0] & 0x03) << 8) | p[1]);
        auto a = pk.addr.find(address);
        if (a == pk.addr.end()) continue;           // padding (0) or another component
        const int ci = (p[0] >> 4) & 0x03;
        const bool first = (p[0] >> 3) & 1, last = (p[0] >> 2) & 1;
        size_t useful = p[2] & 0x7F;
        if (useful > plen - 5) continue;

        Bytes &dg = pk.dg[address];
        auto c = pk.cont.find(address);
        if (first) {
            dg.clear();
        } else if (c == pk.cont.end() || c->second != ci || dg.empty()) {
            dg.clear();                             // lost a packet: wait for the next first packet
            pk.cont[address] = (ci + 1) & 3;
            continue;
        }
        pk.cont[address] = (ci + 1) & 3;
        dg.insert(dg.end(), p + 3, p + 3 + useful);
        if (last) {
            a->second->dataGroup(dg.data(), dg.size());
            dg.clear();
        }
    }
}

SpiProgress SpiDecoder::progress() const
{
    SpiProgress pr;
    pr.carousels = (int)carousels_.size();
    for (const auto &c : carousels_) {
        pr.objects += c.second->objects();
        pr.complete += c.second->complete();
        pr.directory |= c.second->hasDirectory();
    }
    return pr;
}

// ---------------------------------------------------------------------------
// Objects

void SpiDecoder::onObject(const std::string &name, int ctype, int csub, bool compressed, const Bytes &body)
{
    saveFile(name, body);
    if (ctype == 2 && !compressed) {   // image
        images_[name] = std::make_shared<const Bytes>(body);
        resolveLogos();
    } else if (ctype == 7 && csub == 0) {   // SPI Service Information
        if (compressed)
            edi_log("SPI: %s is compressed - not supported", name.c_str());
        else {
            parseServiceInformation(body);
            resolveLogos();
        }
    }
}

// File name for a ContentName: keep sub-folders, drop anything unsafe
static std::string safePath(const std::string &name)
{
    std::string out, part;
    auto flush = [&]() {
        if (!part.empty() && part != "." && part != "..") out += (out.empty() ? "" : "/") + part;
        part.clear();
    };
    for (char c : name) {
        if (c == '/' || c == '\\') { flush(); continue; }
        if ((unsigned char)c < 0x20 || strchr(":*?\"<>|", c)) c = '_';
        part += c;
    }
    flush();
    return out.empty() ? "unnamed" : out;
}

void SpiDecoder::saveFile(const std::string &name, const Bytes &body)
{
    if (!haveEnsemble_) {
        pendingSave_.push_back({name, body});
        return;
    }
    auto s = saved_.find(name);
    if (s != saved_.end() && s->second == body.size()) return;   // carousel repeat
    std::error_code ec;
    fs::path p = fs::u8path(folder_) / fs::u8path(safePath(name));
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) {
            edi_log("SPI: cannot write %s", pathStr(p).c_str());
            return;
        }
        f.write((const char *)body.data(), (std::streamsize)body.size());
    }
    fs::rename(tmp, p, ec);
    if (ec) fs::remove(tmp, ec);
    saved_[name] = body.size();
}

void SpiDecoder::setEnsemble(uint8_t ecc, uint16_t eid)
{
    char id[8];
    snprintf(id, sizeof(id), "%02X%04X", ecc, eid);
    std::string f = pathStr(fs::u8path(baseDir_) / "EDIplayer_SPI" / id);
    if (haveEnsemble_ && f == folder_) return;
    folder_ = f;
    haveEnsemble_ = true;
    std::error_code ec;
    fs::create_directories(fs::u8path(folder_), ec);
    if (ec) edi_log("SPI: cannot create %s: %s", folder_.c_str(), ec.message().c_str());
    for (auto &p : pendingSave_) saveFile(p.first, p.second);
    pendingSave_.clear();
    loadIndex();
}

// ---------------------------------------------------------------------------
// Binary encoded Service Information (ETSI TS 102 371), tags as AbracaDABra

namespace {
enum : uint8_t {
    TagCdata = 0x01, TagServiceInformation = 0x03, TagTokenTable = 0x04,
    TagServices = 0x26, TagService = 0x28, TagBearerServiceId = 0x29, TagMultimedia = 0x2B, TagBearer = 0x2D,
    AttrBearerId = 0x80, AttrMmUrl = 0x82, AttrMmType = 0x83, AttrMmWidth = 0x84, AttrMmHeight = 0x85,
};

struct SiParser {
    std::map<uint8_t, std::string> tokens;
    std::map<uint16_t, std::vector<std::pair<std::string, std::array<int, 3>>>> out;   // SId -> (url, type/w/h)

    // service being parsed
    std::vector<uint16_t> sids;
    std::vector<std::pair<std::string, std::array<int, 3>>> media;
    // multimedia inside the ensemble (<services>) element but outside any <service>
    // (usually wrapped in <mediaDescription>): the ensemble logo
    std::vector<std::pair<std::string, std::array<int, 3>>> ensembleMedia;
    std::vector<std::pair<std::string, std::array<int, 3>>> *current = nullptr;   // multimedia being filled
    int inService = 0, inEnsemble = 0;

    std::string str(const uint8_t *p, size_t n) const
    {
        std::string s;
        for (size_t i = 0; i < n; i++) {
            auto t = tokens.find(p[i]);
            if (p[i] < 0x20 && t != tokens.end()) s += t->second;
            else s += (char)p[i];
        }
        return s;
    }

    // dab bearer URI (TS 102 371 §4.7.6): flags/SCIdS, [ECC EId], SId (16 or 32 bit)
    static int bearerSid(const uint8_t *p, size_t n)
    {
        if (n < 3) return -1;
        bool longSid = p[0] & 0x10;
        size_t sidLen = longSid ? 4 : 2;
        size_t off = n >= 1 + 3 + sidLen ? 4 : 1;   // ECC + EId present?
        if (off + sidLen > n) return -1;
        if (longSid) return -1;                       // data service: no logo slot
        if (p[0] & 0x0F) return -1;                   // SCIdS > 0: a secondary component of that
                                                      // service, not the service itself
        return (p[off] << 8) | p[off + 1];
    }

    // Returns bytes consumed
    size_t element(const uint8_t *p, size_t max, uint8_t parent)
    {
        if (max < 2) return max;
        uint8_t tag = p[0];
        size_t len = p[1], hdr = 2;
        if (len == 0xFE) {
            if (max < 4) return max;
            len = ((size_t)p[2] << 8) | p[3];
            hdr = 4;
        } else if (len == 0xFF) {
            if (max < 5) return max;
            len = ((size_t)p[2] << 16) | ((size_t)p[3] << 8) | p[4];
            hdr = 5;
        }
        if (hdr + len > max) return max;
        const uint8_t *v = p + hdr;

        if (tag >= 0x80) {   // attribute of `parent`
            if ((parent == TagBearer || parent == TagBearerServiceId) && tag == AttrBearerId) {
                int sid = bearerSid(v, len);
                if (sid >= 0) sids.push_back((uint16_t)sid);
            } else if (parent == TagMultimedia && current && !current->empty()) {
                auto &m = current->back();
                if (tag == AttrMmUrl) m.first = str(v, len);
                else if (tag == AttrMmType && len >= 1) m.second[0] = v[0];
                else if (tag == AttrMmWidth && len >= 2) m.second[1] = (v[0] << 8) | v[1];
                else if (tag == AttrMmHeight && len >= 2) m.second[2] = (v[0] << 8) | v[1];
            }
            return hdr + len;
        }

        if (tag == TagTokenTable) {
            for (size_t i = 0; i + 2 <= len;) {
                uint8_t id = v[i], tl = v[i + 1];
                if (i + 2 + tl > len) break;
                tokens[id] = std::string((const char *)v + i + 2, tl);
                i += 2 + tl;
            }
            return hdr + len;
        }
        if (tag == TagCdata) return hdr + len;

        if (tag == TagService) {
            sids.clear();
            media.clear();
        }
        if (tag == TagMultimedia) {
            current = inEnsemble && !inService ? &ensembleMedia : &media;
            current->push_back({std::string(), {0, 0, 0}});
        }

        inService += tag == TagService;
        inEnsemble += tag == TagServices;
        for (size_t i = 0; i < len;) i += element(v + i, len - i, tag);
        inService -= tag == TagService;
        inEnsemble -= tag == TagServices;

        if (tag == TagService)
            for (uint16_t sid : sids)
                for (const auto &m : media)
                    if (!m.first.empty()) out[sid].push_back(m);
        return hdr + len;
    }
};
}   // namespace

void SpiDecoder::parseServiceInformation(const Bytes &body)
{
    SiParser p;
    for (size_t i = 0; i < body.size();) i += p.element(body.data() + i, body.size() - i, 0);
    size_t n = 0;
    for (const auto &s : p.out) {
        auto &v = siLogos_[s.first];
        v.clear();
        for (const auto &m : s.second) v.push_back({m.first, m.second[0], m.second[1], m.second[2]});
        n += v.size();
    }
    siEnsemble_.clear();
    for (const auto &m : p.ensembleMedia)
        if (!m.first.empty()) siEnsemble_.push_back({m.first, m.second[0], m.second[1], m.second[2]});
    edi_log("SPI: service information: %zu services, %zu logos, %zu ensemble logos", p.out.size(), n,
            siEnsemble_.size());
}

// Find a received image for a logo URL (relative ContentName, or the last part of a URL)
static SharedBytes findImage(const std::map<std::string, SharedBytes> &images, const std::string &url, std::string &name)
{
    auto try1 = [&](const std::string &k) -> SharedBytes {
        auto it = images.find(k);
        if (it == images.end()) return nullptr;
        name = k;
        return it->second;
    };
    if (auto b = try1(url)) return b;
    std::string u = url;
    while (!u.empty() && u[0] == '/') u.erase(0, 1);
    if (auto b = try1(u)) return b;
    if (auto b = try1("/" + u)) return b;
    size_t s = url.rfind('/');
    if (s != std::string::npos)
        if (auto b = try1(url.substr(s + 1))) return b;
    return nullptr;
}

// How well a square logo suits the 32x32 list icon (lower is better): 32x32
// itself, else the smallest larger square (it's scaled down), else the
// largest smaller one. Non-square images aren't used.
static int smallLogoScore(int w, int h)
{
    if (w != h || w <= 0) return -1;
    if (w == 32) return 0;
    if (w > 32) return 1000 + w;
    return 100000 - w;
}

void SpiDecoder::resolveLogos()
{
    bool changed = false;
    for (const auto &s : siLogos_) {
        ServiceLogos l = logos_[s.first];
        auto names = logoNames_[s.first];
        SharedBytes bestSmall;
        std::string bestSmallName;
        int bestScore = -1;
        for (const auto &m : s.second) {
            std::string name;
            SharedBytes img = findImage(images_, m.url, name);
            if (!img) continue;
            int w = m.w, h = m.h;
            if (!w || !h) imageSize(img->data(), img->size(), w, h);
            const int score = smallLogoScore(w, h);
            if (score >= 0 && (bestScore < 0 || score < bestScore)) {
                bestScore = score;
                bestSmall = img;
                bestSmallName = name;
            }
            if (w == 320 && h == 240 && l.logo320 != img) {
                l.logo320 = img;
                names.second = name;
                changed = true;
            }
        }
        if (bestSmall && l.logo32 != bestSmall) {
            l.logo32 = bestSmall;
            names.first = bestSmallName;
            changed = true;
        }
        logos_[s.first] = l;
        logoNames_[s.first] = names;
    }

    // ensemble logo: the same choice of square as a service's list icon
    int bestScore = -1;
    for (const auto &m : siEnsemble_) {
        std::string name;
        SharedBytes img = findImage(images_, m.url, name);
        if (!img) continue;
        int w = m.w, h = m.h;
        if (!w || !h) imageSize(img->data(), img->size(), w, h);
        const int score = smallLogoScore(w, h);
        if (score < 0 || (bestScore >= 0 && score >= bestScore)) continue;
        bestScore = score;
        if (ensembleLogo_ != img) {
            ensembleLogo_ = img;
            ensembleLogoName_ = name;
            changed = true;
        }
    }

    if (changed) {
        logosChanged_ = true;
        writeIndex();
    }
}

bool SpiDecoder::takeLogos(std::map<uint16_t, ServiceLogos> &out, SharedBytes &ensemble)
{
    if (!logosChanged_) return false;
    logosChanged_ = false;
    out = logos_;
    ensemble = ensembleLogo_;
    return true;
}

// logos.txt: "<SId hex> <list icon file> <320x240 file>", '-' for none, and
// "ENSEMBLE <file>" for the ensemble logo
void SpiDecoder::writeIndex()
{
    if (!haveEnsemble_) return;
    std::ofstream f(fs::u8path(folder_) / "logos.txt");
    f << "# EDIplayer: service logos from SPI (SId, list icon: 32x32 or nearest square, 320x240)\n";
    if (!ensembleLogoName_.empty()) f << "ENSEMBLE\t" << safePath(ensembleLogoName_) << "\n";
    for (const auto &n : logoNames_) {
        if (n.second.first.empty() && n.second.second.empty()) continue;
        char sid[8];
        snprintf(sid, sizeof(sid), "%04X", n.first);
        f << sid << "\t" << (n.second.first.empty() ? "-" : safePath(n.second.first)) << "\t"
          << (n.second.second.empty() ? "-" : safePath(n.second.second)) << "\n";
    }
}

void SpiDecoder::loadIndex()
{
    std::ifstream f(fs::u8path(folder_) / "logos.txt");
    std::string line;
    int n = 0;
    auto read = [&](const std::string &file) -> SharedBytes {
        if (file == "-" || file.empty()) return nullptr;
        std::ifstream in(fs::u8path(folder_) / fs::u8path(file), std::ios::binary);
        if (!in) return nullptr;
        auto b = std::make_shared<Bytes>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return b->empty() ? nullptr : b;
    };
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string sid, file32, file320;
        if (!std::getline(ss, sid, '\t') || !std::getline(ss, file32, '\t')) continue;
        std::getline(ss, file320, '\t');
        if (sid == "ENSEMBLE") {
            if (!ensembleLogo_ && (ensembleLogo_ = read(file32))) {
                ensembleLogoName_ = file32;
                n++;
            }
            continue;
        }
        uint16_t id = (uint16_t)strtol(sid.c_str(), nullptr, 16);
        ServiceLogos &l = logos_[id];
        auto &names = logoNames_[id];
        if (!l.logo32 && (l.logo32 = read(file32))) names.first = file32;
        if (!l.logo320 && (l.logo320 = read(file320))) names.second = file320;
        if (l.logo32 || l.logo320) n++;
    }
    if (n) {
        edi_log("SPI: %d service logos loaded from %s", n, folder_.c_str());
        logosChanged_ = true;
    }
}
