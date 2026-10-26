#include "dls.h"
#include "crc16.h"
#include "fic.h"
#include <string.h>
#include "edi_log.h"
#include <stdio.h>

#define DLS_CMD_REMOVE   0x1
#define DLS_CMD_DL_PLUS  0x2
#define DLPLUS_CID_TAGS  0x0

void dls_init(dls_ctx_t *ctx, dls_cb_t cb, void *opaque)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->cb         = cb;
    ctx->opaque     = opaque;
    ctx->seg_toggle = -1;
    ctx->seg_count  = -1;
}

static void reset_segments(dls_ctx_t *ctx, int toggle)
{
    ctx->seg_toggle = toggle;
    ctx->seg_have   = 0;
    ctx->seg_count  = -1;
}

static bool dlplus_equal(const dlplus_t *a, const dlplus_t *b)
{
    if (a->item_toggle != b->item_toggle || a->item_running != b->item_running ||
            a->n_tags != b->n_tags)
        return false;
    for (int i = 0; i < a->n_tags; i++) {
        if (a->tags[i].content_type  != b->tags[i].content_type  ||
            a->tags[i].start_marker  != b->tags[i].start_marker  ||
            a->tags[i].length_marker != b->tags[i].length_marker)
            return false;
    }
    return true;
}

/* Report the current label + matching DL Plus tags if they differ from
 * what was last reported. */
static void publish(dls_ctx_t *ctx)
{
    /* A remove-label command before any label: nothing to report yet */
    if (!ctx->label_valid && !ctx->pub_valid)
        return;

    char text[DLS_MAX_TEXT] = "";
    if (ctx->label_valid)
        dab_text_to_utf8(text, sizeof(text), ctx->label_raw,
                         ctx->label_raw_len, ctx->label_charset);

    /* DL Plus tags apply only to the label whose toggle matches the link bit */
    const dlplus_t *dlp = NULL;
    if (ctx->label_valid && ctx->dlp_valid &&
            ctx->dlp_link == ctx->label_toggle)
        dlp = &ctx->dlp;

    if (ctx->pub_valid &&
            strcmp(text, ctx->pub_text) == 0 &&
            ctx->pub_has_dlp == (dlp != NULL) &&
            (!dlp || dlplus_equal(dlp, &ctx->pub_dlp)))
        return;

    ctx->pub_valid   = true;
    memcpy(ctx->pub_text, text, sizeof(text));
    ctx->pub_has_dlp = dlp != NULL;
    if (dlp)
        ctx->pub_dlp = *dlp;

    ctx->cb(ctx->opaque, text, dlp);
}

static void handle_label_segment(dls_ctx_t *ctx, const uint8_t *dg, size_t field_len)
{
    int  toggle = (dg[0] >> 7) & 1;
    bool first  = (dg[0] >> 6) & 1;
    bool last   = (dg[0] >> 5) & 1;
    int  segnum = first ? 0 : (dg[1] >> 4) & 0x07;

    if (toggle != ctx->seg_toggle)
        reset_segments(ctx, toggle);

    if (first)
        ctx->charset = dg[1] >> 4;

    memcpy(ctx->seg_data[segnum], dg + 2, field_len);
    ctx->seg_len[segnum] = (uint8_t)field_len;
    ctx->seg_have |= (uint8_t)(1u << segnum);
    if (last)
        ctx->seg_count = segnum + 1;

    if (ctx->seg_count < 0)
        return;
    uint8_t need_mask = (uint8_t)((1u << ctx->seg_count) - 1);
    if ((ctx->seg_have & need_mask) != need_mask)
        return;

    /* All segments present — assemble */
    uint8_t raw[DLS_MAX_RAW];
    size_t  raw_len = 0;
    for (int i = 0; i < ctx->seg_count; i++) {
        memcpy(raw + raw_len, ctx->seg_data[i], ctx->seg_len[i]);
        raw_len += ctx->seg_len[i];
    }

    ctx->label_valid   = true;
    ctx->label_toggle  = toggle;
    ctx->label_charset = ctx->charset;
    memcpy(ctx->label_raw, raw, raw_len);
    ctx->label_raw_len = raw_len;
    publish(ctx);
}

