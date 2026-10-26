#pragma once

// Audio recorder for the playing service, two independent recordings:
//
//   Raw: the coded audio as broadcast.
//        DAB  -> .mp2, the MPEG Layer II frames unchanged.
//        DAB+ -> .aac, each AU in a LOAS/LATM frame whose AudioSpecificConfig
//                signals the 960-sample transform and SBR/PS (ADTS can't),
//                so ffmpeg/VLC decode it correctly.
//   Wav: the decoded PCM from the faad / mad decoders, as 32-bit float
//        stereo, so peaks above full scale are kept rather than clipped.
//
// Files go to <dir>/EDIplayer_audio/<date>_<time>_<station>.<ext>. Writes
// happen on the receive thread; start/stop on the GUI thread.

#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

class Recorder {
public:
    enum Kind { Raw = 0, Wav = 1 };

    struct State {
        bool active = false;
        double seconds = 0;
        std::string file;
    };

    ~Recorder();

    void setBaseDir(const std::string &dir) { baseDir_ = dir; }

    // dabplus selects the raw format (.aac / .mp2)
    bool start(Kind k, const std::string &station, bool dabplus, std::string &err);
    void stop(Kind k);
    void stopAll();
    State state(Kind k) const;

    // Receive thread: coded audio (an AU with its superframe format byte, or
    // an MP2 frame) and decoded PCM
    void rawAu(const uint8_t *au, size_t len, uint8_t sfFormat);
    void rawMp2(const uint8_t *frame, size_t len);
    void pcm(const float *data, size_t frames, int rate);

private:
    struct File {
        std::unique_ptr<std::ofstream> f;
        std::string path;
        std::chrono::steady_clock::time_point started;
        bool dabplus = false;
        uint64_t bytes = 0;      // payload bytes written (WAV: PCM data)
        int rate = 0;            // WAV sample rate
        std::chrono::steady_clock::time_point lastPatch;
    };

    void close(File &f, Kind k);
    void writeWavHeader(File &f);

    mutable std::mutex mutex_;
    std::string baseDir_ = ".";
    File files_[2];
};
