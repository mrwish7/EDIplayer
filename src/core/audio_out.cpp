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
    ((AudioOut *)dev->pUserData)->render((float *)out, frames);
}

// Kaiser-windowed sinc (beta 9, 32 taps) tabulated at 256 phases; phase p
// row gives the weights of x[head-15 .. head+16] for a read position
// p/256 past head. Phase 0 is an exact unit impulse, so at normal speed
// samples pass through unchanged.
static double besselI0(double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 32; k++) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
    }
    return sum;
}

static const float *sincTable()
{
    static float table[257][32];
    static bool built = false;
    if (!built) {
        const double half = 16, beta = 9;
        for (int p = 0; p <= 256; p++) {
            for (int j = 0; j < 32; j++) {
                const double t = p / 256.0 - (j - 15);
                const double s = t == 0 ? 1 : sin(M_PI * t) / (M_PI * t);
                const double u = 1 - (t / half) * (t / half);
                table[p][j] = (float)(s * (u > 0 ? besselI0(beta * sqrt(u)) / besselI0(beta) : 0));
            }
        }
        built = true;
    }
    return &table[0][0];
}

AudioOut::AudioOut() { sincTable(); }

AudioOut::~AudioOut() { closeDevice(); }

bool AudioOut::openDevice(int rate)
{
    closeDevice();
    device_ = new ma_device;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
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
    fifo_.assign((size_t)rate * 4 * 2, 0.0f);   // 4 s
    head_ = count_ = 0;
    frac_ = 0;
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

void AudioOut::pcm(const float *data, size_t frames, int rate)
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
    // never write over the interpolator's history behind head_
    for (size_t i = 0; i < frames && count_ + kBefore + 1 < cap; i++) {
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
    std::fill(fifo_.begin(), fifo_.end(), 0.0f);   // no stale history for the interpolator
    failed_ = false;
    peakL_ = peakR_ = 0;
}

void AudioOut::render(float *out, size_t frames)
{
    const float vol = volume_;
    // perceptual volume curve
    const float gain = vol * vol;
    float pl = 0, pr = 0;
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        // the interpolator reads kAfter frames past the read position
        if (playing_ && count_ < frames * 2 + kAfter + 2) {
            playing_ = false;    // underrun: pause and refill
            underruns_++;
        }
        if (playing_) {
            // Drift compensation: the sender's clock and the sound card's differ slightly,
            // so play up to 0.2 % faster or slower to hold the FIFO near its target level.
            const size_t target = (size_t)rate_ * kTargetMs / 1000;
            const size_t band = (size_t)rate_ * 150 / 1000;
            if (count_ > target + band) speed_ = 1.002;
            else if (count_ + band < target) speed_ = 0.998;
            else if ((speed_ > 1 && count_ <= target) || (speed_ < 1 && count_ >= target)) speed_ = 1.0;

            const size_t cap = fifo_.size() / 2;
            const float *table = sincTable();
            for (; n < frames; n++) {
                float l, r;
                if (frac_ == 0) {   // on a sample: exact
                    l = fifo_[head_ * 2];
                    r = fifo_[head_ * 2 + 1];
                } else {
                    // weights for this fractional position, interpolated between table phases
                    const double ph = frac_ * kPhases;
                    const int p0 = (int)ph;
                    const float a = (float)(ph - p0);
                    const float *w0 = table + p0 * kTaps, *w1 = w0 + kTaps;
                    size_t idx = (head_ + cap - kBefore) % cap;
                    l = r = 0;
                    for (int j = 0; j < kTaps; j++) {
                        const float w = w0[j] + (w1[j] - w0[j]) * a;
                        l += fifo_[idx * 2] * w;
                        r += fifo_[idx * 2 + 1] * w;
                        if (++idx == cap) idx = 0;
                    }
                }
                pl = std::max(pl, std::fabs(l));
                pr = std::max(pr, std::fabs(r));
                out[n * 2] = l * gain;      // float: overs pass to the system mixer
                out[n * 2 + 1] = r * gain;
                frac_ += speed_ - 1.0;      // advance one frame, plus the drift correction
                head_ = (head_ + 1) % cap;
                count_--;
                if (frac_ >= 1.0) { frac_ -= 1.0; head_ = (head_ + 1) % cap; count_--; }
                else if (frac_ < 0.0) { frac_ += 1.0; head_ = (head_ + cap - 1) % cap; count_++; }
            }
        }
    }
    if (n < frames) memset(out + n * 2, 0, (frames - n) * 2 * sizeof(float));
    // meter with a simple decay
    peakL_ = std::max(std::min(pl, 1.0f), peakL_ * 0.85f);
    peakR_ = std::max(std::min(pr, 1.0f), peakR_ * 0.85f);
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
