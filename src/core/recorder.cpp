#include "recorder.h"
#include "edi/edi_log.h"
extern "C" {
#include "edi/audio.h"
}

#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

Recorder::~Recorder() { stopAll(); }

// "2026-10-26_12-34-56_SWR3", safe as a file name on Windows and Linux
static std::string fileStem(const std::string &station)
{
    char ts[32];
    std::time_t t = std::time(nullptr);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::strftime(ts, sizeof(ts), "%Y-%m-%d_%H-%M-%S", &tm);
    std::string name;
    for (char c : station) {
        if ((unsigned char)c < 0x20 || strchr("\\/:*?\"<>|", c)) c = '_';
        name += c;
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    return std::string(ts) + "_" + (name.empty() ? "service" : name);
}

bool Recorder::start(Kind k, const std::string &station, bool dabplus, std::string &err)
{
    std::lock_guard<std::mutex> lk(mutex_);
    File &f = files_[k];
    if (f.f) return true;

    std::error_code ec;
    fs::path dir = fs::u8path(baseDir_) / "EDIplayer_audio";
    fs::create_directories(dir, ec);
    const char *ext = k == Wav ? ".wav" : dabplus ? ".aac" : ".mp2";
    fs::path p = dir / fs::u8path(fileStem(station) + ext);   // raw and wav differ by extension
    auto u8 = p.u8string();
    const std::string path(u8.begin(), u8.end());
    f.f.reset(new std::ofstream(p, std::ios::binary));
    if (!*f.f) {
        f.f.reset();
        err = "cannot create " + path;
        edi_log("record: %s", err.c_str());
        return false;
    }
    f.path = path;
    f.dabplus = dabplus;
    f.bytes = 0;
    f.rate = 0;
    f.started = f.lastPatch = std::chrono::steady_clock::now();
    if (k == Wav) writeWavHeader(f);   // placeholder until the rate is known
    edi_log("record: started %s", f.path.c_str());
    return true;
}

void Recorder::close(File &f, Kind k)
{
    if (!f.f) return;
    if (k == Wav) writeWavHeader(f);
    f.f->close();
    f.f.reset();
    edi_log("record: saved %s (%.1f MB)", f.path.c_str(), f.bytes / 1e6);
}

void Recorder::stop(Kind k)
{
    std::lock_guard<std::mutex> lk(mutex_);
    close(files_[k], k);
}

void Recorder::stopAll()
{
    std::lock_guard<std::mutex> lk(mutex_);
    close(files_[Raw], Raw);
    close(files_[Wav], Wav);
}

Recorder::State Recorder::state(Kind k) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    State s;
    const File &f = files_[k];
    s.file = f.path;
    s.active = f.f != nullptr;
    if (s.active)
        s.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - f.started).count();
    return s;
}

// ---------------------------------------------------------------------------
// WAV

void Recorder::writeWavHeader(File &f)
{
    auto le32 = [](uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8)); };
    auto le16 = [](uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); };
    // WAVE_FORMAT_IEEE_FLOAT (3), 2 channels, 32 bits
    const uint32_t rate = f.rate ? (uint32_t)f.rate : 48000;
    const uint32_t data = (uint32_t)std::min<uint64_t>(f.bytes, 0xFFFFFFFFu - 36);
    uint8_t h[44];
    memcpy(h, "RIFF", 4); le32(h + 4, 36 + data); memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16); le16(h + 20, 3); le16(h + 22, 2); le32(h + 24, rate); le32(h + 28, rate * 8);
    le16(h + 32, 8); le16(h + 34, 32); memcpy(h + 36, "data", 4); le32(h + 40, data);
    auto pos = f.f->tellp();
    f.f->seekp(0);
    f.f->write((const char *)h, sizeof(h));
    if (pos > 44) f.f->seekp(pos);
    f.f->flush();
}

