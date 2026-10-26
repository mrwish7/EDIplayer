#include "fic.h"
#include <string.h>
#include "edi_log.h"
#include <stdio.h>

/*
 * FIC structure (ETSI EN 300 401):
 *
 *   FIC = N × FIB, where each FIB is 32 bytes:
 *     [0-29]  FIG data (multiple concatenated FIGs)
 *     [30-31] CRC-16 (skipped here — AF-layer CRC already validated the packet)
 *
 *   Each FIG begins with a 1-byte header:
 *     bits [7:5]  FIG type  (0-6; 7 = end marker)
 *     bits [4:0]  length    (bytes of FIG data following this header byte)
 *
 *   FIG type 0 second byte (C/N | OE | P/D | Extension[4:0]):
 *     P/D = 0  → Programme Service (SId = 16 bits)
 *     P/D = 1  → Data Service      (SId = 32 bits, not parsed here)
 *
 *   FIG type 1 second byte (Charset[7:4] | OE | Extension[2:0])
 */

#define FIB_TOTAL    32   /* bytes per FIB including CRC */
#define FIB_DATA_LEN 30   /* FIG data bytes per FIB */

#define CHARSET_EBU_LATIN 0x0
#define CHARSET_UCS2      0x6
#define CHARSET_UTF8      0xF

/*
 * EBU Latin based repertoire → UTF-8 (ETSI TS 101 756 Table 1).
 * 0x20–0x7F are treated as ASCII; only the control and upper ranges
 * need mapping. Line break (0x0A) and end of headline (0x0B) become a
 * space so DL Plus character positions stay aligned; soft hyphen is dropped.
 */
static const char *const ebu_lo[0x20] = {
    "",   "Ę", "Į", "Ų", "Ă", "Ė", "Ď", "Ș", "Ț", "Ċ", " ", " ", "Ġ", "Ĺ", "Ż", "Ń",
    "ą",  "ę", "į", "ų", "ă", "ė", "ď", "ș", "ț", "ċ", "Ň", "Ě", "ġ", "ĺ", "ż", "",
};
static const char *const ebu_hi[0x80] = {
    "á", "à", "é", "è", "í", "ì", "ó", "ò", "ú", "ù", "Ñ", "Ç", "Ş", "ß", "¡", "Ÿ",
    "â", "ä", "ê", "ë", "î", "ï", "ô", "ö", "û", "ü", "ñ", "ç", "ş", "ğ", "ı", "ÿ",
    "Ķ", "Ņ", "©", "Ģ", "Ğ", "ě", "ň", "ő", "Ő", "€", "£", "$", "Ā", "Ē", "Ī", "Ū",
    "ķ", "ņ", "Ļ", "ģ", "ļ", "İ", "ń", "ű", "Ű", "¿", "ľ", "°", "ā", "ē", "ī", "ū",
    "Á", "À", "É", "È", "Í", "Ì", "Ó", "Ò", "Ú", "Ù", "Ř", "Č", "Š", "Ž", "Ð", "Ŀ",
    "Â", "Ä", "Ê", "Ë", "Î", "Ï", "Ô", "Ö", "Û", "Ü", "ř", "č", "š", "ž", "đ", "ŀ",
    "Ã", "Å", "Æ", "Œ", "ŷ", "Ý", "Õ", "Ø", "Þ", "Ŋ", "Ŕ", "Ć", "Ś", "Ź", "Ť", "ð",
    "ã", "å", "æ", "œ", "ŵ", "ý", "õ", "ø", "þ", "ŋ", "ŕ", "ć", "ś", "ź", "ť", "ħ",
};

