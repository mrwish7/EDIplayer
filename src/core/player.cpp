#include "player.h"
#include "audio_decoders.h"

#include "edi/edi_log.h"
extern "C" {
#include "edi/audio.h"
#include "edi/dls.h"
#include "edi/fic.h"
#include "edi/mot.h"
#include "edi/pad.h"
#include "edi/tags.h"
}

#include <algorithm>
#include <array>
#include <cctype>
#include <deque>
#include <chrono>
#include <cstring>
#include <map>

static const uint32_t kPresentFrames = 125;     // sub-channel counts as present if seen in ~3 s
static const uint32_t kListInterval = 20;       // refresh the service list every ~0.5 s

// ---------------------------------------------------------------------------
// Receive-thread state

struct Player::Engine {
    Player *owner;

    fic_state_t fic;
    uint16_t mstSize[64] = {};
    uint16_t mstStart[64] = {};       // EST start address
    uint8_t mstTpl[64] = {};          // EST type and protection level
    uint64_t lastSeen[64] = {};       // frame counter when each SCID was last present
    std::atomic<uint64_t> frames{0};  // read by the GUI thread for statistics
    std::string listKey;              // detects service list changes
    // Sort by the first label seen: some stations rotate their service label
    // (dynamic PS-style), which would otherwise reshuffle the list.
    std::map<uint16_t, std::string> sortLabel;

    // SPI (service logos), decoded for the whole ensemble
    SpiDecoder spi;

    // selected service
    int sid = -1;
    int prevDflc = -1;
    audio_ctx_t audio;
    pad_ctx_t pad;
    dls_ctx_t dls;
    mot_ctx_t mot;
    DabPlusDecoder aac;
    Mp2FrameDecoder mp2;
    std::vector<float> pcm;
    std::string format;
    std::chrono::steady_clock::time_point lastAudio;

    // bitrate breakdown: (logical frames, audio bytes, PAD bytes) sampled every kListInterval
    uint64_t svcFrames = 0;
    std::deque<std::array<uint64_t, 3>> rateSamples;

    // DL Plus objects by content type, kept across labels (see onDls)
    std::map<int, std::string> dlpObjects;
    int dlpToggle = -1;
    bool dlpRunning = false;

    explicit Engine(Player *p) : owner(p)
    {
        fic_init(&fic);
        mot_init(&mot, onSlide, this);
        resetService();
    }
    ~Engine() { mot_free(&mot); }

    void resetService()
    {
        mot_free(&mot);
        mot_init(&mot, onSlide, this);
        dls_init(&dls, onDls, this);
        pad_init(&pad, &dls, &mot);
        audio_init(&audio, pad_process, &pad);
        audio_set_data_cb(&audio, onAudio, this);
        aac.reset();
        mp2.reset();
        format.clear();
        prevDflc = -1;
        dlpObjects.clear();
        dlpToggle = -1;
        dlpRunning = false;
        svcFrames = 0;
        rateSamples.clear();
    }

    static void onAudio(void *opaque, audio_fmt_t fmt, uint8_t sf, const uint8_t *data, size_t len);
    static void onDls(void *opaque, const char *text, const dlplus_t *dlp);
    static void onSlide(void *opaque, const mot_slide_t *s);

    void processFrame(const edi_frame_t &frame);
    void updateServiceList();
    void updateSpi();
    void updateMuxInfo(const edi_frame_t &frame);
    void select(int newSid);
};

