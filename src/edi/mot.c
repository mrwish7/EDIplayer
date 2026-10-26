#include "mot.h"
#include "crc16.h"
#include <stdlib.h>
#include <string.h>
#include "edi_log.h"
#include <stdio.h>

#define DG_TYPE_MOT_HEADER  3
#define DG_TYPE_MOT_BODY    4
#define DG_TYPE_MOT_DIR     6
#define DG_TYPE_MOT_DIR_CMP 7

/* MOT header extension parameter ids */
#define PARAM_CONTENT_NAME      0x0C
#define PARAM_CATEGORY_SLIDE_ID 0x25
#define PARAM_CATEGORY_TITLE    0x26
#define PARAM_CLICK_THROUGH_URL 0x27
#define PARAM_ALT_LOCATION_URL  0x28

#define CONTENT_TYPE_IMAGE 2

void mot_init(mot_ctx_t *ctx, mot_cb_t cb, void *opaque)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->cb     = cb;
    ctx->opaque = opaque;
    for (int i = 0; i < MOT_SLOTS; i++) {
        ctx->objs[i].header.n_total = -1;
        ctx->objs[i].body.n_total   = -1;
    }
}

static void seglist_clear(mot_seglist_t *l)
{
    for (int i = 0; i < MOT_MAX_SEGMENTS; i++) {
        free(l->segs[i].data);
        l->segs[i].data = NULL;
        l->segs[i].len  = 0;
    }
    l->n_total = -1;
    l->n_have  = 0;
}

static void object_clear(mot_object_t *o)
{
    seglist_clear(&o->header);
    seglist_clear(&o->body);
    o->used = false;
}

void mot_free(mot_ctx_t *ctx)
{
    for (int i = 0; i < MOT_SLOTS; i++)
        object_clear(&ctx->objs[i]);
}

static mot_object_t *find_or_alloc(mot_ctx_t *ctx, uint16_t tid)
{
    mot_object_t *lru = &ctx->objs[0];
    for (int i = 0; i < MOT_SLOTS; i++) {
        mot_object_t *o = &ctx->objs[i];
        if (o->used && o->tid == tid)
            return o;
    }
    for (int i = 0; i < MOT_SLOTS; i++) {
        mot_object_t *o = &ctx->objs[i];
        if (!o->used) { lru = o; break; }
        if (o->last_use < lru->last_use) lru = o;
    }
    object_clear(lru);
    lru->used = true;
    lru->tid  = tid;
    return lru;
}

/* Store a segment. Returns false if it conflicts with what's held. */
static bool seglist_add(mot_seglist_t *l, int segnum, bool last,
                        const uint8_t *data, size_t len)
{
    if (segnum >= MOT_MAX_SEGMENTS)
        return false;

    mot_seg_t *s = &l->segs[segnum];
    if (s->data) {
        /* Repetition — if it differs, the object changed under this TId */
        if (s->len != len || memcmp(s->data, data, len) != 0)
            return false;
    } else {
        s->data = malloc(len ? len : 1);
        if (!s->data) return true;
        memcpy(s->data, data, len);
        s->len = (uint16_t)len;
        l->n_have++;
    }
    if (last)
        l->n_total = segnum + 1;
    return true;
}

static bool seglist_complete(const mot_seglist_t *l)
{
    if (l->n_total < 0)
        return false;
    for (int i = 0; i < l->n_total; i++)
        if (!l->segs[i].data)
            return false;
    return true;
}

/* Concatenate all segments into a newly allocated buffer */
static uint8_t *seglist_join(const mot_seglist_t *l, size_t *out_len)
{
    size_t total = 0;
    for (int i = 0; i < l->n_total; i++)
        total += l->segs[i].len;
    uint8_t *buf = malloc(total ? total : 1);
    if (!buf) return NULL;
    size_t off = 0;
    for (int i = 0; i < l->n_total; i++) {
        memcpy(buf + off, l->segs[i].data, l->segs[i].len);
        off += l->segs[i].len;
    }
    *out_len = total;
    return buf;
}