size_t dab_text_to_utf8(char *dst, size_t dst_size,
                        const uint8_t *src, size_t len, uint8_t charset)
{
    if (dst_size == 0) return 0;
    char *p   = dst;
    char *end = dst + dst_size - 1; /* reserve NUL */

    for (size_t i = 0; i < len; i++) {
        char        tmp[4];
        const char *m  = NULL;
        size_t      ml = 1;
        uint8_t     c  = src[i];

        if (charset == CHARSET_EBU_LATIN) {
            if (c < 0x20)       m = ebu_lo[c];
            else if (c >= 0x80) m = ebu_hi[c - 0x80];
        } else if (charset == CHARSET_UCS2) {
            /* UCS-2 big-endian: encode each 16-bit code point as UTF-8 */
            if (i + 1 >= len) break;
            uint16_t cp = (uint16_t)((src[i] << 8) | src[i + 1]);
            i++;
            if (cp < 0x80) {
                tmp[0] = (char)cp; ml = 1;
            } else if (cp < 0x800) {
                tmp[0] = (char)(0xC0 | (cp >> 6));
                tmp[1] = (char)(0x80 | (cp & 0x3F)); ml = 2;
            } else {
                tmp[0] = (char)(0xE0 | (cp >> 12));
                tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                tmp[2] = (char)(0x80 | (cp & 0x3F)); ml = 3;
            }
            m = tmp;
        }

        if (!m) { tmp[0] = (char)c; m = tmp; ml = 1; }
        else if (m != tmp) ml = strlen(m);

        if (p + ml > end) break;
        memcpy(p, m, ml);
        p += ml;
    }
    *p = '\0';
    return (size_t)(p - dst);
}

/* Convert a 16-byte FIG 1 label to NUL-terminated UTF-8, trimming
 * leading and trailing spaces (control codes such as EBU 0x0A/0x0B convert
 * to spaces too, so trim after conversion). dst must hold FIC_LABEL_MAX bytes. */
static void label_to_utf8(char *dst, const uint8_t *src, uint8_t charset)
{
    int n = 16;
    while (n > 0 && src[n - 1] == 0x00)
        n--;
    size_t len = dab_text_to_utf8(dst, FIC_LABEL_MAX, src, (size_t)n, charset);
    while (len > 0 && dst[len - 1] == ' ')
        dst[--len] = '\0';
    size_t s = 0;
    while (s < len && dst[s] == ' ')
        s++;
    memmove(dst, dst + s, len - s + 1);
}

void fic_init(fic_state_t *s)
{
    memset(s, 0, sizeof(*s));
}

static svc_info_t *find_or_add(fic_state_t *s, uint16_t sid)
{
    for (int i = 0; i < s->n_svcs; i++)
        if (s->svcs[i].sid == sid)
            return &s->svcs[i];
    if (s->n_svcs >= FIC_MAX_SVCS)
        return NULL;
    svc_info_t *sv = &s->svcs[s->n_svcs++];
    memset(sv, 0, sizeof(*sv));
    sv->sid = sid;
    return sv;
}

/*
 * FIG 0/2 — Service Organisation (Programme Services, P/D=0).
 * ETSI EN 300 401 §6.3.1.
 *
 * For each service:
 *   SId            16 bits
 *   Local flag      1 bit
 *   CAId            3 bits
 *   nSCn            4 bits  (actual service component count, 1–12)
 *   For each component (2 bytes each):
 *     TMId          2 bits  (00=MSC stream audio, 01=MSC stream data,
 *                            10=FIDC, 11=MSC packet data)
 *     ASCTy/DSCTy   6 bits
 *     SubChId       6 bits  (= SCID in ETI/EDI)
 *     PS            1 bit
 *     CA            1 bit
 */
static void parse_fig0_2(fic_state_t *s, const uint8_t *d, int len)
{
    int i = 0;
    while (i + 3 <= len) {
        uint16_t sid = ((uint16_t)d[i] << 8) | d[i + 1];
        i += 2;
        int nsc = d[i] & 0x0F; /* actual component count */
        i++;

        for (int c = 0; c < nsc && i + 2 <= len; c++) {
            uint8_t tmid    = (d[i] >> 6) & 0x03;
            uint8_t ty      = d[i] & 0x3F;
            uint8_t subchid = (d[i + 1] >> 2) & 0x3F;
            bool    primary = (d[i + 1] >> 1) & 0x01;
            i += 2;

            if (primary && (tmid == 0 || tmid == 1)) { /* MSC stream (audio or data) */
                svc_info_t *sv = find_or_add(s, sid);
                if (sv) {
                    sv->scid     = subchid;
                    sv->tmid     = tmid;
                    sv->ascty    = ty;
                    sv->has_scid = true;
                }
            }
            /* TMId 2 (FIDC) and 3 (MSC packet) use different SubChId fields;
             * skip them as they're uncommon for the use case here. */
        }
    }
}

