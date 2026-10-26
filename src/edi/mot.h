#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * MOT (Multimedia Object Transfer) decoder for the Slideshow application.
 *
 * MSC data groups (ETSI EN 300 401 §5.3.3) carrying MOT header (type 3)
 * and MOT body (type 4) segments (ETSI EN 301 234) are reassembled per
 * Transport Id. When both header and body of an object are complete, the
 * header parameters are parsed (ETSI TS 101 499 for Slideshow-specific
 * ones) and image objects are handed to the callback.
 *
 * MOT directory mode (data group types 6/7) is not supported — Slideshow
 * over X-PAD uses header mode.
 */

#define MOT_SLOTS        4       /* objects assembled concurrently */
#define MOT_MAX_SEGMENTS 1024
#define MOT_STR_MAX      513

typedef struct {
    const uint8_t *data;
    size_t         len;
    uint16_t       transport_id;
    uint8_t        content_type;      /* 2 = image */
    uint16_t       content_subtype;   /* image: 0 GIF, 1 JFIF, 2 BMP, 3 PNG */
    const char    *ext;               /* file extension for the subtype */
    char           content_name[MOT_STR_MAX];
    bool           has_category;
    uint8_t        category_id;
    uint8_t        slide_id;
    char           category_title[MOT_STR_MAX];
    char           click_through_url[MOT_STR_MAX];
    char           alt_location_url[MOT_STR_MAX];
} mot_slide_t;

typedef void (*mot_cb_t)(void *opaque, const mot_slide_t *slide);

typedef struct {
    uint8_t *data;
    uint16_t len;
} mot_seg_t;

typedef struct {
    mot_seg_t segs[MOT_MAX_SEGMENTS];
    int       n_total;      /* -1 until the last segment is seen */
    int       n_have;
} mot_seglist_t;

typedef struct {
    bool          used;
    uint16_t      tid;
    uint32_t      last_use;
    mot_seglist_t header;
    mot_seglist_t body;
} mot_object_t;

typedef struct {
    mot_cb_t     cb;
    void        *opaque;
    mot_object_t objs[MOT_SLOTS];
    uint32_t     tick;

    uint32_t     dg_crc_errors;
    uint32_t     objects;
    bool         warned_directory;
} mot_ctx_t;

void mot_init(mot_ctx_t *ctx, mot_cb_t cb, void *opaque);
void mot_free(mot_ctx_t *ctx);

/* Process one complete MSC data group (including its CRC, if flagged). */
void mot_process_dg(mot_ctx_t *ctx, const uint8_t *dg, size_t len);
