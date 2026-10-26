#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dls.h"
#include "mot.h"

/*
 * PAD (Programme Associated Data) demultiplexer — ETSI EN 300 401 §7.4.
 *
 * F-PAD (2 bytes) signals whether X-PAD is present (short = 4 bytes,
 * or variable size) and whether a Contents Indicator (CI) list leads it.
 * The CI list names the application type and length of each X-PAD data
 * subfield; without a CI the X-PAD continues the previous application.
 *
 * Application types handled:
 *    1      Data Group Length Indicator (precedes each MOT data group)
 *    2 / 3  Dynamic Label, start / continuation
 *   12 / 13 MOT (Slideshow), start / continuation
 */

#define PAD_MAX_XPAD   512
#define PAD_MAX_MOT_DG 16384   /* DGLI is 14 bits */

#define XPAD_APP_DGLI     1
#define XPAD_APP_DL_START 2
#define XPAD_APP_DL_CONT  3
#define XPAD_APP_MOT_START 12
#define XPAD_APP_MOT_CONT  13

typedef struct {
    dls_ctx_t *dls;
    mot_ctx_t *mot;

    /* Previous X-PAD, for continuation without CI */
    int        last_app;        /* continuation app type, -1 = none */
    size_t     last_len;

    /* Data Group Length Indicator */
    uint8_t    dgli[4];
    size_t     dgli_got;
    size_t     dgli_len;        /* 0 = none pending */

    /* MOT data group assembly */
    uint8_t    mot_dg[PAD_MAX_MOT_DG];
    size_t     mot_len;
    size_t     mot_need;
    bool       mot_active;
} pad_ctx_t;

void pad_init(pad_ctx_t *ctx, dls_ctx_t *dls, mot_ctx_t *mot);

/* PAD callback for the audio layer (see audio.h pad_cb_t).
 * opaque must be a pad_ctx_t*. Returns the X-PAD bytes in use. */
size_t pad_process(void *opaque, const uint8_t *xpad, size_t xpad_len,
                   bool exact_xpad_len, const uint8_t *fpad);
