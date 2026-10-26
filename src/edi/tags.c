#include "tags.h"
#include "crc16.h"
#include <string.h>
#include "edi_log.h"
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Byte-order helpers
 * ---------------------------------------------------------------------- */

static inline uint16_t r16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t r32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

static inline uint32_t r24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

/* -------------------------------------------------------------------------
 * TAG item parser dispatch
 * ---------------------------------------------------------------------- */

static bool parse_deti(edi_frame_t *frame, const uint8_t *val, uint32_t len)
{
    if (len < 6) {
        edi_log("DETI: too short (%u bytes)\n", len);
        return false;
    }

    deti_t *d = &frame->deti;

    /* bytes 0-1: detiHeader */
    uint16_t h0 = r16(val);
    d->atstf  = (h0 >> 15) & 1;
    d->ficf   = (h0 >> 14) & 1;
    d->rfudf  = (h0 >> 13) & 1;
    uint8_t fcth = (uint8_t)((h0 >> 8) & 0x1F);
    uint8_t fct  = (uint8_t)(h0 & 0xFF);
    d->dflc   = (uint16_t)(fcth * 250 + fct);

    /* bytes 2-5: etiHeader */
    uint32_t h1 = r32(val + 2);
    d->err  = (uint8_t)((h1 >> 24) & 0xFF);
    d->mid  = (uint8_t)((h1 >> 22) & 0x03);
    d->fp   = (uint8_t)((h1 >> 19) & 0x07);
    d->rfu  = (h1 >> 16) & 0x1;
    d->mnsc = d->rfu ? 0xFFFF : (uint16_t)(h1 & 0xFFFF);

    uint32_t fic_len = d->ficf ? (uint32_t)(4 * (d->mid == 3 ? 32 : 24)) : 0;
    uint32_t expected = 6u
        + (d->atstf  ? 1u + 4u + 3u : 0u)
        + fic_len
        + (d->rfudf  ? 3u : 0u);

    if (len != expected) {
        edi_log("DETI: length %u != expected %u (atstf=%d ficf=%d mid=%d)\n",
                len, expected, d->atstf, d->ficf, d->mid);
        return false;
    }

    size_t i = 6;

    /* Timestamp */
    d->utco = 0; d->seconds = 0; d->tsta = 0xFFFFFF;
    if (d->atstf) {
        d->utco    = val[i++];
        d->seconds = r32(val + i); i += 4;
        d->tsta    = r24(val + i); i += 3;
    }

    /* FIC */
    d->fic_len = 0;
    if (d->ficf && fic_len <= MAX_FIC_LEN) {
        memcpy(d->fic, val + i, fic_len);
        d->fic_len = (uint16_t)fic_len;
        i += fic_len;
    }

    (void)i; /* rfudf bytes intentionally skipped */

    frame->deti_valid = true;
    return true;
}

static bool parse_est(edi_frame_t *frame, const uint8_t *val, uint32_t len, uint8_t n)
{
    /* n is the raw tag 4th byte, 1-indexed stream number */
    if (n == 0 || n > MAX_EST) {
        edi_log("EST: invalid stream number %u\n", n);
        return false;
    }
    if (len < 3) {
        edi_log("EST%u: too short (%u bytes)\n", n, len);
        return false;
    }
    if (frame->n_est >= MAX_EST) return false;

    uint32_t sstc = r24(val);
    est_t *e = &frame->est[frame->n_est++];
    e->n        = n;
    e->scid     = (uint8_t)((sstc >> 18) & 0x3F);
    e->sad      = (uint16_t)((sstc >> 8) & 0x3FF);
    e->tpl      = (uint8_t)((sstc >> 2) & 0x3F);
    e->mst      = val + 3;
    e->mst_size = (uint16_t)(len - 3);
    e->valid    = true;
    return true;
}

static void parse_tagpacket(edi_frame_t *frame, const uint8_t *data, size_t len)
{
    size_t i = 0;
    while (i + 8 <= len) {
        uint32_t tag_id   = r32(data + i);
        uint32_t tag_bits = r32(data + i + 4);

        if (tag_bits % 8 != 0) {
            edi_log("TAG: bit-length %u not byte-aligned\n", tag_bits);
            break;
        }
        uint32_t tag_len = tag_bits / 8;
        if (i + 8 + tag_len > len) {
            edi_log("TAG: value extends past packet end\n");
            break;
        }

        const uint8_t *val = data + i + 8;
        i += 8 + tag_len;

        if (tag_id == 0x64657469u) {                    /* "deti" */
            parse_deti(frame, val, tag_len);
        } else if ((tag_id & 0xFFFFFF00u) == 0x65737400u) { /* "est\n" n=1..64 */
            parse_est(frame, val, tag_len, (uint8_t)(tag_id & 0xFF));
        }
        /* *ptr, *dmy, Fptt, Fsid, Fsst, avtm and ODR-specific tags: ignored */
    }
}

/* -------------------------------------------------------------------------
 * AF packet entry point
 * ---------------------------------------------------------------------- */

/*
 * AF packet layout (ETSI TS 102 821 §6.1, AFPACKET_HEADER_LEN = 10):
 *
 *  [0-1]   SYNC    = 'AF' (0x41 0x46)
 *  [2-5]   LEN     payload length in bytes (uint32 BE)
 *  [6-7]   SEQ     sequence number (uint16 BE, wraps at 0xFFFF)
 *  [8]     AR      b7=CF (CRC present), b6-4=MAJ(1), b3-0=MIN(0)  → 0x90
 *  [9]     PT      packet type: 'T' (0x54) for TAG items
 *  [10 .. 10+LEN-1] payload (TAG items)
 *  [10+LEN .. +1]   CRC-16 (when CF=1), over bytes [0 .. 10+LEN-1], XOR 0xFFFF
 */
bool parse_af_packet(edi_frame_t *frame, const uint8_t *af, size_t af_len)
{
    if (af_len < 10) return false;
    if (af[0] != 'A' || af[1] != 'F') return false;

    uint32_t payload_len = r32(af + 2);
    uint8_t  ar          = af[8];
    uint8_t  pt          = af[9];
    bool     has_crc     = (ar & 0x80) != 0;

    if (af_len < 10u + payload_len + (has_crc ? 2u : 0u)) return false;

    if (has_crc) {
        uint16_t computed = crc16(0xFFFF, af, 10 + payload_len) ^ 0xFFFF;
        uint16_t stored   = r16(af + 10 + payload_len);
        if (computed != stored) {
            edi_log("AF: CRC error (got 0x%04x expected 0x%04x)\n",
                    stored, computed);
            return false;
        }
    }

    if (pt != 'T') return false; /* only TAG packets supported */

    memset(frame, 0, sizeof(*frame));
    parse_tagpacket(frame, af + 10, payload_len);
    return frame->deti_valid;
}