void Player::Engine::onAudio(void *opaque, audio_fmt_t fmt, uint8_t sf, const uint8_t *data, size_t len)
{
    Engine *e = (Engine *)opaque;
    Recorder &rec = e->owner->recorder_;
    e->pcm.clear();
    int rate = 0;
    bool ok;
    std::string desc;
    if (fmt == AUDIO_DABPLUS) {
        rec.rawAu(data, len, sf);
        ok = e->aac.decode(data, len, sf, e->pcm, rate);
        desc = e->aac.describe();
    } else {
        rec.rawMp2(data, len);
        ok = e->mp2.decode(data, len, e->pcm, rate);
        desc = e->mp2.describe();
    }
    if (ok) e->lastAudio = std::chrono::steady_clock::now();
    if (!e->pcm.empty() && rate > 0) {
        rec.pcm(e->pcm.data(), e->pcm.size() / 2, rate);
        if (e->owner->sink_) e->owner->sink_->pcm(e->pcm.data(), e->pcm.size() / 2, rate);
    }

    if (desc != e->format) {
        e->format = desc;
        std::lock_guard<std::mutex> lk(e->owner->mutex_);
        e->owner->snap_.audioFormat = desc;
    }
}

// Byte offset of character n in a DAB-coded label
static size_t charOffset(const uint8_t *raw, size_t len, uint8_t charset, size_t n)
{
    if (charset == 0x6) return std::min(len, n * 2);       // UCS-2
    if (charset != 0xF) return std::min(len, n);           // EBU Latin: one byte per character
    size_t i = 0;                                          // UTF-8
    while (n && i < len) {
        i++;
        while (i < len && (raw[i] & 0xC0) == 0x80) i++;
        n--;
    }
    return i;
}

