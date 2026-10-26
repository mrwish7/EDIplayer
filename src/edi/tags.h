#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * EDI TAG item parsing.
 *
 * Source is full-mux DETI EDI (ETSI TS 102 693): parse `deti` + `est` tags
 * from AF packets.
 *
 * References:
 *   ETSI TS 102 693 §5.4.1 (DETI tag items)
 *   ODR-DabMux lib/edi/ETIDecoder.cpp, lib/edi/TagItems.cpp
 */

/* Maximum FIC payload: Mode IV = 32×4 = 128 bytes */
#define MAX_FIC_LEN  128

/* Maximum sub-channel MST: 864 CUs × 8 bytes = 6912 bytes (full CIF) */
#define MAX_MST_LEN  6912

/* Maximum EST streams per frame */
#define MAX_EST      64

/*
 * Parsed DETI tag (from source full-mux EDI stream).
 *
 * Bytes 0-1 (detiHeader, uint16 BE):
 *   b15     atstf   — Absolute Time Stamp present
 *   b14     ficf    — FIC data present
 *   b13     rfudf   — RFUD data present
 *   b12-8   fcth    — high 5 bits of DFLC  (dflc = fcth*250 + fct)
 *   b7-0    fct     — low 8 bits of DFLC
 *
 * Bytes 2-5 (etiHeader, uint32 BE):
 *   b31-24  err, b23-22 mid, b21-19 fp, b16 rfu, b15-0 mnsc
 */
typedef struct {
    bool     atstf;
    bool     ficf;
    bool     rfudf;
    uint16_t dflc;          /* DAB Frame Level Counter, range 0..4999 */
    uint8_t  err;
    uint8_t  mid;           /* Mode ID */
    uint8_t  fp;            /* Frame Phase */
    bool     rfu;
    uint16_t mnsc;
    /* Timestamp (valid when atstf) */
    uint8_t  utco;
    uint32_t seconds;
    uint32_t tsta;          /* 24-bit sub-second */
    /* FIC (valid when ficf) */
    uint8_t  fic[MAX_FIC_LEN];
    uint16_t fic_len;
} deti_t;

/*
 * Parsed EST tag (one sub-channel from source full-mux EDI stream).
 *
 * Tag name: 'e','s','t', n  (n = 1-indexed stream number, binary byte)
 * Value bytes 0-2 (SSTC, uint24 BE):
 *   bits 23-18  scid  — Sub-Channel ID 0..63
 *   bits 17-8   sad   — Sub-Channel Address
 *   bits 7-2    tpl   — Transport Protection Level
 * Value bytes 3+: MST audio data
 *
 * mst points into the source AF packet buffer — only valid while that buffer lives.
 */
typedef struct {
    uint8_t        n;
    uint8_t        scid;
    uint16_t       sad;
    uint8_t        tpl;
    const uint8_t *mst;
    uint16_t       mst_size;
    bool           valid;
} est_t;

/* One fully parsed EDI frame */
typedef struct {
    bool   deti_valid;
    deti_t deti;
    int    n_est;
    est_t  est[MAX_EST];
} edi_frame_t;

/* Parse an AF packet into frame. Returns true on success. */
bool parse_af_packet(edi_frame_t *frame, const uint8_t *af_data, size_t af_len);
