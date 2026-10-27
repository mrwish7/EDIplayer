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

/* Short label: the characters picked by the 16-bit mask (bit 15 = first) */
static void short_label_to_utf8(char *dst, const uint8_t *src, uint16_t mask, uint8_t charset)
{
    uint8_t pick[32];
    size_t n = 0;
    for (int i = 0, ch = 0; i < 16 && ch < 16; ch++) {
        int cl = 1;                                           /* bytes in this character */
        if (charset == CHARSET_UCS2) cl = 2;
        else if (charset == CHARSET_UTF8)
            while (i + cl < 16 && (src[i + cl] & 0xC0) == 0x80) cl++;
        if (i + cl > 16) break;
        if (mask & (0x8000 >> ch)) {
            memcpy(pick + n, src + i, (size_t)cl);
            n += (size_t)cl;
        }
        i += cl;
    }
    dab_text_to_utf8(dst, FIC_LABEL_MAX, pick, n, charset);
}

void fic_init(fic_state_t *s)
{
    memset(s, 0, sizeof(*s));
}

static fic_service_t *find_or_add_service(fic_state_t *s, uint32_t sid, bool pd)
{
    for (int i = 0; i < s->n_services; i++)
        if (s->services[i].sid == sid && s->services[i].pd == pd)
            return &s->services[i];
    if (s->n_services >= FIC_MAX_SVCS)
        return NULL;
    fic_service_t *sv = &s->services[s->n_services++];
    memset(sv, 0, sizeof(*sv));
    sv->sid = sid;
    sv->pd  = pd;
    return sv;
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
static uint32_t read_sid(const uint8_t *d, bool pd)
{
    return pd ? ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3]
              : ((uint32_t)d[0] << 8) | d[1];
}