static std::string trimSpaces(const std::string &s)
{
    size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// DL Plus objects (ETSI TS 102 980): each command updates only the content
// types it carries, so objects are kept across labels:
//   - a tag sets the object for its content type to the tagged text
//   - a tag whose text is blank deletes that object (stations point a tag at a
//     single space for this)
//   - a change of the item toggle bit starts a new item: ITEM.* are cleared
//   - DUMMY tags change nothing
void Player::Engine::onDls(void *opaque, const char *text, const dlplus_t *dlp)
{
    Engine *e = (Engine *)opaque;
    if (!text[0]) {   // "remove label" command
        e->dlpObjects.clear();
        e->dlpToggle = -1;
    }
    if (dlp) {
        if (e->dlpToggle >= 0 && (int)dlp->item_toggle != e->dlpToggle)
            for (int ct = 1; ct <= 11; ct++) e->dlpObjects.erase(ct);   // ITEM.TITLE .. ITEM.GENRE
        e->dlpToggle = dlp->item_toggle;
        e->dlpRunning = dlp->item_running;

        const dls_ctx_t &d = e->dls;
        for (int i = 0; i < dlp->n_tags; i++) {
            const dlplus_tag_t &t = dlp->tags[i];
            if (t.content_type == 0) continue;   // DUMMY
            size_t a = charOffset(d.label_raw, d.label_raw_len, d.label_charset, t.start_marker);
            size_t b = charOffset(d.label_raw, d.label_raw_len, d.label_charset,
                                  (size_t)t.start_marker + t.length_marker + 1);
            if (b <= a) continue;                // tag beyond the label: ignore
            char buf[DLS_MAX_TEXT];
            dab_text_to_utf8(buf, sizeof(buf), d.label_raw + a, b - a, d.label_charset);
            std::string v = trimSpaces(buf);
            if (v.empty()) e->dlpObjects.erase(t.content_type);
            else e->dlpObjects[t.content_type] = v;
        }
    }
    std::vector<DlPlusItem> items;
    for (const auto &o : e->dlpObjects) items.push_back({o.first, o.second});
    std::string tags;
    if (dlp) {
        char b[48];
        snprintf(b, sizeof(b), "  [DL+ toggle=%d running=%d", dlp->item_toggle, dlp->item_running);
        tags = b;
        for (int i = 0; i < dlp->n_tags; i++) {
            snprintf(b, sizeof(b), " %d:%d+%d", dlp->tags[i].content_type, dlp->tags[i].start_marker,
                     dlp->tags[i].length_marker);
            tags += b;
        }
        tags += "]";
    }
    edi_log("DLS: \"%s\"%s", text, tags.c_str());
    std::lock_guard<std::mutex> lk(e->owner->mutex_);
    PlayerSnapshot &s = e->owner->snap_;
    s.dls = text;
    s.dlplus = items;
    s.itemRunning = e->dlpRunning;
    s.dlsVersion++;
}

void Player::Engine::onSlide(void *opaque, const mot_slide_t *ms)
{
    Engine *e = (Engine *)opaque;
    {
        // slides repeat in a carousel: only report changes
        std::lock_guard<std::mutex> lk(e->owner->mutex_);
        const auto &cur = e->owner->snap_.slide;
        if (cur && cur->data.size() == ms->len && memcmp(cur->data.data(), ms->data, ms->len) == 0)
            return;
    }
    auto sl = std::make_shared<Slide>();
    sl->data.assign(ms->data, ms->data + ms->len);
    sl->ext = ms->ext ? ms->ext : "";
    sl->name = ms->content_name;
    sl->categoryTitle = ms->category_title;
    sl->clickUrl = ms->click_through_url;
    edi_log("SLS: %s image, %zu bytes, \"%s\"", sl->ext.c_str(), ms->len, ms->content_name);
    std::lock_guard<std::mutex> lk(e->owner->mutex_);
    e->owner->snap_.slide = sl;
    e->owner->snap_.slideVersion++;
}

static std::string lower(const std::string &s)
{
    std::string r = s;
    for (auto &c : r) c = (char)tolower((unsigned char)c);
    return r;
}

void Player::Engine::updateServiceList()
{
    std::vector<ServiceEntry> list;
    for (int i = 0; i < fic.n_svcs; i++) {
        const svc_info_t &sv = fic.svcs[i];
        if (!sv.has_scid || sv.tmid != 0) continue;              // audio services only
        if (!lastSeen[sv.scid] || frames - lastSeen[sv.scid] > kPresentFrames) continue;
        ServiceEntry e;
        e.sid = sv.sid;
        e.scid = sv.scid;
        e.label = sv.has_label ? sv.label : "";
        e.kbps = mstSize[sv.scid] / 3;                           // bytes per 24 ms -> kbit/s
        e.dabplus = sv.ascty == ASCTY_DABPLUS;
        e.pty = sv.has_pty ? sv.pty : -1;
        e.eccOwn = sv.has_ecc;
        e.ecc = sv.has_ecc ? sv.ecc : fic.has_ecc ? fic.ecc : -1;
        e.asu = sv.asu;
        e.announcing = fic_active_announcements(&fic, &sv, &e.announceSubch);
        const fic_subch_t &sc = fic.subch[sv.scid];
        if (sc.valid) {
            char prot[16];
            if (sc.eep) snprintf(prot, sizeof(prot), "EEP-%d%c", sc.level, sc.eep_b ? 'B' : 'A');
            else snprintf(prot, sizeof(prot), "UEP-%d", sc.level);
            e.protection = prot;
        }
        if (!e.label.empty() && !sortLabel.count(e.sid)) sortLabel[e.sid] = lower(e.label);
        list.push_back(e);
    }
    auto key = [&](const ServiceEntry &e) {
        auto it = sortLabel.find(e.sid);
        return it == sortLabel.end() ? std::string() : it->second;
    };
    std::sort(list.begin(), list.end(), [&](const ServiceEntry &a, const ServiceEntry &b) {
        std::string la = key(a), lb = key(b);
        if (la.empty() != lb.empty()) return lb.empty();   // unlabelled services last
        return la != lb ? la < lb : a.sid < b.sid;
    });

    std::string state = fic.has_ens_label ? fic.ens_label : "";
    for (const auto &e : list)
        state += "|" + std::to_string(e.sid) + ":" + e.label + ":" + std::to_string(e.kbps) + std::to_string(e.dabplus) +
                 ":" + std::to_string(e.pty) + e.protection + ":" + std::to_string(e.ecc) + ":" +
                 std::to_string(e.asu) + ":" + std::to_string(e.announcing);
    if (state == listKey) return;
    listKey = state;

    std::lock_guard<std::mutex> lk(owner->mutex_);
    owner->snap_.services = std::move(list);
    owner->snap_.eid = fic.eid;
    owner->snap_.ensemble = fic.has_ens_label ? fic.ens_label : "";
}

// Find SPI components in the FIC, name the output folder, publish logos and progress
void Player::Engine::updateSpi()
{
    fic_app_comp_t comps[16];
    int n = fic_find_packet_apps(&fic, UATYPE_SPI, comps, 16);
    for (int i = 0; i < n; i++) spi.addComponent(comps[i].subch, comps[i].address);

    // EDIplayer_SPI/<ECC><EId>: wait for FIG 0/9; without it, use ECC 00 after ~10 s
    if (fic.has_ens_label && (fic.has_ecc || frames > 400))
        spi.setEnsemble(fic.has_ecc ? fic.ecc : 0, fic.eid);

    std::map<uint16_t, ServiceLogos> logos;
    SharedBytes ensembleLogo;
    const bool newLogos = spi.takeLogos(logos, ensembleLogo);
    const SpiProgress pr = spi.progress();
    std::lock_guard<std::mutex> lk(owner->mutex_);
    PlayerSnapshot &s = owner->snap_;
    if (newLogos) {
        s.logos = std::move(logos);
        s.ensembleLogo = ensembleLogo;
        s.logosVersion++;
    }
    s.spi = pr;
    s.spiFolder = spi.folder();
    s.ecc = fic.has_ecc ? fic.ecc : -1;
}

void Player::Engine::updateMuxInfo(const edi_frame_t &frame)
{
    auto m = std::make_shared<MuxInfo>();
    m->fic = fic;
    for (int sc = 0; sc < 64; sc++) {
        if (!lastSeen[sc] || frames - lastSeen[sc] > kPresentFrames) continue;
        MuxInfo::Stream st;
        st.subch = sc;
        st.start = mstStart[sc];
        st.tpl = mstTpl[sc];
        st.bytes = mstSize[sc];
        m->streams.push_back(st);
    }
    if (frame.deti_valid) {
        m->mode = frame.deti.mid;
        m->ficLen = frame.deti.ficf ? frame.deti.fic_len : 0;
        m->dflc = frame.deti.dflc;
        m->atst = frame.deti.atstf;
        m->utco = frame.deti.utco;
        m->seconds = frame.deti.seconds;
    }
    m->frames = frames;
    std::lock_guard<std::mutex> lk(owner->mutex_);
    owner->snap_.mux = std::move(m);
}

void Player::Engine::select(int newSid)
{
    sid = newSid;
    owner->recorder_.stopAll();   // a recording belongs to one service
    resetService();
    if (owner->sink_) owner->sink_->flush();
    std::lock_guard<std::mutex> lk(owner->mutex_);
    PlayerSnapshot &s = owner->snap_;
    s.selectedSid = newSid;
    s.audioFormat.clear();
    s.dls.clear();
    s.dlplus.clear();
    s.itemRunning = false;
    s.dlsVersion++;
    s.slide.reset();
    s.slideVersion++;
    s.auOk = s.auErrors = s.frameGaps = 0;
    s.rates = PlayerSnapshot::Rates();
}

void Player::Engine::processFrame(const edi_frame_t &frame)
{
    frames++;
    if (frame.deti.ficf) fic_parse(&fic, frame.deti.fic, frame.deti.fic_len);
    for (int i = 0; i < frame.n_est; i++) {
        const est_t &e = frame.est[i];
        if (!e.valid) continue;
        mstSize[e.scid] = e.mst_size;
        mstStart[e.scid] = e.sad;
        mstTpl[e.scid] = e.tpl;
        lastSeen[e.scid] = frames;
        if (spi.hasSubchannel(e.scid)) spi.feed(e.scid, e.mst, e.mst_size);
    }
    if (frames % kListInterval == 1) {
        updateServiceList();
        updateSpi();
        updateMuxInfo(frame);
    }

    int pend = owner->pendingSid_.exchange(-2);
    if (pend != -2) select(pend);
    if (sid < 0) return;

    // the service's sub-channel can move with a reconfiguration, so look it up each time
    const est_t *target = nullptr;
    for (int i = 0; i < fic.n_svcs && !target; i++) {
        if (fic.svcs[i].sid != sid || !fic.svcs[i].has_scid) continue;
        for (int k = 0; k < frame.n_est; k++)
            if (frame.est[k].valid && frame.est[k].scid == fic.svcs[i].scid) target = &frame.est[k];
    }
    if (!target) return;

    // a missing logical frame breaks superframe assembly - start over
    uint32_t gaps = 0;
    if (prevDflc >= 0 && frame.deti.dflc != (prevDflc + 1) % 5000) {
        gaps = 1;
        audio_reset(&audio);
    }
    prevDflc = frame.deti.dflc;

    audio_feed(&audio, target->mst, target->mst_size);
    svcFrames++;

    // bitrate breakdown over the last ~4 s (8 samples of 20 logical frames)
    PlayerSnapshot::Rates rates;
    bool newRates = false;
    if (frames % kListInterval == 1) {
        rateSamples.push_back({svcFrames, audio.audio_bytes, audio.pad_bytes});
        while (rateSamples.size() > 9) rateSamples.pop_front();
        const auto &a = rateSamples.front(), &b = rateSamples.back();
        if (rateSamples.size() >= 2 && b[0] > a[0] && audio.fmt != AUDIO_UNKNOWN) {
            const double secs = (b[0] - a[0]) * 0.024;
            rates.valid = true;
            rates.dabplus = audio.fmt == AUDIO_DABPLUS;
            rates.subchannel = target->mst_size * 8.0 / 24.0;
            rates.useful = rates.dabplus ? rates.subchannel * 110.0 / 120.0 : rates.subchannel;   // RS(120,110)
            rates.audio = (b[1] - a[1]) * 8.0 / secs / 1000.0;
            rates.pad = (b[2] - a[2]) * 8.0 / secs / 1000.0;
            rates.overhead = std::max(0.0, rates.useful - rates.audio - rates.pad);
        }
        newRates = true;
    }

    const bool ok = std::chrono::steady_clock::now() - lastAudio < std::chrono::seconds(1);
    std::lock_guard<std::mutex> lk(owner->mutex_);
    PlayerSnapshot &s = owner->snap_;
    if (newRates) s.rates = rates;
    s.auOk = audio.au_ok;
    s.auErrors = audio.au_crc_err;
    s.frameGaps += gaps;
    s.audioOk = ok;
}

// ---------------------------------------------------------------------------

Player::Player(PcmSink *sink) : sink_(sink), eng_(new Engine(this)) {}

Player::~Player() { stop(); }

bool Player::start(const std::string &url, std::string &err)
{
    stop();
    EdiSourceUrl u;
    if (!EdiSourceUrl::parse(url, u, err)) return false;

    eng_.reset(new Engine(this));
    eng_->spi.setBaseDir(spiBaseDir_);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        int keep = snap_.selectedSid;
        snap_ = PlayerSnapshot();
        snap_.running = true;
        snap_.sourceUrl = u.describe();
        snap_.selectedSid = keep;
    }
    // resume the previously selected service once it shows up
    pendingSid_ = snapshot().selectedSid;
    url_ = url;
    edi_log("player: starting %s", u.describe().c_str());
    source_.start(u, [this](const uint8_t *af, size_t len) { onAfPacket(af, len); });
    running_ = true;
    return true;
}