/*
 * DL Plus command (TS 102 980 §5.1):
 *   byte 0: CId (4) | CB (4);  for CId 0 (tags): CB = IT | IR | NT (2)
 *   then NT+1 tags, each: rfa|ContentType (7), rfa|StartMarker (7),
 *                         rfa|LengthMarker (7)
 */
static void handle_dl_plus(dls_ctx_t *ctx, const uint8_t *dg, size_t field_len)
{
    const uint8_t *f = dg + 2;
    if (field_len < 1 || (f[0] >> 4) != DLPLUS_CID_TAGS)
        return;

    int n_tags = (f[0] & 0x03) + 1;
    if (field_len < 1 + 3 * (size_t)n_tags)
        return;

    dlplus_t dlp;
    memset(&dlp, 0, sizeof(dlp));
    dlp.item_toggle  = (f[0] >> 3) & 1;
    dlp.item_running = (f[0] >> 2) & 1;
    dlp.n_tags       = n_tags;
    for (int i = 0; i < n_tags; i++) {
        dlp.tags[i].content_type  = f[1 + 3 * i] & 0x7F;
        dlp.tags[i].start_marker  = f[2 + 3 * i] & 0x7F;
        dlp.tags[i].length_marker = f[3 + 3 * i] & 0x7F;
    }

    ctx->dlp_valid = true;
    ctx->dlp_link  = (dg[1] >> 7) & 1;
    ctx->dlp       = dlp;
    publish(ctx);
}

static void decode_data_group(dls_ctx_t *ctx)
{
    const uint8_t *dg  = ctx->dg;
    size_t         len = ctx->dg_need - 2;

    uint16_t stored   = (uint16_t)((dg[len] << 8) | dg[len + 1]);
    uint16_t computed = crc16(0xFFFF, dg, len) ^ 0xFFFF;
    if (stored != computed) {
        ctx->crc_errors++;
        return;
    }

    bool c_flag = (dg[0] >> 4) & 1;
    if (!c_flag) {
        handle_label_segment(ctx, dg, len - 2);
        return;
    }

    switch (dg[0] & 0x0F) {
    case DLS_CMD_REMOVE:
        ctx->label_valid = false;
        reset_segments(ctx, -1);
        publish(ctx);
        break;
    case DLS_CMD_DL_PLUS:
        handle_dl_plus(ctx, dg, len - 2);
        break;
    default:
        break;
    }
}

/* Total data group size once the 2-byte prefix is known */
static size_t data_group_size(const uint8_t *dg)
{
    bool c_flag = (dg[0] >> 4) & 1;
    if (!c_flag)
        return 2 + (size_t)(dg[0] & 0x0F) + 1 + 2;
    if ((dg[0] & 0x0F) == DLS_CMD_DL_PLUS)
        return 2 + (size_t)(dg[1] & 0x0F) + 1 + 2;
    return 2 + 2;  /* remove label / unknown command: prefix + CRC */
}

void dls_feed(dls_ctx_t *ctx, bool start, const uint8_t *data, size_t len)
{
    if (start) {
        ctx->dg_active = true;
        ctx->dg_len    = 0;
        ctx->dg_need   = 0;
    }
    if (!ctx->dg_active)
        return;

    for (size_t i = 0; i < len; i++) {
        if (ctx->dg_len >= sizeof(ctx->dg)) {
            ctx->dg_active = false;
            return;
        }
        ctx->dg[ctx->dg_len++] = data[i];

        if (ctx->dg_len == 2)
            ctx->dg_need = data_group_size(ctx->dg);

        if (ctx->dg_need && ctx->dg_len == ctx->dg_need) {
            decode_data_group(ctx);
            ctx->dg_active = false;  /* rest of subfield is padding */
            return;
        }
    }
}
