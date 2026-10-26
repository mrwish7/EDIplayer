#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "miniaudio.h"

#include "audio_out.h"
#include "edi/edi_log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

static void dataCallback(ma_device *dev, void *out, const void *, ma_uint32 frames)
{
    ((AudioOut *)dev->pUserData)->render((int16_t *)out, frames);
}

AudioOut::AudioOut() {}

AudioOut::~AudioOut() { closeDevice(); }

bool AudioOut::openDevice(int rate)
{
    closeDevice();
    device_ = new ma_device;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_s16;
    cfg.playback.channels = 2;
    cfg.sampleRate = (ma_uint32)rate;
    cfg.dataCallback = dataCallback;
    cfg.pUserData = this;
    cfg.periodSizeInMilliseconds = 20;
    ma_result r = ma_device_init(nullptr, &cfg, device_);
    if (r == MA_SUCCESS) r = ma_device_start(device_);
    if (r != MA_SUCCESS) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            error_ = std::string("cannot open audio output: ") + ma_result_description(r);
        }
        edi_log("audio: %s", error_.c_str());
        ma_device_uninit(device_);
        delete device_;
        device_ = nullptr;
        failed_ = true;
        return false;
    }
    edi_log("audio: output opened at %d Hz (%s)", rate, device_->playback.name);
    std::lock_guard<std::mutex> lk(mutex_);
    error_.clear();
    rate_ = rate;
    fifo_.assign((size_t)rate * 4 * 2, 0);   // 4 s
    head_ = count_ = 0;
    playing_ = false;
    return true;
}

void AudioOut::closeDevice()
{
    if (!device_) return;
    ma_device_uninit(device_);   // waits for the callback to finish
    delete device_;
    device_ = nullptr;
    rate_ = 0;
}

void AudioOut::pcm(const int16_t *data, size_t frames, int rate)
{
    // called on the receive thread; the device is only opened/closed here and in flush()
    if (rate != rate_) {
        if (failed_ && rate == failedRate_) return;   // retried after the next flush()
        if (!openDevice(rate)) {
            failedRate_ = rate;
            return;
        }
    }
    if (!device_) return;

    std::lock_guard<std::mutex> lk(mutex_);
    const size_t cap = fifo_.size() / 2;
    const size_t maxFrames = (size_t)rate_ * kMaxMs / 1000;
    if (count_ + frames > maxFrames) {
        // too far behind: drop the oldest audio back to the prefill level
        size_t keep = (size_t)rate_ * kPrefillMs / 1000;
        size_t drop = count_ + frames > keep ? std::min(count_, count_ + frames - keep) : 0;
        head_ = (head_ + drop) % cap;
        count_ -= drop;
    }
    size_t tail = (head_ + count_) % cap;
    for (size_t i = 0; i < frames && count_ < cap; i++) {
        fifo_[tail * 2] = data[i * 2];
        fifo_[tail * 2 + 1] = data[i * 2 + 1];
        tail = (tail + 1) % cap;
        count_++;
    }
    if (!playing_ && count_ >= (size_t)rate_ * kPrefillMs / 1000) playing_ = true;
}

void AudioOut::flush()
{
    std::lock_guard<std::mutex> lk(mutex_);
    head_ = count_ = 0;
    frac_ = 0;
    speed_ = 1.0;
    playing_ = false;
    failed_ = false;
    peakL_ = peakR_ = 0;
}

void AudioOut::render(int16_t *out, size_t frames)
{
    const float vol = volume_;
    // perceptual volume curve
    const float gain = vol * vol;
    int pl = 0, pr = 0;
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (playing_ && count_ < frames * 2 + 2) {
            playing_ = false;    // underrun: pause and refill
            underruns_++;
        }
        if (playing_) {
            // Drift compensation: the sender's clock and the sound card's differ slightly,
            // so play up to 0.2 % faster or slower (linear interpolation) to hold the
            // FIFO near its target level.
            const size_t target = (size_t)rate_ * kTargetMs / 1000;
            const size_t band = (size_t)rate_ * 150 / 1000;
            if (count_ > target + band) speed_ = 1.002;
            else if (count_ + band < target) speed_ = 0.998;
            else if ((speed_ > 1 && count_ <= target) || (speed_ < 1 && count_ >= target)) speed_ = 1.0;

            const size_t cap = fifo_.size() / 2;
            for (; n < frames; n++) {
                const size_t h1 = (head_ + 1) % cap;
                const float f = (float)frac_;
                float l = fifo_[head_ * 2] + (fifo_[h1 * 2] - fifo_[head_ * 2]) * f;
                float r = fifo_[head_ * 2 + 1] + (fifo_[h1 * 2 + 1] - fifo_[head_ * 2 + 1]) * f;
                pl = std::max(pl, (int)std::fabs(l));
                pr = std::max(pr, (int)std::fabs(r));
                out[n * 2] = (int16_t)(l * gain);
                out[n * 2 + 1] = (int16_t)(r * gain);
                frac_ += speed_;
                while (frac_ >= 1.0) {
                    frac_ -= 1.0;
                    head_ = (head_ + 1) % cap;
                    count_--;
                }
            }
        }
    }
    if (n < frames) memset(out + n * 2, 0, (frames - n) * 2 * sizeof(int16_t));
    // meter with a simple decay
    peakL_ = std::max(pl / 32768.0f, peakL_ * 0.85f);
    peakR_ = std::max(pr / 32768.0f, peakR_ * 0.85f);
}

int AudioOut::bufferedMs() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return rate_ ? (int)(count_ * 1000 / rate_) : 0;
}

std::string AudioOut::lastError() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return error_;
}
