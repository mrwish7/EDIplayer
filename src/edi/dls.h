#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Dynamic Label (DLS) decoder — ETSI EN 300 401 §7.4.5.2,
 * with DL Plus tag support (ETSI TS 102 980).
 *
 * DL data group layout:
 *   byte 0: Toggle (1) | First (1) | Last (1) | C (1) | Length-1 or Command (4)
 *   byte 1: C=0: First ? Charset (4) : rfa (1) + SegNum (3);  rfa (4)
 *           C=1, DL Plus: Link (1) | rfa (3) | field length-1 (4)
 *   field:  up to 16 bytes (label characters, or DL Plus command)
 *   CRC:    2 bytes, CRC-16/CCITT over prefix + field, inverted
 *
 * A label is split into up to 8 segments; it is complete once segments
 * 0..Last have all arrived with the same toggle bit.
 */

#define DLS_MAX_SEGS     8
#define DLS_SEG_LEN      16
#define DLS_MAX_RAW      (DLS_MAX_SEGS * DLS_SEG_LEN)
#define DLS_MAX_TEXT     (DLS_MAX_RAW * 3 + 1)   /* UTF-8 worst case + NUL */
#define DLPLUS_MAX_TAGS  4

typedef struct {
    uint8_t content_type;   /* 0 = DUMMY, 1 = ITEM.TITLE, 4 = ITEM.ARTIST ... */
    uint8_t start_marker;   /* character index into the label */
    uint8_t length_marker;  /* tag length - 1 */
} dlplus_tag_t;

typedef struct {
    bool         item_toggle;
    bool         item_running;
    int          n_tags;
    dlplus_tag_t tags[DLPLUS_MAX_TAGS];
} dlplus_t;

/* Called whenever the current label (or its DL Plus tags) changes.
 * text is UTF-8; dlplus is NULL when the label has no DL Plus tags. */
typedef void (*dls_cb_t)(void *opaque, const char *text, const dlplus_t *dlplus);

typedef struct {
    dls_cb_t  cb;
    void     *opaque;

    /* Data group assembly */
    uint8_t   dg[2 + DLS_SEG_LEN + 2];
    size_t    dg_len;
    size_t    dg_need;
    bool      dg_active;

    /* Segment reassembly */
    int       seg_toggle;               /* -1 = none yet */
    uint8_t   seg_data[DLS_MAX_SEGS][DLS_SEG_LEN];
    uint8_t   seg_len[DLS_MAX_SEGS];
    uint8_t   seg_have;                 /* bitmask of received segments */
    int       seg_count;                /* -1 until Last segment seen */
    uint8_t   charset;

    /* Current complete label */
    bool      label_valid;
    int       label_toggle;
    uint8_t   label_raw[DLS_MAX_RAW];
    size_t    label_raw_len;
    uint8_t   label_charset;

    /* Most recent DL Plus command */
    bool      dlp_valid;
    int       dlp_link;                 /* toggle bit of the label it applies to */
    dlplus_t  dlp;

    /* Last state reported via cb */
    bool      pub_valid;
    char      pub_text[DLS_MAX_TEXT];
    bool      pub_has_dlp;
    dlplus_t  pub_dlp;

    uint32_t  crc_errors;
} dls_ctx_t;

void dls_init(dls_ctx_t *ctx, dls_cb_t cb, void *opaque);

/* Feed one X-PAD data subfield of a DL data group.
 * start=true for app type 2 (start of data group), false for type 3. */
void dls_feed(dls_ctx_t *ctx, bool start, const uint8_t *data, size_t len);
