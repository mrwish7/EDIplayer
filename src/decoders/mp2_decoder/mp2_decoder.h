// MPEG-1 Layer II (MP2) decoder, based on a stripped-down libmad (Layer III/MP3
// intentionally omitted - see libmad/frame.cpp - MP3 continues to be handled by
// the existing Helix-based MP3Decoder).
#pragma once

#include "../Audio.h"
#include "libmad/stream.h"
#include "libmad/frame.h"
#include "libmad/layer12.h"
#include "libmad/synth.h"

class MP2Decoder : public Decoder {

  public:
    MP2Decoder(Audio& audioRef) : Decoder(audioRef), audio(audioRef) {}
    ~MP2Decoder() { reset(); }

    bool                  init() override;
    void                  clear() override;
    void                  reset() override;
    bool                  isValid() override;
    int32_t               findSyncWord(uint8_t* buf, int32_t nBytes) override;
    uint8_t               getChannels() override;
    uint32_t              getSampleRate() override;
    uint32_t              getOutputSamples() override;
    uint8_t               getBitsPerSample() override;
    uint32_t              getBitRate() override;
    uint32_t              getAudioDataStart() override;
    uint32_t              getAudioFileDuration() override;
    const char*           getStreamTitle() override;
    const char*           whoIsIt() override;
    int32_t               decode(uint8_t* inbuf, int32_t* bytesLeft, int32_t* outbuf) override;
    void                  setRawBlockParams(uint8_t channels, uint32_t sampleRate, uint8_t BPS, uint32_t tsis, uint32_t AuDaLength) override;
    std::vector<uint32_t> getMetadataBlockPicture() override;
    const char*           arg1() override; // MPEG version + "Layer II"
    const char*           arg2() override;
    int32_t               val1() override;
    int32_t               val2() override;

    enum {
        MP2_NONE = 0,
        MP2_ERR = -1,
        MP2_STOP = -100,
        MP2_NEXT_FRAME = 100, // need more data, nothing decoded yet
    };

  private:
    Audio& audio;

    ps_ptr<mad_stream> m_stream;
    ps_ptr<mad_frame>  m_frame;
    ps_ptr<mad_synth>  m_synth;
    ps_ptr<char>       m_arg1_str;

    bool m_haveFrame = false; // true once at least one frame has been decoded

// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
// Macro for comfortable calls
#define MP2_LOG_ERROR(fmt, ...)   Audio::AUDIO_LOG_IMPL(1, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define MP2_LOG_WARN(fmt, ...)    Audio::AUDIO_LOG_IMPL(2, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define MP2_LOG_INFO(fmt, ...)    Audio::AUDIO_LOG_IMPL(3, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define MP2_LOG_DEBUG(fmt, ...)   Audio::AUDIO_LOG_IMPL(4, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define MP2_LOG_VERBOSE(fmt, ...) Audio::AUDIO_LOG_IMPL(5, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
};
