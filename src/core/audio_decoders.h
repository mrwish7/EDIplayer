#pragma once

// Audio decoders for DAB services, built on the libfaad (AAC) and libmad
// (MPEG Layer II) ports in src/decoders. Both produce interleaved stereo
// float PCM, full scale = 1.0. Peaks are NOT clipped: loud broadcasts decode
// to slightly above full scale (inter-sample overs), and clipping them here
// is audible distortion.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class NeaacDecoder;
class MP2Decoder;
class Audio;

// DAB+ (HE-AAC v2 with 960-sample transform, ETSI TS 102 563)
class DabPlusDecoder {
public:
    DabPlusDecoder();
    ~DabPlusDecoder();

    // Decode one access unit. sfFormat is the superframe format byte, which
    // selects the decoder configuration. au == nullptr (a corrupt AU) yields
    // one frame of silence so playback timing is kept. Appends to `out`.
    bool decode(const uint8_t *au, size_t len, uint8_t sfFormat, std::vector<float> &out, int &rate);
    void reset();

    // e.g. "HE-AAC v2, 48 kHz, stereo (PS)"
    std::string describe() const;

private:
    bool configure(uint8_t sfFormat);

    std::unique_ptr<NeaacDecoder> dec_;
    void *handle_ = nullptr;
    int format_ = -1;          // sfFormat the decoder is configured for
    int rate_ = 0;             // output sample rate
    size_t lastFrames_ = 0;    // samples per channel of the last good AU
    bool sbr_ = false, ps_ = false, stereo_ = false;
    int errors_ = 0;
};

// DAB (MPEG-1 Layer II, ETSI EN 300 401 §7)
class Mp2FrameDecoder {
public:
    Mp2FrameDecoder();
    ~Mp2FrameDecoder();

    // Decode one MPEG audio frame (one 24 ms logical frame at 48 kHz).
    bool decode(const uint8_t *frame, size_t len, std::vector<float> &out, int &rate);
    void reset();
    std::string describe() const;

private:
    std::unique_ptr<Audio> audio_;
    std::unique_ptr<MP2Decoder> dec_;
    std::vector<uint8_t> in_;
    std::vector<int32_t> pcm_;
    int rate_ = 0, channels_ = 0, bitrate_ = 0;
};
