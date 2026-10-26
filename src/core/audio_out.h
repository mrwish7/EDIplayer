#pragma once

// Sound card output (miniaudio) with a jitter buffer.
//
// The EDI stream is paced by the multiplexer's clock and the network, the
// sound card by its own clock, so decoded audio goes through a FIFO: playback
// starts once kPrefillMs are buffered; on underrun it pauses and refills, and
// if the FIFO grows too large (clock drift, network bursts) the excess is
// dropped; in between, playback speed is nudged by up to 0.2 % to follow the
// sender's clock, using a 32-tap windowed-sinc interpolator (linear
// interpolation was audibly gritty: its error was only ~27 dB below the
// music). The device is (re)opened at the stream's sample rate; miniaudio
// converts to the device's native rate if needed.
//
// Samples are float throughout, so peaks slightly above full scale reach the
// system mixer intact (where the system volume normally brings them back into
// range) instead of being hard-clipped here.

#include "player.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

struct ma_device;

class AudioOut : public PcmSink {
public:
    AudioOut();
    ~AudioOut() override;

    void pcm(const float *data, size_t frames, int rate) override;
    void flush() override;

    void setVolume(float v) { volume_ = v; }
    float volume() const { return volume_; }

    // Recent peak levels (0..1), for a level meter
    float peakLeft() const { return peakL_; }
    float peakRight() const { return peakR_; }
    int bufferedMs() const;
    std::string lastError() const;
    uint32_t underruns() const { return underruns_; }

    // miniaudio callback
    void render(float *out, size_t frames);

private:
    bool openDevice(int rate);
    void closeDevice();

    static const int kPrefillMs = 400;   // start playing at this level
    static const int kTargetMs = 600;    // drift compensation steers towards this
    static const int kMaxMs = 1500;      // beyond this, drop audio back to kPrefillMs

    ma_device *device_ = nullptr;
    std::atomic<int> rate_{0};
    std::atomic<bool> failed_{false};
    int failedRate_ = 0;

    // windowed-sinc interpolator: taps x[head-15] .. x[head+16]
    static const int kTaps = 32, kBefore = 15, kAfter = 16, kPhases = 256;

    mutable std::mutex mutex_;          // guards the FIFO
    std::vector<float> fifo_;           // ring of interleaved stereo frames; keeps
                                        // kBefore frames of history behind head_
    size_t head_ = 0, count_ = 0;       // in frames
    bool playing_ = false;              // false while (re)filling
    double frac_ = 0, speed_ = 1.0;     // fractional read position, playback speed

    std::atomic<float> volume_{0.8f};
    std::atomic<float> peakL_{0}, peakR_{0};
    std::atomic<uint32_t> underruns_{0};
    std::string error_;
};
