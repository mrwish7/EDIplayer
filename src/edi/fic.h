#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Minimal FIC (Fast Information Channel) parser.
 * Extracts only what is needed to annotate --list-services output:
 *
 *   FIG type 0, extension 2 — Service Organisation
 *     Maps SId (Service Identifier) → SubChId (= SCID in the EST tag)
 *
 *   FIG type 1, extension 0 — Ensemble Label
 *     EId → 16-character EBU Latin label string
 *
 *   FIG type 1, extension 1 — Programme Service Label
 *     Maps SId → 16-character EBU Latin label string
 *
 * References: ETSI EN 300 401 §6.3.1 (FIG 0/2), §8.1.13 (FIG 1/0)
 * and §8.1.14.1 (FIG 1/1).
 */

#define FIC_MAX_SVCS 64
#define FIC_LABEL_MAX (16 * 3 + 1) /* 16 chars as UTF-8 (≤3 bytes each) + NUL */

/* Audio Service Component Types (FIG 0/2, TMId 0) */
#define ASCTY_MP2      0
#define ASCTY_DABPLUS  63

typedef struct {
    uint16_t sid;
    uint8_t  scid;                   /* sub-channel of the primary component */
    uint8_t  tmid;                   /* 0 = MSC stream audio, 1 = MSC stream data */
    uint8_t  ascty;                  /* audio component type (tmid 0) */
    char     label[FIC_LABEL_MAX];   /* UTF-8 */
    bool     has_scid;
    bool     has_label;
} svc_info_t;

typedef struct {
    svc_info_t svcs[FIC_MAX_SVCS];
    int        n_svcs;
    uint16_t   eid;
    char       ens_label[FIC_LABEL_MAX]; /* UTF-8 */
    bool       has_ens_label;
} fic_state_t;

void fic_init(fic_state_t *s);

/* Feed one frame's FIC data (raw FIBs including CRC bytes).
 * Call repeatedly across frames until fic_complete() returns true. */
void fic_parse(fic_state_t *s, const uint8_t *fic, uint16_t fic_len);

/* Returns true when every known SCID has a label and the ensemble label
 * has been seen. n_streams is the
 * number of EST streams seen so we know when coverage is sufficient. */
bool fic_complete(const fic_state_t *s, int n_streams);

/* Look up a service by SCID. Returns NULL if not found. */
const svc_info_t *fic_find_by_scid(const fic_state_t *s, uint8_t scid);

/* Convert len bytes of DAB-encoded text to NUL-terminated UTF-8.
 * charset: 0 = EBU Latin, 6 = UCS-2 BE, 15 = UTF-8 (others copied raw).
 * Output is truncated to fit dst_size. Returns bytes written (excl. NUL). */
size_t dab_text_to_utf8(char *dst, size_t dst_size,
                        const uint8_t *src, size_t len, uint8_t charset);
