#pragma once

// EDIplayer engine: receives an EDI multiplex, keeps the service list from
// the FIC, decodes the selected service's audio and PAD (DLS / DL Plus and
// MOT Slideshow).
//
// All decoding runs on the EdiSource receive thread. The GUI talks to the
// player through select() and snapshot(); decoded audio goes to a PcmSink.

#include "edi_source.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Receives decoded audio: interleaved 16-bit stereo.
class PcmSink {
public:
    virtual ~PcmSink() = default;
    virtual void pcm(const int16_t *data, size_t frames, int rate) = 0;
    virtual void flush() = 0;   // drop buffered audio (service change / stop)
};

struct ServiceEntry {
    uint16_t sid = 0;
    std::string label;
    int scid = -1;
    int kbps = 0;
    bool dabplus = false;      // false: DAB (MP2)
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

struct PlayerSnapshot {
    bool running = false;
    EdiSource::State sourceState = EdiSource::State::Idle;
    std::string sourceError;
    std::string sourceUrl;

    uint16_t eid = 0;
    std::string ensemble;
    std::vector<ServiceEntry> services;   // audio services, sorted by label

    int selectedSid = -1;
    std::string audioFormat;               // e.g. "HE-AAC v2, 48 kHz, stereo (PS)"
    bool audioOk = false;                  // decoding produced audio recently

    uint64_t dlsVersion = 0;
    std::string dls;
    std::vector<DlPlusItem> dlplus;
    bool itemRunning = false;

    uint64_t slideVersion = 0;
    std::shared_ptr<const Slide> slide;

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

    PlayerSnapshot snapshot() const;

    // Names for DL Plus content types ("Title", "Artist", ...)
    static const char *dlPlusTypeName(int type);

private:
    struct Engine;
    void onAfPacket(const uint8_t *af, size_t len);

    PcmSink *sink_;
    EdiSource source_;
    std::atomic<bool> running_{false};
    std::atomic<int> pendingSid_{-2};      // -2: no change requested
    std::string url_;

    mutable std::mutex mutex_;             // guards snap_
    PlayerSnapshot snap_;
    std::unique_ptr<Engine> eng_;          // receive-thread state
};
