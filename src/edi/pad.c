#include "pad.h"
#include "crc16.h"
#include <string.h>

/* Variable size X-PAD subfield lengths, indexed by CI length indicator */
static const size_t xpad_lens[8] = { 4, 6, 8, 12, 16, 24, 32, 48 };

typedef struct {
    int    app;
    size_t len;
    bool   start;
} subfield_t;

void pad_init(pad_ctx_t *ctx, dls_ctx_t *dls, mot_ctx_t *mot)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->dls      = dls;
    ctx->mot      = mot;
    ctx->last_app = -1;
}

static int continued_app(int app)
{
    switch (app) {
    case XPAD_APP_DL_START:  return XPAD_APP_DL_CONT;
    case XPAD_APP_MOT_START: return XPAD_APP_MOT_CONT;
    default:                 return app;
    }
}

/*
 * Data Group Length Indicator (EN 300 401 §7.4.5.1.1):
 *   rfa (2) | length (14), then CRC-16 over those 2 bytes.
 * Valid only for the immediately following MOT data group.
 */
static void dgli_feed(pad_ctx_t *ctx, bool start, const uint8_t *d, size_t len)
{
    if (start)
        ctx->dgli_got = 0;
    for (size_t i = 0; i < len && ctx->dgli_got < 4; i++)
        ctx->dgli[ctx->dgli_got++] = d[i];
    if (ctx->dgli_got < 4)
        return;

    uint16_t stored   = (uint16_t)((ctx->dgli[2] << 8) | ctx->dgli[3]);
    uint16_t computed = crc16(0xFFFF, ctx->dgli, 2) ^ 0xFFFF;
    if (stored == computed)
        ctx->dgli_len = ((size_t)(ctx->dgli[0] & 0x3F) << 8) | ctx->dgli[1];
    ctx->dgli_got = 5;  /* done; ignore further bytes until next start */
}

static void mot_feed(pad_ctx_t *ctx, bool start, const uint8_t *d, size_t len)
{
    if (start) {
        ctx->mot_need   = ctx->dgli_len;
        ctx->dgli_len   = 0;
        ctx->mot_len    = 0;
        ctx->mot_active = ctx->mot_need > 0 && ctx->mot_need <= PAD_MAX_MOT_DG;
    }
    if (!ctx->mot_active)
        return;

    size_t take = ctx->mot_need - ctx->mot_len;
    if (take > len) take = len;
    memcpy(ctx->mot_dg + ctx->mot_len, d, take);
    ctx->mot_len += take;

    if (ctx->mot_len == ctx->mot_need) {
        mot_process_dg(ctx->mot, ctx->mot_dg, ctx->mot_len);
        ctx->mot_active = false;
    }
}

size_t pad_process(void *opaque, const uint8_t *xpad_rev, size_t xpad_len,
                   bool exact_xpad_len, const uint8_t *fpad)
{
    pad_ctx_t *ctx = opaque;
    (void)exact_xpad_len;  /* CI list delimits the X-PAD in both cases */

    /*
     * F-PAD (EN 300 401 §7.4.1):
     *   byte L-1: F-PAD type (2) | X-PAD Ind (2) | ...
     *   byte L:   ... | CI flag (bit 1) | Z (bit 0)
     */
    uint8_t fpad_type = fpad[0] >> 6;
    uint8_t xpad_ind  = (fpad[0] >> 4) & 0x03;
    bool    ci_flag   = (fpad[1] >> 1) & 1;

    int prev_app    = ctx->last_app;
    size_t prev_len = ctx->last_len;
    ctx->last_app = -1;

    if (fpad_type != 0 || (xpad_ind != 1 && xpad_ind != 2))
        return 0;

    /* X-PAD is transmitted in reverse byte order, ending next to the F-PAD:
     * if the space offered is larger than the buffer (MP2 at high bit rates
     * passes the whole frame), keep the end of it */
    uint8_t xpad[PAD_MAX_XPAD];
    if (xpad_len > sizeof(xpad)) {
        xpad_rev += xpad_len - sizeof(xpad);
        xpad_len = sizeof(xpad);
    }
    for (size_t i = 0; i < xpad_len; i++)
        xpad[i] = xpad_rev[xpad_len - 1 - i];

    subfield_t sub[4];
    int        n_sub  = 0;
    size_t     ci_len = 0;

    if (ci_flag) {
        if (xpad_ind == 1) {
            /* Short X-PAD: 1 CI byte + 3 data bytes */
            if (xpad_len < 1) return 0;
            int app = xpad[0] & 0x1F;
            ci_len = 1;
            if (app != 0)
                sub[n_sub++] = (subfield_t){ app, 3, true };
        } else {
            /* Variable size X-PAD: up to 4 CIs, end marker app type 0 */
            for (int i = 0; i < 4; i++) {
                if (xpad_len < (size_t)i + 1) return 0;
                uint8_t ci = xpad[i];
                ci_len++;
                if ((ci & 0x1F) == 0) break;
                sub[n_sub++] = (subfield_t){ ci & 0x1F, xpad_lens[ci >> 5], true };
            }
        }
    } else if (prev_app >= 0) {
        /* No CI: same length as the previous X-PAD, continuing its last app */
        sub[n_sub++] = (subfield_t){ prev_app, prev_len, false };
    }

    if (n_sub == 0)
        return xpad_ind == 1 ? 4 : ci_len;

    size_t off = ci_len;
    int    last_app = -1;
    for (int i = 0; i < n_sub; i++) {
        if (off + sub[i].len > xpad_len)
            break;
        const uint8_t *d     = xpad + off;
        size_t         len   = sub[i].len;
        int            app   = sub[i].app;
        bool           start = sub[i].start;
        off += len;

        switch (app) {
        case XPAD_APP_DGLI:
            dgli_feed(ctx, start, d, len);
            break;
        case XPAD_APP_DL_START:
        case XPAD_APP_DL_CONT:
            dls_feed(ctx->dls, start && app == XPAD_APP_DL_START, d, len);
            break;
        case XPAD_APP_MOT_START:
        case XPAD_APP_MOT_CONT:
            mot_feed(ctx, start && app == XPAD_APP_MOT_START, d, len);
            break;
        default:
            break;
        }
        last_app = continued_app(app);
    }

    if (last_app >= 0) {
        ctx->last_app = last_app;
        ctx->last_len = off;  /* total X-PAD length incl. CI list */
    }
    return xpad_ind == 1 ? 4 : off;   /* short X-PAD is always 4 bytes */
}
