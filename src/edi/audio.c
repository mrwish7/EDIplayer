#include "audio.h"
#include "crc16.h"
#include <string.h>
#include "edi_log.h"
#include <stdio.h>

#define FPAD_LEN 2

void audio_init(audio_ctx_t *ctx, pad_cb_t cb, void *opaque)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->cb     = cb;
    ctx->opaque = opaque;
}

void audio_set_data_cb(audio_ctx_t *ctx, audio_data_cb_t cb, void *opaque)
{
    ctx->data_cb     = cb;
    ctx->data_opaque = opaque;
}

void audio_reset(audio_ctx_t *ctx)
{
    ctx->n_frames = 0;
}

const char *audio_fmt_name(audio_fmt_t fmt)
{
    switch (fmt) {
    case AUDIO_DABPLUS: return "DAB+";
    case AUDIO_MP2:     return "DAB (MP2)";
    default:            return "unknown";
    }
}

/* -------------------------------------------------------------------------
 * DAB+ superframe handling (ETSI TS 102 563 §5)
 * ---------------------------------------------------------------------- */

/*
 * Fire code CRC: generator x^16+x^14+x^13+x^12+x^11+x^5+x^3+x^2+x+1
 * (0x782F), initial value 0, no final inversion. Covers superframe
 * bytes 2..10; the result is stored in bytes 0-1.
 */