static void copy_str(char *dst, const uint8_t *src, size_t len)
{
    if (len >= MOT_STR_MAX) len = MOT_STR_MAX - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static const char *image_ext(uint16_t subtype)
{
    switch (subtype) {
    case 0:  return "gif";
    case 1:  return "jpg";
    case 2:  return "bmp";
    case 3:  return "png";
    default: return NULL;
    }
}

/*
 * MOT header (EN 301 234 §6.1):
 *   Header core (7 bytes): BodySize (28) | HeaderSize (13) |
 *                          ContentType (6) | ContentSubType (9)
 *   Header extension: parameters, each PLI (2) | ParamId (6) then
 *     PLI 0: no data, 1: 1 byte, 2: 4 bytes,
 *     PLI 3: Ext (1) | DataFieldLength (7 or 15) then that many bytes
 */
static bool parse_header(const uint8_t *h, size_t len, mot_slide_t *s,
                         uint32_t *body_size)
{
    if (len < 7) return false;

    *body_size = ((uint32_t)h[0] << 20) | ((uint32_t)h[1] << 12) |
                 ((uint32_t)h[2] << 4)  | (h[3] >> 4);
    size_t header_size = ((size_t)(h[3] & 0x0F) << 9) | ((size_t)h[4] << 1) | (h[5] >> 7);
    s->content_type    = (h[5] >> 1) & 0x3F;
    s->content_subtype = (uint16_t)(((h[5] & 0x01) << 8) | h[6]);

    if (header_size > len) header_size = len;

    size_t i = 7;
    while (i < header_size) {
        uint8_t pli = h[i] >> 6;
        uint8_t pid = h[i] & 0x3F;
        i++;

        size_t dlen;
        switch (pli) {
        case 0: dlen = 0; break;
        case 1: dlen = 1; break;
        case 2: dlen = 4; break;
        default:
            if (i >= header_size) return false;
            if (h[i] & 0x80) {
                if (i + 1 >= header_size) return false;
                dlen = ((size_t)(h[i] & 0x7F) << 8) | h[i + 1];
                i += 2;
            } else {
                dlen = h[i] & 0x7F;
                i += 1;
            }
            break;
        }
        if (i + dlen > header_size) return false;
        const uint8_t *d = h + i;
        i += dlen;

        switch (pid) {
        case PARAM_CONTENT_NAME:
            /* first byte: charset (4) | rfa (4) */
            if (dlen >= 1) copy_str(s->content_name, d + 1, dlen - 1);
            break;
        case PARAM_CATEGORY_SLIDE_ID:
            if (dlen >= 2) {
                s->has_category = true;
                s->category_id  = d[0];
                s->slide_id     = d[1];
            }
            break;
        case PARAM_CATEGORY_TITLE:
            copy_str(s->category_title, d, dlen);
            break;
        case PARAM_CLICK_THROUGH_URL:
            copy_str(s->click_through_url, d, dlen);
            break;
        case PARAM_ALT_LOCATION_URL:
            copy_str(s->alt_location_url, d, dlen);
            break;
        default:
            break;
        }
    }
    return true;
}

static void try_complete(mot_ctx_t *ctx, mot_object_t *o)
{
    if (!seglist_complete(&o->header) || !seglist_complete(&o->body))
        return;

    size_t   hdr_len = 0, body_len = 0;
    uint8_t *hdr  = seglist_join(&o->header, &hdr_len);
    uint8_t *body = seglist_join(&o->body, &body_len);

    mot_slide_t s;
    memset(&s, 0, sizeof(s));
    uint32_t body_size = 0;

    if (hdr && body && parse_header(hdr, hdr_len, &s, &body_size)) {
        if (body_size != body_len) {
            edi_log("MOT: TId %u body size mismatch (%zu vs header %u)\n",
                    o->tid, body_len, body_size);
        } else if (s.content_type == CONTENT_TYPE_IMAGE &&
                   (s.ext = image_ext(s.content_subtype)) != NULL) {
            s.data         = body;
            s.len          = body_len;
            s.transport_id = o->tid;
            ctx->objects++;
            ctx->cb(ctx->opaque, &s);
        } else {
            edi_log("MOT: ignoring object TId %u (ContentType %u/%u)\n",
                    o->tid, s.content_type, s.content_subtype);
        }
    }

    free(hdr);
    free(body);
    /* Start afresh: a carousel repeat of this TId will be reassembled and
     * reported again, and the output side de-duplicates by content */
    object_clear(o);
}

/*
 * MSC data group (EN 300 401 §5.3.3):
 *   byte 0: Extension (1) | CRC (1) | Segment (1) | UserAccess (1) | Type (4)
 *   byte 1: Continuity index (4) | Repetition index (4)
 *   [Extension field (2)]
 *   [Segment field: Last (1) | SegmentNumber (15)]
 *   [User access: rfa (3) | TIdFlag (1) | LengthIndicator (4),
 *                 TId (2, if flagged), End user address]
 *   Data field — for MOT: segmentation header RepCount (3) | SegSize (13),
 *                then segment data
 *   [CRC (2)]
 */
void mot_process_dg(mot_ctx_t *ctx, const uint8_t *dg, size_t len)
{
    if (len < 2) return;

    bool    ext_flag = (dg[0] >> 7) & 1;
    bool    crc_flag = (dg[0] >> 6) & 1;
    bool    seg_flag = (dg[0] >> 5) & 1;
    bool    ua_flag  = (dg[0] >> 4) & 1;
    uint8_t dg_type  = dg[0] & 0x0F;

    size_t end = len;
    if (crc_flag) {
        if (len < 4) return;
        end = len - 2;
        uint16_t stored   = (uint16_t)((dg[end] << 8) | dg[end + 1]);
        uint16_t computed = crc16(0xFFFF, dg, end) ^ 0xFFFF;
        if (stored != computed) {
            ctx->dg_crc_errors++;
            return;
        }
    }

    if (dg_type == DG_TYPE_MOT_DIR || dg_type == DG_TYPE_MOT_DIR_CMP) {
        if (!ctx->warned_directory) {
            edi_log("MOT: directory mode not supported — slides ignored\n");
            ctx->warned_directory = true;
        }
        return;
    }
    if (dg_type != DG_TYPE_MOT_HEADER && dg_type != DG_TYPE_MOT_BODY)
        return;

    size_t off = 2;
    if (ext_flag) off += 2;

    bool last   = false;
    int  segnum = 0;
    if (seg_flag) {
        if (off + 2 > end) return;
        last   = (dg[off] >> 7) & 1;
        segnum = ((dg[off] & 0x7F) << 8) | dg[off + 1];
        off += 2;
    }

    bool     has_tid = false;
    uint16_t tid     = 0;
    if (ua_flag) {
        if (off + 1 > end) return;
        bool   tid_flag = (dg[off] >> 4) & 1;
        size_t li       = dg[off] & 0x0F;
        off++;
        if (off + li > end) return;
        if (tid_flag && li >= 2) {
            has_tid = true;
            tid = (uint16_t)((dg[off] << 8) | dg[off + 1]);
        }
        off += li;
    }
    if (!has_tid) return;

    if (off + 2 > end) return;
    size_t seg_size = ((size_t)(dg[off] & 0x1F) << 8) | dg[off + 1];
    off += 2;
    if (off + seg_size > end) return;

    mot_object_t *o = find_or_alloc(ctx, tid);
    o->last_use = ++ctx->tick;

    mot_seglist_t *l = dg_type == DG_TYPE_MOT_HEADER ? &o->header : &o->body;
    if (!seglist_add(l, segnum, last, dg + off, seg_size)) {
        /* Content under this TId changed: restart the object */
        object_clear(o);
        o->used     = true;
        o->tid      = tid;
        o->last_use = ctx->tick;
        l = dg_type == DG_TYPE_MOT_HEADER ? &o->header : &o->body;
        seglist_add(l, segnum, last, dg + off, seg_size);
    }

    try_complete(ctx, o);
}
