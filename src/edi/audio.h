#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "tags.h"

/*
 * Audio sub-channel framing — locates the PAD carried in each audio frame.
 *
 * DAB+ (ETSI TS 102 563):
 *   Five consecutive 24 ms logical frames form one 120 ms audio superframe
 *   (RS(120,110) protected). The superframe start is found by checking the
 *   Fire code CRC in the first 11 bytes. Each superframe holds 2, 3, 4 or 6
 *   AAC access units (AUs); PAD is carried in a Data Stream Element (DSE)
 *   at the start of each AU.
 *
 *   Reed-Solomon correction is not applied — over EDI the payload is
 *   normally error-free, and each AU is still CRC-checked before use.
 *
 * DAB (MPEG-1 Layer II, 48 kHz, ETSI EN 300 401 §7.4):
 *   One audio frame per 24 ms logical frame. PAD sits at the end of the
 *   frame: [X-PAD (reversed)] [ScF-CRC] [F-PAD (2 bytes)].
 *
 * In both cases, the PAD callback receives the X-PAD bytes in transmitted
 * (reversed) order plus the 2-byte F-PAD.
 *
 * The optional data callback receives the coded audio for a decoder:
 *   DAB+: one AAC access unit (CRC removed) per call, plus the superframe
 *         format byte; data is NULL when the AU failed its CRC, so the
 *         decoder can keep timing by inserting silence.
 *   MP2:  one complete MPEG audio frame (the whole logical frame).
 */

/* Returns the number of X-PAD bytes actually in use (CI list + subfields) */
typedef size_t (*pad_cb_t)(void *opaque,
                           const uint8_t *xpad, size_t xpad_len,
                           bool exact_xpad_len, const uint8_t *fpad);

typedef enum {
    AUDIO_UNKNOWN = 0,
    AUDIO_DABPLUS,
    AUDIO_MP2,
} audio_fmt_t;

/*
 * DAB+ superframe format byte (TS 102 563 §5.2, byte 2 of the superframe):
 *   rfa (1) | dac_rate (1) | sbr_flag (1) | aac_channel_mode (1) |
 *   ps_flag (1) | mpeg_surround_config (3)
 */
#define SF_DAC_RATE(b)  (((b) >> 6) & 1)   /* 1 = 48 kHz, 0 = 32 kHz */
#define SF_SBR(b)       (((b) >> 5) & 1)
#define SF_STEREO(b)    (((b) >> 4) & 1)
#define SF_PS(b)        (((b) >> 3) & 1)

typedef void (*audio_data_cb_t)(void *opaque, audio_fmt_t fmt, uint8_t sf_format,
                                const uint8_t *data, size_t len);

typedef struct {
    uint8_t     sf[5 * MAX_MST_LEN];  /* superframe assembly buffer */
    size_t      frame_len;            /* bytes per logical frame (MST size) */
    int         n_frames;             /* frames currently buffered (0..5) */
    audio_fmt_t fmt;
    bool        synced;               /* DAB+ superframe sync held */
    int         mp2_hits;             /* consecutive frames with MP2 header */

    pad_cb_t    cb;
    void       *opaque;
    audio_data_cb_t data_cb;          /* optional, see above */
    void       *data_opaque;
    uint8_t     sf_format;            /* last DAB+ superframe format byte */

    /* Statistics */
    uint32_t    superframes;
    uint32_t    au_ok;
    uint32_t    au_crc_err;
    uint32_t    pad_frames;           /* AUs / MP2 frames carrying PAD */

    /* Byte counters for the bitrate breakdown (cumulative):
     *   DAB+: audio = AU bytes excluding the PAD DSE; pad = DSE bytes
     *   MP2:  pad = F-PAD + X-PAD in use; audio = rest of the frame */
    uint64_t    audio_bytes;
    uint64_t    pad_bytes;
} audio_ctx_t;

void audio_init(audio_ctx_t *ctx, pad_cb_t cb, void *opaque);

/* Also deliver the coded audio (AUs / MP2 frames) to cb. */
void audio_set_data_cb(audio_ctx_t *ctx, audio_data_cb_t cb, void *opaque);

/* Discard partially assembled superframe (call on frame discontinuity). */
void audio_reset(audio_ctx_t *ctx);

/* Feed one logical frame of sub-channel data (the EST MST bytes). */
void audio_feed(audio_ctx_t *ctx, const uint8_t *mst, size_t len);

const char *audio_fmt_name(audio_fmt_t fmt);
