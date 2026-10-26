/*
 * mp2_decoder.cpp
 * MPEG-1 Layer II (MP2) decoder based on a stripped-down libmad (Layer II only,
 * see libmad/frame.cpp for why Layer III is intentionally not linked in).
 */
#include "mp2_decoder.h"

// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
bool MP2Decoder::init() {
    m_stream.alloc("m_stream");
    m_frame.alloc("m_frame");
    m_synth.alloc("m_synth");

    if (!m_stream.valid() || !m_frame.valid() || !m_synth.valid()) {
        reset();
        MP2_LOG_ERROR("not enough memory to allocate mp2decoder buffers");
        return false;
    }
    clear();
    return true;
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void MP2Decoder::reset() {
    m_stream.reset();
    m_frame.reset();
    m_synth.reset();
    m_arg1_str.reset();
    m_haveFrame = false;
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void MP2Decoder::clear() {
    /* important to do this - libmad's own init() functions additionally expect
     * to run on top, but they only ever set fields to zero/null anyway */
    m_stream.clear();
    m_frame.clear();
    m_synth.clear();
    if (m_stream.valid()) mad_stream_init(m_stream.get());
    if (m_frame.valid()) mad_frame_init(m_frame.get());
    if (m_synth.valid()) mad_synth_init(m_synth.get());
    m_haveFrame = false;
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
bool MP2Decoder::isValid() {
    if (!m_stream.valid() || !m_frame.valid() || !m_synth.valid()) { return false; }
    return true;
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
/* B I T S T R E A M   S Y N C */
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
int32_t MP2Decoder::findSyncWord(uint8_t* buf, int32_t nBytes) {
    for (int32_t i = 0; i <= nBytes - 4; i++) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) continue;

        uint8_t mpegVerBits = (buf[i + 1] >> 3) & 0x03; // 00=MPEG2.5, 01=reserved, 10=MPEG2, 11=MPEG1
        uint8_t layer       = (buf[i + 1] >> 1) & 0x03; // 01=Layer III, 10=Layer II, 11=Layer I, 00=reserved
        uint8_t brIdx       = (buf[i + 2] >> 4) & 0x0F;
        uint8_t srIdx       = (buf[i + 2] >> 2) & 0x03;

        if (mpegVerBits == 1) continue; // reserved version
        if (layer != 2) continue;       // must be Layer II specifically (MP2)
        if (brIdx == 15) continue;      // forbidden bitrate index
        if (srIdx == 3) continue;       // reserved samplerate index

        return i;
    }
    return -1;
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
/* D E C O D E */
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
int32_t MP2Decoder::decode(uint8_t* inbuf, int32_t* bytesLeft, int32_t* outbuf) {

    if (*bytesLeft < MAD_BUFFER_GUARD) return MP2_NEXT_FRAME; // not enough data to even attempt a header parse

    mad_stream_buffer(m_stream.get(), inbuf, (unsigned long)*bytesLeft);

    if (mad_frame_decode(m_frame.get(), m_stream.get()) == -1) {
        if (m_stream->error == MAD_ERROR_BUFLEN) {
            return MP2_NEXT_FRAME; // not enough data buffered for a full frame yet, consume nothing
        }
        if (MAD_RECOVERABLE(m_stream->error)) {
            int32_t consumed = (int32_t)(m_stream->next_frame - inbuf);
            *bytesLeft -= (consumed > 0 ? consumed : 1);
            MP2_LOG_DEBUG("MP2 recoverable error: {}", mad_stream_errorstr(m_stream.get()));
            return MP2_ERR;
        }
        MP2_LOG_ERROR("MP2 unrecoverable error: {}", mad_stream_errorstr(m_stream.get()));
        *bytesLeft = 0;
        return MP2_STOP;
    }

    mad_synth_frame(m_synth.get(), m_frame.get());
    m_haveFrame = true;

    int32_t consumed = (int32_t)(m_stream->next_frame - inbuf);
    *bytesLeft -= consumed;

    struct mad_pcm* pcm = &m_synth->pcm;
    int              nch = pcm->channels;
    int              n = pcm->length;

    // EDIplayer: hand back libmad's fixed-point samples (Q3.28, MAD_F_ONE = full scale)
    // unclipped and at full precision; the caller converts to float. Rounding to 16 bits
    // here clipped the overs of loud broadcasts (MP2_scaleToInt16).
    for (int i = n - 1; i >= 0; i--) {
        mad_fixed_t l = pcm->samples[0][i];
        mad_fixed_t r = (nch == 2) ? pcm->samples[1][i] : l;
        outbuf[i * 2]     = (int32_t)l;
        outbuf[i * 2 + 1] = (int32_t)r;
    }

    return MP2_NONE;
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
/* A C C E S S O R S */
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
uint8_t MP2Decoder::getChannels() {
    if (!m_haveFrame) return 2;
    return MAD_NCHANNELS(&m_frame->header);
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
uint32_t MP2Decoder::getSampleRate() {
    if (!m_haveFrame) return 0;
    return m_frame->header.samplerate;
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
uint32_t MP2Decoder::getOutputSamples() {
    if (!m_haveFrame) return 0;
    return m_synth->pcm.length; // samples per channel
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
uint8_t MP2Decoder::getBitsPerSample() { return 16; }
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
uint32_t MP2Decoder::getBitRate() {
    if (!m_haveFrame) return 0;
    return m_frame->header.bitrate;
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
uint32_t MP2Decoder::getAudioDataStart() { return 0; }
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
uint32_t MP2Decoder::getAudioFileDuration() { return 0; }
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
const char* MP2Decoder::getStreamTitle() { return nullptr; } // nothing todo
const char* MP2Decoder::whoIsIt() { return "MP2"; }
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void MP2Decoder::setRawBlockParams(uint8_t, uint32_t, uint8_t, uint32_t, uint32_t) { /* not used by MP2 */ }
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
std::vector<uint32_t> MP2Decoder::getMetadataBlockPicture() { return {}; }
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
const char* MP2Decoder::arg1() {
    if (!m_haveFrame) return "MPEG-1 Layer II";
    bool isLSF = (m_frame->header.flags & MAD_FLAG_LSF_EXT) != 0;
    m_arg1_str.assign(isLSF ? "MPEG-2 Layer II" : "MPEG-1 Layer II");
    return m_arg1_str.get();
}
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
const char* MP2Decoder::arg2() { return ""; }
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
int32_t MP2Decoder::val1() { return 0; }
// —————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
int32_t MP2Decoder::val2() { return 0; }
