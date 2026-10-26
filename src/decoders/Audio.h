// Desktop stand-in for ESP32-audioI2S's Audio.h (from UecpBridge; log output goes to edi_log()).
//
// aac_decoder.h ("../Audio.h"), aac_decoder/libfaad/aac_defines.h ("../../Audio.h") and
// mp2_decoder.h ("../Audio.h") all pull in the real Audio.h purely to get two things:
// the abstract Decoder base class, and the static Audio::AUDIO_LOG_IMPL(...) logging
// helper the AAC_LOG_*/MP2_LOG_* macros expand to. Neither decoder .cpp dereferences
// any other Audio member, so this header intentionally provides only those two pieces
// instead of the full WiFi/SD/I2S-laden original.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "psram_unique_ptr.hpp"
#include "edi/edi_log.h"

class Audio {
  public:
    // fmt uses ps_ptr<char>::appendf's "{}" placeholder style (see AAC_LOG_ERROR("{}", text) call
    // sites), NOT printf's "%s" - matching the real Audio::AUDIO_LOG_IMPL, which builds its message
    // the same way via logStr.appendf(fmt, ...). A naive std::fprintf(stderr, fmt, ...) here would
    // silently drop every argument, since "{}" isn't a printf conversion specifier.
    template <typename... Args>
    static void AUDIO_LOG_IMPL(uint8_t level, const char* path, int line, const char* func, const char* fmt, Args&&... args) {
        static const char* const tag[6] = {"", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE"};
        const char* base = path;
        for (const char* p = path; *p; ++p) {
            if (*p == '/' || *p == '\\') base = p + 1;
        }
        ps_ptr<char> msg;
        msg.appendf(fmt, std::forward<Args>(args)...);
        edi_log("[%s] %s:%d %s] %s", level <= 5 ? tag[level] : "", base, line, func ? func : "", msg.c_get() ? msg.c_get() : "");
    }
};

class Decoder {
  public:
    virtual ~Decoder() = default;
    virtual bool                  init() = 0;
    virtual void                  clear() = 0;
    virtual void                  reset() = 0;
    virtual bool                  isValid() = 0;
    virtual int32_t               findSyncWord(uint8_t* buf, int32_t nBytes) = 0;
    virtual uint8_t               getChannels() = 0;
    virtual uint32_t              getSampleRate() = 0;
    virtual uint8_t               getBitsPerSample() = 0;
    virtual uint32_t              getBitRate() = 0;
    virtual uint32_t              getAudioDataStart() = 0;
    virtual uint32_t              getAudioFileDuration() = 0;
    virtual uint32_t              getOutputSamples() = 0;
    virtual int32_t                decode(uint8_t* inbuf, int32_t* bytesLeft, int32_t* outbuf1) = 0;
    virtual void                  setRawBlockParams(uint8_t param1, uint32_t param2, uint8_t param3, uint32_t param4, uint32_t param5) = 0;
    virtual const char*           getStreamTitle() { return nullptr; }
    virtual const char*           whoIsIt() { return nullptr; }
    virtual bool                  isLatm() { return false; } // true only for AACDecoder when the current stream is LOAS/LATM framed
    virtual std::vector<uint32_t> getMetadataBlockPicture() = 0;
    virtual const char*           arg1() = 0; // decoder specific
    virtual const char*           arg2() = 0; // decoder specific
    virtual int32_t               val1() = 0; // decoder specific
    virtual int32_t               val2() = 0; // decoder specific

  protected:
    Decoder(Audio& audioRef) : audio(audioRef) {}
    Audio& audio;

  private:
    Decoder() = delete;
};