/*
 * FIG 1/0 — Ensemble Label.
 * ETSI EN 300 401 §8.1.13.
 *
 *   EId            16 bits
 *   Label          128 bits (16 EBU Latin characters, space-padded)
 *   Short label    16 bits  (bitmask, not needed here)
 */
static void parse_fig1_0(fic_state_t *s, const uint8_t *d, int len,
                         uint8_t charset)
{
    if (len < 18)
        return;
    s->eid = ((uint16_t)d[0] << 8) | d[1];
    label_to_utf8(s->ens_label, d + 2, charset);
    s->has_ens_label = true;
}

/*
 * FIG 1/1 — Programme Service Label.
 * ETSI EN 300 401 §8.1.1.
 *
 * For each service (20 bytes each):
 *   SId            16 bits
 *   Label          128 bits (16 EBU Latin characters, space-padded)
 *   Short label    16 bits  (bitmask, not needed here)
 */
static void parse_fig1_1(fic_state_t *s, const uint8_t *d, int len,
                         uint8_t charset)
{
    int i = 0;
    while (i + 20 <= len) {
        uint16_t sid = ((uint16_t)d[i] << 8) | d[i + 1];
        i += 2;

        svc_info_t *sv = find_or_add(s, sid);
        if (sv) {   /* labels may change, so always take the latest */
            label_to_utf8(sv->label, d + i, charset);
            sv->has_label = true;
        }
        i += 16 + 2; /* label + short label flag */
    }
}

static void parse_fib(fic_state_t *s, const uint8_t *fib)
{
    const uint8_t *p   = fib;
    const uint8_t *end = fib + FIB_DATA_LEN;

    while (p < end) {
        uint8_t hdr      = p[0];
        uint8_t fig_type = (hdr >> 5) & 0x07;
        uint8_t fig_len  = hdr & 0x1F;

        if (fig_type == 7) break; /* end marker */

        const uint8_t *fig_data = p + 1;
        p += 1 + fig_len;
        if (fig_len == 0 || p > end || fig_data >= end) break;

        if (fig_type == 0 && fig_len >= 2) {
            uint8_t common = fig_data[0];
            uint8_t pd     = (common >> 5) & 0x01;
            uint8_t ext    = common & 0x1F;
            if (ext == 2 && pd == 0)
                parse_fig0_2(s, fig_data + 1, fig_len - 1);

        } else if (fig_type == 1 && fig_len >= 2) {
            uint8_t common = fig_data[0];
            uint8_t charset = (common >> 4) & 0x0F;
            uint8_t ext     = common & 0x07;
            if (ext == 0)
                parse_fig1_0(s, fig_data + 1, fig_len - 1, charset);
            else if (ext == 1)
                parse_fig1_1(s, fig_data + 1, fig_len - 1, charset);
        }
    }
}

void fic_parse(fic_state_t *s, const uint8_t *fic, uint16_t fic_len)
{
    for (uint16_t off = 0; off + FIB_TOTAL <= fic_len; off += FIB_TOTAL)
        parse_fib(s, fic + off);
}

bool fic_complete(const fic_state_t *s, int n_streams)
{
    int matched = 0;
    for (int i = 0; i < s->n_svcs; i++)
        if (s->svcs[i].has_scid && s->svcs[i].has_label)
            matched++;
    return s->has_ens_label && matched >= n_streams;
}

const svc_info_t *fic_find_by_scid(const fic_state_t *s, uint8_t scid)
{
    for (int i = 0; i < s->n_svcs; i++)
        if (s->svcs[i].has_scid && s->svcs[i].scid == scid)
            return &s->svcs[i];
    return NULL;
}
