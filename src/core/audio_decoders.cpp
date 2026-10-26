#include "audio_decoders.h"
#include "edi/edi_log.h"
extern "C" {
#include "edi/audio.h"
}

#include "aac_decoder/libfaad/neaacdec.h"
#include "mp2_decoder/mp2_decoder.h"

#include <cstring>

// ---------------------------------------------------------------------------
// DAB+

DabPlusDecoder::DabPlusDecoder() : dec_(new NeaacDecoder) {}

DabPlusDecoder::~DabPlusDecoder() { reset(); }

void DabPlusDecoder::reset()
{
    if (handle_) dec_->NeAACDecClose(handle_);
    handle_ = nullptr;
    format_ = -1;
    rate_ = 0;
    lastFrames_ = 0;
    errors_ = 0;
}

bool DabPlusDecoder::configure(uint8_t sf)
{
    reset();
    sbr_ = SF_SBR(sf);
    ps_ = SF_PS(sf);
    stereo_ = SF_STEREO(sf);
    rate_ = SF_DAC_RATE(sf) ? 48000 : 32000;

    handle_ = dec_->NeAACDecOpen();
    if (!handle_) return false;
    NeAACDecConfigurationPtr cfg = dec_->NeAACDecGetCurrentConfiguration(handle_);
    cfg->outputFormat = FAAD_FMT_16BIT;
    dec_->NeAACDecSetConfiguration(handle_, cfg);

    // AudioSpecificConfig, as dablin builds it - the only way to select the
    // 960-sample transform:
    //   00010  AudioObjectType 2 (AAC LC)
    //   xxxx   core sampling frequency index (24/48/16/32 kHz)
    //   xxxx   channel configuration
    //   100    GASpecificConfig: frameLengthFlag = 1 (960)
    // SBR and PS use implicit signalling: libfaad assumes SBR for a core
    // rate <= 24 kHz, and always outputs stereo when PS support is built in.
    const int srIndex = SF_DAC_RATE(sf) ? (sbr_ ? 6 : 3) : (sbr_ ? 8 : 5);
    const int chConfig = stereo_ ? 2 : 1;
    uint8_t asc[2];
    asc[0] = (uint8_t)((2 << 3) | (srIndex >> 1));
    asc[1] = (uint8_t)(((srIndex & 1) << 7) | (chConfig << 3) | 0x04);

    uint32_t rate = 0;
    uint8_t ch = 0;
    if (dec_->NeAACDecInit2(handle_, asc, sizeof(asc), &rate, &ch) != 0) {
        edi_log("aac: decoder init failed (format 0x%02X)", sf);
        dec_->NeAACDecClose(handle_);
        handle_ = nullptr;
        return false;
    }
    format_ = sf;
    edi_log("aac: %s", describe().c_str());
    return true;
}

bool DabPlusDecoder::decode(const uint8_t *au, size_t len, uint8_t sf, std::vector<int16_t> &out, int &rate)
{
    if ((int)sf != format_ && !configure(sf)) return false;
    rate = rate_;

    if (au) {
        NeAACDecFrameInfo info;
        memset(&info, 0, sizeof(info));
        int16_t pcm[2048 * 2 * 2];
        void *buf = pcm;
        dec_->NeAACDecDecode2(handle_, &info, const_cast<uint8_t *>(au), (uint32_t)len, &buf, sizeof(pcm));
        if (info.error == 0 && info.samples > 0 && info.channels > 0) {
            const size_t frames = info.samples / info.channels;
            if (info.samplerate) rate = rate_ = (int)info.samplerate;
            lastFrames_ = frames;
            size_t o = out.size();
            out.resize(o + frames * 2);
            for (size_t i = 0; i < frames; i++) {
                out[o + i * 2] = pcm[i * info.channels];
                out[o + i * 2 + 1] = pcm[i * info.channels + (info.channels > 1 ? 1 : 0)];
            }
            return true;
        }
        if (info.error && errors_++ < 5)
            edi_log("aac: decode error %u: %s", info.error, dec_->NeAACDecGetErrorMessage(info.error).text);
    }
    // lost or undecodable AU: keep timing with silence
    if (lastFrames_) out.resize(out.size() + lastFrames_ * 2, 0);
    return false;
}

std::string DabPlusDecoder::describe() const
{
    if (format_ < 0) return "";
    std::string s = ps_ ? "HE-AAC v2" : sbr_ ? "HE-AAC" : "AAC-LC";
    s += ", " + std::to_string(rate_ / 1000) + " kHz";
    s += stereo_ || ps_ ? ", stereo" : ", mono";
    if (ps_) s += " (PS)";
    return s;
}

// ---------------------------------------------------------------------------
// DAB (MP2)

Mp2FrameDecoder::Mp2FrameDecoder() : audio_(new Audio), dec_(new MP2Decoder(*audio_))
{
    dec_->init();
}

Mp2FrameDecoder::~Mp2FrameDecoder() = default;

void Mp2FrameDecoder::reset()
{
    dec_->clear();
    rate_ = channels_ = bitrate_ = 0;
}

bool Mp2FrameDecoder::decode(const uint8_t *frame, size_t len, std::vector<int16_t> &out, int &rate)
{
    // libmad wants MAD_BUFFER_GUARD bytes after the last frame
    in_.assign(frame, frame + len);
    in_.resize(len + MAD_BUFFER_GUARD, 0);
    pcm_.resize(1152 * 2);

    int32_t left = (int32_t)in_.size();
    int32_t r = dec_->decode(in_.data(), &left, pcm_.data());
    if (r != MP2Decoder::MP2_NONE) return false;

    const size_t frames = dec_->getOutputSamples();
    rate = rate_ = (int)dec_->getSampleRate();
    channels_ = dec_->getChannels();
    bitrate_ = (int)dec_->getBitRate();
    size_t o = out.size();
    out.resize(o + frames * 2);
    for (size_t i = 0; i < frames * 2; i++)
        out[o + i] = (int16_t)(pcm_[i] >> 16);
    return true;
}

std::string Mp2FrameDecoder::describe() const
{
    if (!rate_) return "";
    return "MPEG-1 Layer II, " + std::to_string(rate_ / 1000) + " kHz, " +
           (channels_ == 2 ? "stereo" : "mono");
}