void Player::stop()
{
    if (!running_) return;
    source_.stop();
    recorder_.stopAll();
    running_ = false;
    if (sink_) sink_->flush();
    std::lock_guard<std::mutex> lk(mutex_);
    snap_.running = false;
    snap_.audioOk = false;
    edi_log("player: stopped");
}

void Player::select(int sid)
{
    if (running_) {
        pendingSid_ = sid;
    } else {
        std::lock_guard<std::mutex> lk(mutex_);
        snap_.selectedSid = sid;
    }
}

bool Player::startRecording(Recorder::Kind k, std::string &err)
{
    std::string station;
    bool dabplus = false, found = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto &e : snap_.services) {
            if (e.sid != snap_.selectedSid) continue;
            char sid[16];
            snprintf(sid, sizeof(sid), "SId %04X", e.sid);
            station = e.label.empty() ? sid : e.label;
            dabplus = e.dabplus;
            found = true;
        }
    }
    if (!running_ || !found) {
        err = "Select a service to record";
        return false;
    }
    return recorder_.start(k, station, dabplus, err);
}

void Player::onAfPacket(const uint8_t *af, size_t len)
{
    static edi_frame_t frame;   // only the receive thread uses it (large: keep off the stack)
    if (!parse_af_packet(&frame, af, len)) return;
    eng_->processFrame(frame);
}

