// edi-probe - command line test tool for the EDIplayer engine.
//
//   edi-probe <source> [--sid <hex>] [--seconds <n>] [--wav <file>] [--slides <dir>]
//             [--spi <seconds>] [--spi-dir <dir>] [--record <dir>]
//
// Lists the services in the multiplex. With --sid it plays that service for
// --seconds (default 20), optionally writing the decoded audio to a WAV file
// and the slideshow images into a directory, and prints DLS / DL Plus.
// With --spi it follows the SPI carousel for that long, saving the files
// under <spi-dir>/EDIplayer_SPI (default: current directory). With --record
// (and --sid) it records the service raw and as WAV into <dir>/EDIplayer_audio.

#include "core/player.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

class WavSink : public PcmSink {
public:
    FILE *f = nullptr;
    int rate = 0;
    uint64_t frames = 0;

    void pcm(const float *data, size_t n, int r) override
    {
        rate = r;
        frames += n;
        if (f) fwrite(data, sizeof(float), n * 2, f);
    }
    void flush() override {}

    void open(const char *path)
    {
        f = fopen(path, "wb");
        if (f) fseek(f, 44, SEEK_SET);
    }
    void close()
    {
        if (!f) return;
        auto le32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) fputc((v >> (i * 8)) & 0xFF, f); };
        auto le16 = [&](uint16_t v) { fputc(v & 0xFF, f); fputc(v >> 8, f); };
        uint32_t bytes = (uint32_t)(frames * 8);   // 32-bit float stereo
        fseek(f, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, f); le32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
        le32(16); le16(3); le16(2); le32((uint32_t)rate); le32((uint32_t)rate * 8); le16(8); le16(32);
        fwrite("data", 1, 4, f); le32(bytes);
        fclose(f);
        f = nullptr;
    }
};

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <source> [--sid <hex>] [--seconds <n>] [--wav <file>] [--slides <dir>]\n", argv[0]);
        return 1;
    }
    std::string url = argv[1];
    int sid = -1, seconds = 20, spiSeconds = 0;
    const char *wav = nullptr, *slides = nullptr, *spiDir = ".", *recordDir = nullptr;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--sid")) sid = (int)strtol(argv[i + 1], nullptr, 16);
        else if (!strcmp(argv[i], "--seconds")) seconds = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--wav")) wav = argv[i + 1];
        else if (!strcmp(argv[i], "--slides")) slides = argv[i + 1];
        else if (!strcmp(argv[i], "--spi")) spiSeconds = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--spi-dir")) spiDir = argv[i + 1];
        else if (!strcmp(argv[i], "--record")) recordDir = argv[i + 1];
    }

    WavSink sink;
    if (wav) sink.open(wav);
    Player player(&sink);
    player.setSpiBaseDir(spiDir);
    std::string err;
    if (!player.start(url, err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    // wait for the service list
    PlayerSnapshot s;
    for (int i = 0; i < 40; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        s = player.snapshot();
        if (!s.services.empty() && !s.ensemble.empty() && i >= 12) break;
    }
    printf("Ensemble 0x%04X \"%s\"  (%llu frames)\n", s.eid, s.ensemble.c_str(), (unsigned long long)s.afFrames);
    for (const auto &e : s.services)
        printf("  SId %04X  SCID %2d  %-5s %3d kbit/s  %-7s %-16s \"%s\"\n", e.sid, e.scid, e.dabplus ? "DAB+" : "DAB",
               e.kbps, e.protection.c_str(), e.pty >= 0 ? Player::ptyName(e.pty) : "?", e.label.c_str());

    if (spiSeconds > 0) {
        auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(spiSeconds)) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            s = player.snapshot();
            printf("SPI: %d carousel(s)  %d / %d objects%s\n", s.spi.carousels, s.spi.complete, s.spi.objects,
                   s.spi.directory ? "" : "  (no directory yet)");
            fflush(stdout);
            if (s.spi.directory && s.spi.objects && s.spi.complete == s.spi.objects) break;
        }
        printf("SPI folder: %s\n", s.spiFolder.c_str());
        for (const auto &l : s.logos) {
            int w1 = 0, h1 = 0, w2 = 0, h2 = 0;
            if (l.second.logo32) imageSize(l.second.logo32->data(), l.second.logo32->size(), w1, h1);
            if (l.second.logo320) imageSize(l.second.logo320->data(), l.second.logo320->size(), w2, h2);
            printf("  logos SId %04X  small %dx%d  large %dx%d\n", l.first, w1, h1, w2, h2);
        }
    }
    if (sid < 0) return s.services.empty() ? 2 : 0;

    player.select(sid);
    if (recordDir) {
        // wait until the service is selected, then record raw + wav for the rest of the run
        player.setRecordBaseDir(recordDir);
        for (int i = 0; i < 50 && player.snapshot().selectedSid != sid; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::string err;
        if (!player.startRecording(Recorder::Raw, err) || !player.startRecording(Recorder::Wav, err))
            fprintf(stderr, "record: %s\n", err.c_str());
    }
    uint64_t dlsV = 0, slideV = 0;
    int nSlides = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(seconds)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        s = player.snapshot();
        if (s.dlsVersion != dlsV) {
            dlsV = s.dlsVersion;
            if (!s.dls.empty()) {
                printf("DLS  \"%s\"\n", s.dls.c_str());
                for (const auto &d : s.dlplus) printf("  DL+ %-10s \"%s\"\n", Player::dlPlusTypeName(d.type), d.text.c_str());
            }
        }
        if (s.slideVersion != slideV) {
            slideV = s.slideVersion;
            if (s.slide) {
                printf("SLS  %s %zu bytes \"%s\"\n", s.slide->ext.c_str(), s.slide->data.size(), s.slide->name.c_str());
                if (slides) {
                    std::string p = std::string(slides) + "/slide" + std::to_string(++nSlides) + "." + s.slide->ext;
                    if (FILE *f = fopen(p.c_str(), "wb")) {
                        fwrite(s.slide->data.data(), 1, s.slide->data.size(), f);
                        fclose(f);
                    }
                }
            }
        }
    }
    s = player.snapshot();
    player.stop();
    sink.close();
    const double secs = sink.rate ? (double)sink.frames / sink.rate : 0;
    printf("Audio: %s  decoded %.1f s  AU ok %u  AU errors %u  gaps %u\n", s.audioFormat.c_str(), secs, s.auOk,
           s.auErrors, s.frameGaps);
    if (s.rates.valid)
        printf("Rates: sub-channel %.1f  useful %.1f  audio %.1f  PAD %.1f  overhead %.1f kbit/s\n", s.rates.subchannel,
               s.rates.useful, s.rates.audio, s.rates.pad, s.rates.overhead);
    return secs > seconds * 0.5 ? 0 : 3;
}
