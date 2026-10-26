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
#include <cctype>
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
    uint64_t lastSeen[64] = {};       // frame counter when each SCID was last present
    std::atomic<uint64_t> frames{0};  // read by the GUI thread for statistics
    std::string listKey;              // detects service list changes
    // Sort by the first label seen: some stations rotate their service label
    // (dynamic PS-style), which would otherwise reshuffle the list.
    std::map<uint16_t, std::string> sortLabel;

    // selected service
    int sid = -1;
    int prevDflc = -1;
    audio_ctx_t audio;
    pad_ctx_t pad;
    dls_ctx_t dls;
    mot_ctx_t mot;
    DabPlusDecoder aac;
    Mp2FrameDecoder mp2;
    std::vector<int16_t> pcm;
    std::string format;
    std::chrono::steady_clock::time_point lastAudio;

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
    }

    static void onAudio(void *opaque, audio_fmt_t fmt, uint8_t sf, const uint8_t *data, size_t len);
    static void onDls(void *opaque, const char *text, const dlplus_t *dlp);
    static void onSlide(void *opaque, const mot_slide_t *s);

    void processFrame(const edi_frame_t &frame);
    void updateServiceList();
    void select(int newSid);
};

void Player::Engine::onAudio(void *opaque, audio_fmt_t fmt, uint8_t sf, const uint8_t *data, size_t len)
{
    Engine *e = (Engine *)opaque;
    e->pcm.clear();
    int rate = 0;
    bool ok;
    std::string desc;
    if (fmt == AUDIO_DABPLUS) {
        ok = e->aac.decode(data, len, sf, e->pcm, rate);
        desc = e->aac.describe();
    } else {
        ok = e->mp2.decode(data, len, e->pcm, rate);
        desc = e->mp2.describe();
    }
    if (ok) e->lastAudio = std::chrono::steady_clock::now();
    if (!e->pcm.empty() && rate > 0 && e->owner->sink_)
        e->owner->sink_->pcm(e->pcm.data(), e->pcm.size() / 2, rate);

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

void Player::Engine::onDls(void *opaque, const char *text, const dlplus_t *dlp)
{
    Engine *e = (Engine *)opaque;
    std::vector<DlPlusItem> items;
    if (dlp) {
        const dls_ctx_t &d = e->dls;
        for (int i = 0; i < dlp->n_tags; i++) {
            const dlplus_tag_t &t = dlp->tags[i];
            if (t.content_type == 0) continue;   // DUMMY
            size_t a = charOffset(d.label_raw, d.label_raw_len, d.label_charset, t.start_marker);
            size_t b = charOffset(d.label_raw, d.label_raw_len, d.label_charset,
                                  (size_t)t.start_marker + t.length_marker + 1);
            if (b <= a) continue;
            char buf[DLS_MAX_TEXT];
            dab_text_to_utf8(buf, sizeof(buf), d.label_raw + a, b - a, d.label_charset);
            items.push_back({t.content_type, buf});
        }
    }
    edi_log("DLS: \"%s\"%s", text, dlp ? " (DL Plus)" : "");
    std::lock_guard<std::mutex> lk(e->owner->mutex_);
    PlayerSnapshot &s = e->owner->snap_;
    s.dls = text;
    s.dlplus = items;
    s.itemRunning = dlp && dlp->item_running;
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
        state += "|" + std::to_string(e.sid) + ":" + e.label + ":" + std::to_string(e.kbps) + std::to_string(e.dabplus);
    if (state == listKey) return;
    listKey = state;

    std::lock_guard<std::mutex> lk(owner->mutex_);
    owner->snap_.services = std::move(list);
    owner->snap_.eid = fic.eid;
    owner->snap_.ensemble = fic.has_ens_label ? fic.ens_label : "";
}

void Player::Engine::select(int newSid)
{
    sid = newSid;
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
}

void Player::Engine::processFrame(const edi_frame_t &frame)
{
    frames++;
    if (frame.deti.ficf) fic_parse(&fic, frame.deti.fic, frame.deti.fic_len);
    for (int i = 0; i < frame.n_est; i++) {
        const est_t &e = frame.est[i];
        if (!e.valid) continue;
        mstSize[e.scid] = e.mst_size;
        lastSeen[e.scid] = frames;
    }
    if (frames % kListInterval == 1) updateServiceList();

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

    const bool ok = std::chrono::steady_clock::now() - lastAudio < std::chrono::seconds(1);
    std::lock_guard<std::mutex> lk(owner->mutex_);
    PlayerSnapshot &s = owner->snap_;
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
    s.rxBytes = source_.bytes;
    s.afFrames = eng_ ? eng_->frames.load() : 0;
    return s;
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