PlayerSnapshot Player::snapshot() const
{
    PlayerSnapshot s;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        s = snap_;
    }
    s.sourceState = source_.state();
    s.sourceError = source_.lastError();
    s.rec[Recorder::Raw] = recorder_.state(Recorder::Raw);
    s.rec[Recorder::Wav] = recorder_.state(Recorder::Wav);
    s.rxBytes = source_.bytes;
    s.afFrames = eng_ ? eng_->frames.load() : 0;
    return s;
}

const char *Player::ptyName(int pty)
{
    static const char *const names[32] = {
        "None", "News", "Current Affairs", "Information", "Sport", "Education", "Drama", "Culture",
        "Science", "Varied", "Pop Music", "Rock Music", "Easy Listening", "Light Classical", "Serious Classical",
        "Other Music", "Weather", "Finance", "Children's", "Social Affairs", "Religion", "Phone In", "Travel",
        "Leisure", "Jazz Music", "Country Music", "National Music", "Oldies Music", "Folk Music", "Documentary",
        "", "",
    };
    return pty >= 0 && pty < 32 ? names[pty] : "";
}

const char *Player::announcementName(int bit)
{
    static const char *const names[11] = {
        "Alarm", "Road traffic", "Transport", "Warning/service", "News", "Weather",
        "Event", "Special event", "Programme info", "Sport", "Financial",
    };
    return bit >= 0 && bit < 11 ? names[bit] : "Announcement";
}

const char *Player::dlPlusTypeName(int type)
{
    static const char *const names[64] = {
        "Dummy", "Title", "Album", "Track", "Artist", "Composition", "Movement", "Conductor",
        "Composer", "Band", "Comment", "Genre", "News", "Local news", "Stock market", "Sport",
        "Lottery", "Horoscope", "Daily diversion", "Health", "Event", "Scene", "Cinema", "Stupidity machine",
        "Date & time", "Weather", "Traffic", "Alarm", "Advertisement", "URL", "Info", "Station (short)",
        "Station", "Now", "Next", "Part", "Host", "Editorial staff", "Frequency", "Homepage",
        "Sub-channel", "Hotline", "Studio phone", "Phone", "SMS studio", "SMS", "Email hotline", "Email studio",
        "Email", "MMS", "Chat", "Chat centre", "Vote question", "Vote centre", "", "",
        "Private 1", "Private 2", "Private 3", "Place", "Appointment", "Identifier", "Purchase", "Get data",
    };
    return type >= 0 && type < 64 ? names[type] : "";
}
