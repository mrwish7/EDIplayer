#pragma once

// EDIplayer engine: receives an EDI multiplex, keeps the service list from
// the FIC, decodes the selected service's audio and PAD (DLS / DL Plus and
// MOT Slideshow).
//
// All decoding runs on the EdiSource receive thread. The GUI talks to the
// player through select() and snapshot(); decoded audio goes to a PcmSink.

#include "edi_source.h"
#include "recorder.h"
#include "spi.h"
extern "C" {
#include "edi/fic.h"
}

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Receives decoded audio: interleaved stereo float, full scale = 1.0 (peaks
// may exceed it slightly; see audio_decoders.h).
class PcmSink {
public:
    virtual ~PcmSink() = default;
    virtual void pcm(const float *data, size_t frames, int rate) = 0;
    virtual void flush() = 0;   // drop buffered audio (service change / stop)
};

struct ServiceEntry {
    uint16_t sid = 0;
    std::string label;
    int scid = -1;
    int kbps = 0;
    bool dabplus = false;      // false: DAB (MP2)
    int pty = -1;              // international programme type (FIG 0/17), -1 unknown
    std::string protection;    // "EEP-3A", "UEP-3", empty if unknown
    int ecc = -1;              // the service's ECC (own if signalled, else the ensemble's), -1 unknown
    bool eccOwn = false;       // ECC signalled for this service (FIG 0/9 extended field)
    uint16_t asu = 0;          // announcement types supported (FIG 0/18)
    uint16_t announcing = 0;   // announcement types active now (FIG 0/19)
    int announceSubch = -1;    // sub-channel carrying the active announcement
};

struct DlPlusItem {
    int type = 0;              // content type (ETSI TS 102 980 table 1)
    std::string text;
};

struct Slide {
    std::vector<uint8_t> data; // JPEG / PNG / GIF / BMP file contents
    std::string ext;
    std::string name;          // MOT ContentName
    std::string categoryTitle;
    std::string clickUrl;
};

// Everything known about the multiplex, for the ensemble information view
struct MuxInfo {
    fic_state_t fic;                       // the decoded FIC
    struct Stream {                        // a sub-channel present in the EDI stream (EST tag)
        int subch = 0;
        int start = 0;                     // start address, CUs
        int tpl = 0;                       // type and protection level
        int bytes = 0;                     // per 24 ms frame
    };
    std::vector<Stream> streams;           // by sub-channel
    // DETI (ETSI TS 102 693)
    int mode = 0;                          // mode identity (1..3; 0 = mode IV)
    int ficLen = 0;                        // FIC bytes per 24 ms frame
    int dflc = 0;                          // frame counter, 0..4999
    bool atst = false;                     // timestamp present
    int utco = 0;                          // UTC offset (TAI - UTC - 32 s)
    uint32_t seconds = 0;                  // seconds since 2000-01-01, TAI
    uint64_t frames = 0;                   // frames received
};

struct PlayerSnapshot {
    bool running = false;
    EdiSource::State sourceState = EdiSource::State::Idle;
    std::string sourceError;
    std::string sourceUrl;

    uint16_t eid = 0;
    int ecc = -1;                          // Extended Country Code (FIG 0/9), -1 until known
    std::string ensemble;
    std::vector<ServiceEntry> services;   // audio services, sorted by label
    std::shared_ptr<const MuxInfo> mux;    // refreshed every ~0.5 s

    int selectedSid = -1;
    std::string audioFormat;               // e.g. "HE-AAC v2, 48 kHz, stereo (PS)"

    // Bitrate breakdown of the playing service, kbit/s averaged over ~4 s
    struct Rates {
        bool valid = false;
        bool dabplus = false;
        double subchannel = 0;   // gross sub-channel rate
        double useful = 0;       // after Reed-Solomon (DAB+), = subchannel for MP2
        double audio = 0;        // coded audio
        double pad = 0;          // programme associated data
        double overhead = 0;     // useful - audio - pad (headers, CRCs, padding)
    } rates;
    bool audioOk = false;                  // decoding produced audio recently

    uint64_t dlsVersion = 0;
    std::string dls;
    std::vector<DlPlusItem> dlplus;
    bool itemRunning = false;

    uint64_t slideVersion = 0;
    std::shared_ptr<const Slide> slide;

    // SPI: service logos (by SId) and carousel progress
    uint64_t logosVersion = 0;
    std::map<uint16_t, ServiceLogos> logos;
    SharedBytes ensembleLogo;              // 32x32 (or nearest square), if the SI has one
    SpiProgress spi;
    std::string spiFolder;

    // recordings of the playing service: [Recorder::Raw], [Recorder::Wav]
    Recorder::State rec[2];

    // statistics
    uint64_t rxBytes = 0, afFrames = 0;
    uint32_t auOk = 0, auErrors = 0, frameGaps = 0;
};

class Player {
public:
    explicit Player(PcmSink *sink);
    ~Player();

    bool start(const std::string &url, std::string &err);
    void stop();
    bool running() const { return running_; }

    // Play the service with this SId (-1: none). Takes effect on the next
    // EDI frame.
    void select(int sid);

    // Folder in which EDIplayer_SPI/ is created (set before start())
    void setSpiBaseDir(const std::string &dir) { spiBaseDir_ = dir; }

    // Record the playing service into <dir>/EDIplayer_audio. Recordings stop
    // when the service changes or the player stops.
    void setRecordBaseDir(const std::string &dir) { recorder_.setBaseDir(dir); }
    bool startRecording(Recorder::Kind k, std::string &err);
    void stopRecording(Recorder::Kind k) { recorder_.stop(k); }

    PlayerSnapshot snapshot() const;

    // Names for DL Plus content types ("Title", "Artist", ...)
    static const char *dlPlusTypeName(int type);

    // Programme type name (European / RDS table, ETSI TS 101 756 table 12)
    static const char *ptyName(int pty);

    // Announcement type name for bit `bit` of the ASu/ASw flags (EN 300 401 table 14)
    static const char *announcementName(int bit);

private:
    struct Engine;
    void onAfPacket(const uint8_t *af, size_t len);

    PcmSink *sink_;
    Recorder recorder_;      // before source_: the receive thread writes to it
    EdiSource source_;
    std::atomic<bool> running_{false};
    std::atomic<int> pendingSid_{-2};      // -2: no change requested
    std::string url_;
    std::string spiBaseDir_ = ".";

    mutable std::mutex mutex_;             // guards snap_
    PlayerSnapshot snap_;
    std::unique_ptr<Engine> eng_;          // receive-thread state
};