static void parse_fig0_2(fic_state_t *s, const uint8_t *d, int len, bool pd)
{
    const int sid_len = pd ? 4 : 2;
    int i = 0;
    while (i + sid_len + 1 <= len) {
        uint32_t sid32 = read_sid(d + i, pd);
        uint16_t sid = (uint16_t)sid32;
        i += sid_len;
        int nsc = d[i] & 0x0F; /* actual component count */
        fic_service_t *fs = find_or_add_service(s, sid32, pd);
        if (fs) {
            fs->local   = d[i] >> 7;
            fs->caid    = (d[i] >> 4) & 0x07;
            fs->n_comps = 0;
        }
        i++;

        for (int c = 0; c < nsc && i + 2 <= len; c++) {
            uint8_t tmid    = (d[i] >> 6) & 0x03;
            uint8_t ty      = d[i] & 0x3F;
            uint8_t subchid = (d[i + 1] >> 2) & 0x3F;
            bool    primary = (d[i + 1] >> 1) & 0x01;
            uint16_t scid   = (uint16_t)(((d[i] & 0x3F) << 6) | (d[i + 1] >> 2));
            if (fs && fs->n_comps < FIC_MAX_SVC_COMPS) {
                fic_svc_comp_t *sc = &fs->comps[fs->n_comps++];
                sc->tmid    = tmid;
                sc->type    = tmid == 3 ? 0 : ty;
                sc->subch   = subchid;
                sc->scid    = tmid == 3 ? scid : 0;
                sc->primary = primary;
                sc->ca      = d[i + 1] & 0x01;
            }
            i += 2;

            if (tmid == 3 && primary) { /* MSC packet data: remember for SPI lookup */
                int k;
                for (k = 0; k < s->n_prim && s->prim[k].sid != sid32; k++) {}
                if (k == s->n_prim && s->n_prim < FIC_MAX_COMPS) s->n_prim++;
                if (k < FIC_MAX_COMPS) {
                    s->prim[k].sid  = sid32;
                    s->prim[k].scid = scid;
                }
            }
            if (pd) continue;   /* the service list holds programme services only */

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
 * FIG 0/1 — Sub-channel organization. ETSI EN 300 401 §6.2.1.
 *   SubChId (6) | Start address (10) | Short/Long (1)
 *   short form: Table switch (1) | Table index (6)          -> UEP (table 8)
 *   long form:  Option (3) | Protection level (2) | Size (10) -> EEP
 */
static const struct { uint16_t size; uint8_t level; uint16_t kbps; } uep_table[64] = {   /* table 8 */
    {16, 5, 32},   {21, 4, 32},   {24, 3, 32},   {29, 2, 32},   {35, 1, 32},
    {24, 5, 48},   {29, 4, 48},   {35, 3, 48},   {42, 2, 48},   {52, 1, 48},
    {29, 5, 56},   {35, 4, 56},   {42, 3, 56},   {52, 2, 56},
    {32, 5, 64},   {42, 4, 64},   {48, 3, 64},   {58, 2, 64},   {70, 1, 64},
    {40, 5, 80},   {52, 4, 80},   {58, 3, 80},   {70, 2, 80},   {84, 1, 80},
    {48, 5, 96},   {58, 4, 96},   {70, 3, 96},   {84, 2, 96},   {104, 1, 96},
    {58, 5, 112},  {70, 4, 112},  {84, 3, 112},  {104, 2, 112},
    {64, 5, 128},  {84, 4, 128},  {96, 3, 128},  {116, 2, 128}, {140, 1, 128},
    {80, 5, 160},  {104, 4, 160}, {116, 3, 160}, {140, 2, 160}, {168, 1, 160},
    {96, 5, 192},  {116, 4, 192}, {140, 3, 192}, {168, 2, 192}, {208, 1, 192},
    {116, 5, 224}, {140, 4, 224}, {168, 3, 224}, {208, 2, 224}, {232, 1, 224},
    {128, 5, 256}, {168, 4, 256}, {192, 3, 256}, {232, 2, 256}, {280, 1, 256},
    {160, 5, 320}, {208, 4, 320}, {280, 2, 320},
    {192, 5, 384}, {280, 3, 384}, {416, 1, 384},
};

static void parse_fig0_1(fic_state_t *s, const uint8_t *d, int len)
{
    int i = 0;
    while (i + 3 <= len) {
        uint8_t id = d[i] >> 2;
        fic_subch_t sc = s->subch[id];   /* keep FEC and language */
        sc.valid = true;
        sc.start = (uint16_t)(((d[i] & 0x03) << 8) | d[i + 1]);
        if (d[i + 2] & 0x80) {
            if (i + 4 > len) break;
            uint8_t option = (d[i + 2] >> 4) & 0x07;
            sc.eep   = true;
            sc.level = (uint8_t)(((d[i + 2] >> 2) & 0x03) + 1);
            sc.eep_b = option == 1;
            sc.size  = (uint16_t)(((d[i + 2] & 0x03) << 8) | d[i + 3]);
            /* EEP-A: 8n kbit/s in 12n/8n/6n/4n CUs; EEP-B: 32n kbit/s in 27n/21n/18n/15n CUs */
            static const uint8_t cu_a[4] = {12, 8, 6, 4}, cu_b[4] = {27, 21, 18, 15};
            sc.kbps = sc.eep_b ? (uint16_t)(sc.size / cu_b[sc.level - 1] * 32)
                               : (uint16_t)(sc.size / cu_a[sc.level - 1] * 8);
            i += 4;
        } else {
            sc.eep       = false;
            sc.eep_b     = false;
            sc.uep_index = d[i + 2] & 0x3F;
            sc.level     = uep_table[sc.uep_index].level;
            sc.size      = uep_table[sc.uep_index].size;
            sc.kbps      = uep_table[sc.uep_index].kbps;
            i += 3;
        }
        s->subch[id] = sc;
    }
}

/* FIG 0/0 — Ensemble information. EId (16) | Change flags (2) | AL (1) |
 * CIF count high (5) | CIF count low (8) | [Occurrence change (8)] */
static void parse_fig0_0(fic_state_t *s, const uint8_t *d, int len)
{
    if (len < 4) return;
    s->has_fig0_0   = true;
    s->change_flags = d[2] >> 6;
    s->al_flag      = (d[2] >> 5) & 0x01;
    s->cif_count    = (uint16_t)((d[2] & 0x1F) * 250 + d[3]);
    s->has_occurrence_change = s->change_flags && len >= 5;
    if (s->has_occurrence_change) s->occurrence_change = d[4];
}

/* FIG 0/5 — Service component language.
 *   L/S = 0: L/S (1) | MSC/FIC (1) | SubChId (6) | Language (8)
 *   L/S = 1: L/S (1) | rfa (3) | SCId (12) | Language (8) */
static void parse_fig0_5(fic_state_t *s, const uint8_t *d, int len)
{
    int i = 0;
    while (i + 2 <= len) {
        if (d[i] & 0x80) {
            if (i + 3 > len) break;
            uint16_t scid = (uint16_t)(((d[i] & 0x0F) << 8) | d[i + 1]);
            int k;
            for (k = 0; k < s->n_comp_langs && s->comp_langs[k].scid != scid; k++) {}
            if (k == s->n_comp_langs && s->n_comp_langs < FIC_MAX_COMPS) s->n_comp_langs++;
            if (k < FIC_MAX_COMPS) {
                s->comp_langs[k].scid = scid;
                s->comp_langs[k].lang = d[i + 2];
            }
            i += 3;
        } else {
            if (!(d[i] & 0x40)) {   /* MSC */
                s->subch[d[i] & 0x3F].lang     = d[i + 1];
                s->subch[d[i] & 0x3F].has_lang = true;
            }
            i += 2;
        }
    }
}

/* FIG 0/7 — Configuration information. Services (6) | Count (10) */
static void parse_fig0_7(fic_state_t *s, const uint8_t *d, int len)
{
    if (len < 2) return;
    s->has_fig0_7     = true;
    s->cfg_services   = d[0] >> 2;
    s->reconfig_count = (uint16_t)(((d[0] & 0x03) << 8) | d[1]);
}

/* FIG 0/10 — Date and time. rfu (1) | MJD (17) | LSI (1) | rfa (1) | UTC flag (1) |
 * hours (5) | minutes (6) | [seconds (6) | milliseconds (10)] */
static void parse_fig0_10(fic_state_t *s, const uint8_t *d, int len)
{
    if (len < 4) return;
    s->mjd      = ((uint32_t)(d[0] & 0x7F) << 10) | ((uint32_t)d[1] << 2) | (d[2] >> 6);
    s->lsi      = (d[2] >> 5) & 0x01;
    s->utc_long = (d[2] >> 3) & 0x01;
    s->hour     = (uint8_t)(((d[2] & 0x07) << 2) | (d[3] >> 6));
    s->minute   = d[3] & 0x3F;
    s->second   = 0;
    s->ms       = 0;
    if (s->utc_long && len >= 6) {
        s->second = d[4] >> 2;
        s->ms     = (uint16_t)(((d[4] & 0x03) << 8) | d[5]);
    }
    s->has_time = true;
}

/* FIG 0/14 — FEC sub-channel organisation. SubChId (6) | FEC scheme (2) */
static void parse_fig0_14(fic_state_t *s, const uint8_t *d, int len)
{
    for (int i = 0; i < len; i++) {
        s->subch[d[i] >> 2].fec     = d[i] & 0x03;
        s->subch[d[i] >> 2].has_fec = true;
    }
}

/*
 * FIG 0/17 — Programme Type. ETSI EN 300 401 §8.1.5.
 *   SId (16) | S/D (1) | P/S (1) | L flag (1) | CC flag (1) | rfa (4)
 *   [Language (8) if L] | rfa (3) | International code (5) | [rfa (3) | Complementary code (5) if CC]
 * (The 2017 edition made the L and CC flags reserved; they are then 0.)
 */
static void parse_fig0_17(fic_state_t *s, const uint8_t *d, int len)
{
    int i = 0;
    while (i + 4 <= len) {
        uint16_t sid = (uint16_t)((d[i] << 8) | d[i + 1]);
        bool dynamic = d[i + 2] >> 7;
        bool l_flag  = (d[i + 2] >> 5) & 1;
        bool cc_flag = (d[i + 2] >> 4) & 1;
        int  off = i + 3 + (l_flag ? 1 : 0);
        if (off >= len) break;
        uint8_t pty = d[off] & 0x1F;
        i = off + 1 + (cc_flag ? 1 : 0);

        svc_info_t *sv = find_or_add(s, sid);
        if (sv) {
            sv->pty         = pty;
            sv->has_pty     = true;
            sv->pty_dynamic = dynamic;
        }
    }
}

/*
 * FIG 0/3 — Service component in packet mode. ETSI EN 300 401 §6.3.2.
 *   SCId (12) | rfa (3) | CAOrg flag (1)
 *   DG flag (1) | rfu (1) | DSCTy (6)
 *   SubChId (6) | Packet address (10)
 *   [CAOrg (16)]
 */
static void parse_fig0_3(fic_state_t *s, const uint8_t *d, int len)
{
    int i = 0;
    while (i + 5 <= len) {
        uint16_t scid  = (uint16_t)((d[i] << 4) | (d[i + 1] >> 4));
        bool     caorg = d[i + 1] & 0x01;
        fic_packet_comp_t pc;
        pc.scid    = scid;
        pc.dg      = (d[i + 2] >> 7) & 0x01;
        pc.dscty   = d[i + 2] & 0x3F;
        pc.subch   = d[i + 3] >> 2;
        pc.address = (uint16_t)(((d[i + 3] & 0x03) << 8) | d[i + 4]);
        i += 5 + (caorg ? 2 : 0);

        int k;
        for (k = 0; k < s->n_packet && s->packet[k].scid != scid; k++) {}
        if (k == s->n_packet && s->n_packet < FIC_MAX_COMPS) s->n_packet++;
        if (k < FIC_MAX_COMPS) s->packet[k] = pc;
    }
}

/*
 * FIG 0/8 — Service component global definition. ETSI EN 300 401 §6.3.5.
 *   SId (16/32) | Ext flag (1) | rfa (3) | SCIdS (4)
 *   LS = 0: LS (1) | MSC/FIC (1) | SubChId (6)
 *   LS = 1: LS (1) | rfa (3) | SCId (12)
 *   [rfa (8) if Ext flag]
 */
static void parse_fig0_8(fic_state_t *s, const uint8_t *d, int len, bool pd)
{
    const int sid_len = pd ? 4 : 2;
    int i = 0;
    while (i + sid_len + 2 <= len) {
        fic_comp_def_t cd;
        memset(&cd, 0, sizeof(cd));
        cd.sid = read_sid(d + i, pd);
        i += sid_len;
        bool ext = d[i] >> 7;
        cd.scids = d[i] & 0x0F;
        i++;
        cd.long_form = d[i] >> 7;
        if (cd.long_form) {
            if (i + 2 > len) break;
            cd.scid = (uint16_t)(((d[i] & 0x0F) << 8) | d[i + 1]);
            i += 2;
        } else {
            cd.subch = d[i] & 0x3F;
            i++;
        }
        if (ext) i++;

        int k;
        for (k = 0; k < s->n_defs && !(s->defs[k].sid == cd.sid && s->defs[k].scids == cd.scids); k++) {}
        if (k == s->n_defs && s->n_defs < FIC_MAX_COMPS) s->n_defs++;
        if (k < FIC_MAX_COMPS) s->defs[k] = cd;
    }
}

/*
 * FIG 0/9 — Country, LTO and International table. ETSI EN 300 401 §8.1.3.2.
 *   Ext flag (1) | rfa (1) | Ensemble LTO (6) | Ensemble ECC (8) | ...
 */
static void parse_fig0_9(fic_state_t *s, const uint8_t *d, int len)
{
    if (len < 2) return;
    s->ecc     = d[1];
    s->has_ecc = true;
    s->lto     = (int8_t)((d[0] & 0x20) ? -(d[0] & 0x1F) : (d[0] & 0x1F));
    if (len >= 3) s->inter_table_id = d[2];

    /* Extended field (Ext flag): services whose ECC differs from the
     * ensemble's, in sub-fields of
     *   Number of services (2) | rfa (6) | ECC (8) | SId (16) x n */
    if (!(d[0] & 0x80) || len < 3) return;
    int i = 3;   /* after the International Table Id */
    while (i + 2 <= len) {
        int     n   = d[i] >> 6;
        uint8_t ecc = d[i + 1];
        i += 2;
        for (int k = 0; k < n && i + 2 <= len; k++, i += 2) {
            svc_info_t *sv = find_or_add(s, (uint16_t)((d[i] << 8) | d[i + 1]));
            if (sv) {
                sv->ecc     = ecc;
                sv->has_ecc = true;
            }
        }
    }
}

/*
 * FIG 0/18 — Announcement support. ETSI EN 300 401 §8.1.6.1.
 *   SId (16) | ASu flags (16) | rfa (3) | Number of clusters (5) | Cluster Id (8) x n
 */
static void parse_fig0_18(fic_state_t *s, const uint8_t *d, int len)
{
    int i = 0;
    while (i + 5 <= len) {
        uint16_t sid = (uint16_t)((d[i] << 8) | d[i + 1]);
        uint16_t asu = (uint16_t)((d[i + 2] << 8) | d[i + 3]);
        int      n   = d[i + 4] & 0x1F;
        i += 5;
        svc_info_t *sv = find_or_add(s, sid);
        if (sv) {
            sv->asu = asu;
            sv->n_clusters = 0;
        }
        for (int k = 0; k < n && i < len; k++, i++)
            if (sv && sv->n_clusters < sizeof(sv->clusters))
                sv->clusters[sv->n_clusters++] = d[i];
    }
}

/*
 * FIG 0/19 — Announcement switching. ETSI EN 300 401 §8.1.6.2.
 *   Cluster Id (8) | ASw flags (16) | New flag (1) | Region flag (1) | SubChId (6)
 *   [rfa (2) | Region Id lower part (6) if Region flag]
 */
static void parse_fig0_19(fic_state_t *s, const uint8_t *d, int len)
{
    int i = 0;
    while (i + 4 <= len) {
        fic_cluster_t *c = &s->clusters[d[i]];
        c->asw   = (uint16_t)((d[i + 1] << 8) | d[i + 2]);
        c->subch = d[i + 3] & 0x3F;
        c->seen  = s->fic_count;
        i += 4 + ((d[i + 3] & 0x40) ? 1 : 0);
    }
}

uint16_t fic_active_announcements(const fic_state_t *s, const svc_info_t *sv, int *subch)
{
    uint16_t active = 0;
    for (int k = 0; k < sv->n_clusters; k++) {
        const fic_cluster_t *c = &s->clusters[sv->clusters[k]];
        /* FIG 0/19 repeats about every second while an announcement runs */
        if (!c->asw || s->fic_count - c->seen > 84) continue;
        uint16_t a = c->asw & sv->asu;
        if (a && !active && subch) *subch = c->subch;
        active |= a;
    }
    return active;
}

/*
 * FIG 0/13 — User application information. ETSI EN 300 401 §6.3.6.
 *   SId (16/32) | SCIdS (4) | Number of user applications (4)
 *   per application: User Application Type (11) | data length (5) | data
 */
static void parse_fig0_13(fic_state_t *s, const uint8_t *d, int len, bool pd)
{
    const int sid_len = pd ? 4 : 2;
    int i = 0;
    while (i + sid_len + 1 <= len) {
        uint32_t sid = read_sid(d + i, pd);
        i += sid_len;
        uint8_t scids = d[i] >> 4;
        int     n_ua  = d[i] & 0x0F;
        i++;
        for (int u = 0; u < n_ua && i + 2 <= len; u++) {
            uint16_t type = (uint16_t)((d[i] << 3) | (d[i + 1] >> 5));
            int      dlen = d[i + 1] & 0x1F;
            i += 2 + dlen;

            int k;
            for (k = 0; k < s->n_uas; k++)
                if (s->uas[k].sid == sid && s->uas[k].scids == scids && s->uas[k].type == type) break;
            if (k == s->n_uas && s->n_uas < FIC_MAX_COMPS) {
                s->uas[k].sid   = sid;
                s->uas[k].scids = scids;
                s->uas[k].type  = type;
                s->n_uas++;
            }
        }
    }
}

int fic_find_packet_apps(const fic_state_t *s, uint16_t type, fic_app_comp_t *out, int max)
{
    int n = 0;
    for (int u = 0; u < s->n_uas && n < max; u++) {
        const fic_user_app_t *ua = &s->uas[u];
        if (ua->type != type) continue;

        /* (SId, SCIdS) -> SCId: FIG 0/8, or for the primary component FIG 0/2 */
        int scid = -1;
        for (int k = 0; k < s->n_defs; k++)
            if (s->defs[k].sid == ua->sid && s->defs[k].scids == ua->scids && s->defs[k].long_form)
                scid = s->defs[k].scid;
        if (scid < 0 && ua->scids == 0)
            for (int k = 0; k < s->n_prim; k++)
                if (s->prim[k].sid == ua->sid) scid = s->prim[k].scid;
        if (scid < 0) continue;

        /* SCId -> sub-channel and packet address: FIG 0/3 */
        for (int k = 0; k < s->n_packet; k++) {
            const fic_packet_comp_t *pc = &s->packet[k];
            if (pc->scid != scid || pc->dscty != DSCTY_MOT || pc->dg) continue;
            bool dup = false;
            for (int j = 0; j < n; j++)
                if (out[j].subch == pc->subch && out[j].address == pc->address) dup = true;
            if (dup) break;
            out[n].sid     = ua->sid;
            out[n].subch   = pc->subch;
            out[n].address = pc->address;
            n++;
            break;
        }
    }
    return n;
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
    if (len < 20)
        return;
    s->eid = ((uint16_t)d[0] << 8) | d[1];
    label_to_utf8(s->ens_label, d + 2, charset);
    short_label_to_utf8(s->ens_short_label, d + 2, (uint16_t)((d[18] << 8) | d[19]), charset);
    s->ens_charset   = charset;
    s->has_ens_label = true;
}

static void set_service_label(fic_state_t *s, uint32_t sid, bool pd, const uint8_t *d, uint8_t charset)
{
    fic_service_t *fs = find_or_add_service(s, sid, pd);
    if (!fs) return;
    label_to_utf8(fs->label, d, charset);
    short_label_to_utf8(fs->short_label, d, (uint16_t)((d[16] << 8) | d[17]), charset);
    fs->charset   = charset;
    fs->has_label = true;
}

/* FIG 1/5 — Data service label: SId (32) | label (128) | short label flags (16) */
static void parse_fig1_5(fic_state_t *s, const uint8_t *d, int len, uint8_t charset)
{
    if (len < 22) return;
    set_service_label(s, read_sid(d, true), true, d + 4, charset);
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
        set_service_label(s, sid, false, d + i, charset);
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
            uint8_t cn     = (common >> 7) & 0x01;
            uint8_t oe     = (common >> 6) & 0x01;
            uint8_t pd     = (common >> 5) & 0x01;
            uint8_t ext    = common & 0x1F;
            const uint8_t *fd = fig_data + 1;
            int            fl = fig_len - 1;
            s->fig_count[0][ext]++;
            if (oe) continue;   /* other ensemble */
            switch (ext) {
            case 0:  parse_fig0_0(s, fd, fl);      break;
            case 1:  if (!cn) parse_fig0_1(s, fd, fl); break;   /* current configuration only */
            case 5:  parse_fig0_5(s, fd, fl);      break;
            case 7:  parse_fig0_7(s, fd, fl);      break;
            case 10: parse_fig0_10(s, fd, fl);     break;
            case 14: if (!cn) parse_fig0_14(s, fd, fl); break;
            case 2:  parse_fig0_2(s, fd, fl, pd);  break;
            case 17: if (!pd) parse_fig0_17(s, fd, fl); break;
            case 18: if (!pd) parse_fig0_18(s, fd, fl); break;
            case 19: parse_fig0_19(s, fd, fl); break;
            case 3:  parse_fig0_3(s, fd, fl);      break;
            case 8:  parse_fig0_8(s, fd, fl, pd);  break;
            case 9:  parse_fig0_9(s, fd, fl);      break;
            case 13: parse_fig0_13(s, fd, fl, pd); break;
            default: break;
            }

        } else if (fig_type == 1 && fig_len >= 2) {
            uint8_t common = fig_data[0];
            uint8_t charset = (common >> 4) & 0x0F;
            uint8_t ext     = common & 0x07;
            s->fig_count[1][ext]++;
            if (common & 0x08) continue;   /* other ensemble */
            if (ext == 0)
                parse_fig1_0(s, fig_data + 1, fig_len - 1, charset);
            else if (ext == 1)
                parse_fig1_1(s, fig_data + 1, fig_len - 1, charset);
            else if (ext == 5)
                parse_fig1_5(s, fig_data + 1, fig_len - 1, charset);
        } else if (fig_len >= 1) {
            s->fig_count[fig_type][fig_type == 2 ? fig_data[0] & 0x07 : 0]++;
        }
    }
}

void fic_parse(fic_state_t *s, const uint8_t *fic, uint16_t fic_len)
{
    s->fic_count++;
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