static uint16_t firecode_crc(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;
    while (len--) {
        crc ^= (uint16_t)(*data++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x782Fu)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}

static bool superframe_sync(const uint8_t *sf)
{
    /* An all-zero header passes the Fire code trivially — reject it */
    if (sf[3] == 0x00 && sf[4] == 0x00)
        return false;
    uint16_t stored = (uint16_t)((sf[0] << 8) | sf[1]);
    return firecode_crc(sf + 2, 9) == stored;
}

/*
 * Extract PAD from one AU. In DAB+ the PAD is carried in a DSE which, when
 * present, is the first syntax element of the AU (TS 102 563 §5.4.3):
 *
 *   byte 0: id_syn_ele (3 bits, DSE = 4) | element_instance_tag (4)
 *           | data_byte_align_flag (1)
 *   byte 1: count (8 bits)
 *   byte 2: esc_count (8 bits, only if count == 255)
 *   then count (+ esc_count) bytes: [X-PAD (reversed)] [F-PAD]
 */
static void au_check_pad(audio_ctx_t *ctx, const uint8_t *au, size_t au_len)
{
    if (au_len < 3 || (au[0] >> 5) != 4)
        return;

    size_t pad_start = 2;
    size_t pad_len   = au[1];
    if (pad_len == 255) {
        pad_len += au[2];
        pad_start++;
    }
    if (pad_len < FPAD_LEN || au_len < pad_start + pad_len)
        return;

    ctx->pad_frames++;
    ctx->cb(ctx->opaque, au + pad_start, pad_len - FPAD_LEN, true,
            au + pad_start + pad_len - FPAD_LEN);
}

static void process_superframe(audio_ctx_t *ctx)
{
    const uint8_t *sf = ctx->sf;
    size_t s         = ctx->frame_len / 24;  /* sub-channel index */
    size_t audio_len = 110 * s;              /* superframe minus RS parity */

    uint8_t dac_rate = (sf[2] >> 6) & 1;
    uint8_t sbr_flag = (sf[2] >> 5) & 1;

    int    num_aus;
    size_t au_start[7];

    if (dac_rate) num_aus = sbr_flag ? 3 : 6;
    else          num_aus = sbr_flag ? 2 : 4;

    /* AU start addresses: first is implied by header length, the rest are
     * 12-bit fields packed from byte 3 */
    switch (num_aus) {
    case 2:
        au_start[0] = 5;
        au_start[1] = (size_t)(sf[3] << 4 | sf[4] >> 4);
        break;
    case 3:
        au_start[0] = 6;
        au_start[1] = (size_t)(sf[3] << 4 | sf[4] >> 4);
        au_start[2] = (size_t)((sf[4] & 0x0F) << 8 | sf[5]);
        break;
    case 4:
        au_start[0] = 8;
        au_start[1] = (size_t)(sf[3] << 4 | sf[4] >> 4);
        au_start[2] = (size_t)((sf[4] & 0x0F) << 8 | sf[5]);
        au_start[3] = (size_t)(sf[6] << 4 | sf[7] >> 4);
        break;
    default: /* 6 */
        au_start[0] = 11;
        au_start[1] = (size_t)(sf[3] << 4 | sf[4] >> 4);
        au_start[2] = (size_t)((sf[4] & 0x0F) << 8 | sf[5]);
        au_start[3] = (size_t)(sf[6] << 4 | sf[7] >> 4);
        au_start[4] = (size_t)((sf[7] & 0x0F) << 8 | sf[8]);
        au_start[5] = (size_t)(sf[9] << 4 | sf[10] >> 4);
        break;
    }
    au_start[num_aus] = audio_len;

    ctx->superframes++;
    ctx->sf_format = sf[2];

    for (int i = 0; i < num_aus; i++) {
        size_t start = au_start[i];
        size_t end   = au_start[i + 1];
        if (end <= start + 2 || end > audio_len) {
            ctx->au_crc_err++;
            if (ctx->data_cb)
                ctx->data_cb(ctx->data_opaque, AUDIO_DABPLUS, sf[2], NULL, 0);
            continue;
        }

        const uint8_t *au     = sf + start;
        size_t         au_len = end - start - 2;
        uint16_t stored   = (uint16_t)((au[au_len] << 8) | au[au_len + 1]);
        uint16_t computed = crc16(0xFFFF, au, au_len) ^ 0xFFFF;
        if (stored != computed) {
            ctx->au_crc_err++;
            if (ctx->data_cb)
                ctx->data_cb(ctx->data_opaque, AUDIO_DABPLUS, sf[2], NULL, 0);
            continue;
        }

        ctx->au_ok++;
        if (ctx->data_cb)
            ctx->data_cb(ctx->data_opaque, AUDIO_DABPLUS, sf[2], au, au_len);
        au_check_pad(ctx, au, au_len);
    }
}

static void feed_dabplus(audio_ctx_t *ctx, const uint8_t *mst, size_t len)
{
    memcpy(ctx->sf + (size_t)ctx->n_frames * len, mst, len);
    ctx->n_frames++;
    if (ctx->n_frames < 5)
        return;

    if (superframe_sync(ctx->sf)) {
        if (!ctx->synced)
            edi_log("audio: DAB+ superframe sync acquired\n");
        ctx->synced = true;
        ctx->fmt    = AUDIO_DABPLUS;
        process_superframe(ctx);
        ctx->n_frames = 0;
    } else {
        if (ctx->synced)
            edi_log("audio: DAB+ superframe sync lost\n");
        ctx->synced = false;
        /* Slide the window along by one logical frame and try again */
        memmove(ctx->sf, ctx->sf + len, 4 * len);
        ctx->n_frames = 4;
    }
}

/* -------------------------------------------------------------------------
 * DAB MPEG-1 Layer II handling (ETSI EN 300 401 §7.4)
 * ---------------------------------------------------------------------- */

static const uint16_t mp2_bitrates[16] = {
    0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0
};

/* MPEG-1 Layer II, 48 kHz header check. 24 kHz (LSF) frames span two
 * logical frames and are not handled. */
static bool mp2_header_ok(const uint8_t *f, size_t len)
{
    if (len < 8) return false;
    if (f[0] != 0xFF || (f[1] & 0xFE) != 0xFC) return false;
    uint8_t br_idx = f[2] >> 4;
    if (br_idx == 0 || br_idx == 15) return false;
    if (((f[2] >> 2) & 0x03) != 1) return false;  /* 48 kHz */
    return true;
}

static void feed_mp2(audio_ctx_t *ctx, const uint8_t *f, size_t len)
{
    if (ctx->data_cb)
        ctx->data_cb(ctx->data_opaque, AUDIO_MP2, 0, f, len);

    uint16_t bitrate = mp2_bitrates[f[2] >> 4];
    bool     mono    = (f[3] >> 6) == 3;

    /* ScF-CRC is 2 bytes at low bitrates, otherwise 4 (as dablin) */
    size_t scf_crc_len = bitrate < (mono ? 56 : 112) ? 2 : 4;
    size_t hdr_len     = 4;
    if (len < hdr_len + scf_crc_len + FPAD_LEN)
        return;

    /* X-PAD length isn't known up front; pass everything before the
     * ScF-CRC and let the CI list delimit it */
    size_t xpad_len = len - hdr_len - scf_crc_len - FPAD_LEN;
    ctx->pad_frames++;
    ctx->cb(ctx->opaque, f + hdr_len, xpad_len, false, f + len - FPAD_LEN);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

void audio_feed(audio_ctx_t *ctx, const uint8_t *mst, size_t len)
{
    if (len == 0 || len > MAX_MST_LEN)
        return;

    if (len != ctx->frame_len) {
        if (ctx->frame_len)
            edi_log("audio: sub-channel size changed %zu → %zu bytes\n",
                    ctx->frame_len, len);
        ctx->frame_len = len;
        ctx->n_frames  = 0;
        ctx->synced    = false;
        ctx->mp2_hits  = 0;
        ctx->fmt       = AUDIO_UNKNOWN;
    }

    /* MP2 frames start with a sync word in every logical frame; require
     * a few in a row before committing. DAB+ superframe sync wins. */
    if (ctx->fmt != AUDIO_DABPLUS) {
        if (mp2_header_ok(mst, len)) {
            if (++ctx->mp2_hits >= 3) {
                if (ctx->fmt != AUDIO_MP2)
                    edi_log("audio: DAB (MPEG-1 Layer II) stream detected\n");
                ctx->fmt = AUDIO_MP2;
                feed_mp2(ctx, mst, len);
                return;
            }
        } else {
            ctx->mp2_hits = 0;
            if (ctx->fmt == AUDIO_MP2)
                ctx->fmt = AUDIO_UNKNOWN;
        }
    }

    if (len % 24 == 0)
        feed_dabplus(ctx, mst, len);
}