void Recorder::pcm(const float *data, size_t frames, int rate)
{
    std::lock_guard<std::mutex> lk(mutex_);
    File &f = files_[Wav];
    if (!f.f) return;
    if (f.rate && rate != f.rate) {   // a WAV file has one rate
        edi_log("record: sample rate changed, stopping the WAV recording");
        close(f, Wav);
        return;
    }
    f.rate = rate;
    f.f->write((const char *)data, (std::streamsize)(frames * 8));   // float stereo, little endian
    f.bytes += frames * 8;
    // keep the header up to date so the file is usable even if we never close it
    auto now = std::chrono::steady_clock::now();
    if (now - f.lastPatch > std::chrono::seconds(2)) {
        writeWavHeader(f);
        f.lastPatch = now;
    }
}

// ---------------------------------------------------------------------------
// Raw

void Recorder::rawMp2(const uint8_t *frame, size_t len)
{
    std::lock_guard<std::mutex> lk(mutex_);
    File &f = files_[Raw];
    if (!f.f || f.dabplus) return;
    f.f->write((const char *)frame, (std::streamsize)len);
    f.bytes += len;
}

namespace {
struct BitWriter {
    std::vector<uint8_t> b;
    int bits = 0;
    void put(uint32_t v, int n)
    {
        for (int i = n - 1; i >= 0; i--) {
            if (bits % 8 == 0) b.push_back(0);
            if ((v >> i) & 1) b.back() |= (uint8_t)(0x80 >> (bits % 8));
            bits++;
        }
    }
};
}   // namespace

// One AU as a LOAS frame (ISO/IEC 14496-3 §1.7): AudioSyncStream with an
// AudioMuxElement carrying the StreamMuxConfig every time (useSameStreamMux = 0)
void Recorder::rawAu(const uint8_t *au, size_t len, uint8_t sf)
{
    std::lock_guard<std::mutex> lk(mutex_);
    File &f = files_[Raw];
    if (!f.f || !f.dabplus || !au) return;

    const bool sbr = SF_SBR(sf), ps = SF_PS(sf), stereo = SF_STEREO(sf);
    const int outIndex = SF_DAC_RATE(sf) ? 3 : 5;                 // 48 / 32 kHz
    const int coreIndex = sbr ? outIndex + 3 : outIndex;          // 24 / 16 kHz with SBR
    const int chConfig = (stereo && !ps) ? 2 : 1;

    BitWriter m;
    m.put(0, 1);              // useSameStreamMux
    // StreamMuxConfig
    m.put(0, 1);              // audioMuxVersion
    m.put(1, 1);              // allStreamsSameTimeFraming
    m.put(0, 6);              // numSubFrames
    m.put(0, 4);              // numProgram
    m.put(0, 3);              // numLayer
    // AudioSpecificConfig: explicit SBR (5) / PS (29) signalling, then AAC LC with 960 transform
    if (sbr) {
        m.put(ps ? 29 : 5, 5);
        m.put((uint32_t)coreIndex, 4);
        m.put((uint32_t)chConfig, 4);
        m.put((uint32_t)outIndex, 4);   // extensionSamplingFrequencyIndex
        m.put(2, 5);                    // AAC LC
    } else {
        m.put(2, 5);
        m.put((uint32_t)coreIndex, 4);
        m.put((uint32_t)chConfig, 4);
    }
    m.put(1, 1);              // GASpecificConfig: frameLengthFlag (960)
    m.put(0, 1);              // dependsOnCoreCoder
    m.put(0, 1);              // extensionFlag
    m.put(0, 3);              // frameLengthType 0
    m.put(0xFF, 8);           // latmBufferFullness
    m.put(0, 1);              // otherDataPresent
    m.put(0, 1);              // crcCheckPresent
    // PayloadLengthInfo + PayloadMux
    size_t n = len;
    while (n >= 255) { m.put(255, 8); n -= 255; }
    m.put((uint32_t)n, 8);
    for (size_t i = 0; i < len; i++) m.put(au[i], 8);
    // (byte alignment: the last byte is already zero padded)

    const size_t muxLen = m.b.size();
    if (muxLen >= (1 << 13)) return;   // can't happen for DAB+ AUs
    uint8_t hdr[3] = {0x56, (uint8_t)(0xE0 | (muxLen >> 8)), (uint8_t)muxLen};   // 0x2B7 sync + length
    f.f->write((const char *)hdr, 3);
    f.f->write((const char *)m.b.data(), (std::streamsize)muxLen);
    f.bytes += 3 + muxLen;
}
