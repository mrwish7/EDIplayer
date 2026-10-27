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
    uint8_t  pty;                    /* international programme type, FIG 0/17 */
    bool     has_pty;
    uint8_t  ecc;                    /* service ECC if it differs from the ensemble's (FIG 0/9 ext.) */
    bool     has_ecc;
    uint16_t asu;                    /* announcement support flags, FIG 0/18 */
    uint8_t  clusters[8];            /* announcement clusters it belongs to */
    uint8_t  n_clusters;
} svc_info_t;

/* Announcement switching in a cluster, FIG 0/19 */
typedef struct {
    uint16_t asw;        /* active announcement types (0 = none) */
    uint8_t  subch;      /* sub-channel carrying the announcement */
    uint32_t seen;       /* fic_state_t.fic_count when last signalled */
} fic_cluster_t;

/* Sub-channel protection, FIG 0/1 */
typedef struct {
    bool    valid;
    bool    eep;        /* long form: Equal Error Protection */
    uint8_t level;      /* EEP 1..4, UEP 1..5 */
    bool    eep_b;      /* EEP option 1 (profile B) */
} fic_subch_t;

/*
 * Data components, for finding SPI (Service and Programme Information):
 *   FIG 0/2  primary component of each service (incl. data services, 32-bit SId)
 *   FIG 0/3  packet mode component: SCId -> sub-channel, packet address, DSCTy
 *   FIG 0/8  component global definition: (SId, SCIdS) -> SCId / sub-channel
 *   FIG 0/13 user applications of a component: (SId, SCIdS) -> type (7 = SPI)
 */
#define FIC_MAX_COMPS    96
#define UATYPE_SLS       0x002
#define UATYPE_SPI       0x007
#define DSCTY_MOT        60

typedef struct {
    uint16_t scid;          /* service component id (12 bit) */
    uint8_t  subch;
    uint16_t address;       /* packet address */
    uint8_t  dscty;
    bool     dg;            /* true: data groups NOT used (DG flag = 1) */
} fic_packet_comp_t;

typedef struct {
    uint32_t sid;           /* 16-bit (programme) or 32-bit (data) SId */
    uint8_t  scids;
    bool     long_form;     /* scid valid, else subch */
    uint16_t scid;
    uint8_t  subch;
} fic_comp_def_t;

typedef struct {
    uint32_t sid;
    uint8_t  scids;
    uint16_t type;
} fic_user_app_t;

typedef struct {
    uint32_t sid;
    uint16_t scid;          /* primary packet-mode component (TMId 3) */
} fic_primary_packet_t;

typedef struct {
    svc_info_t svcs[FIC_MAX_SVCS];
    int        n_svcs;
    uint16_t   eid;
    char       ens_label[FIC_LABEL_MAX]; /* UTF-8 */
    bool       has_ens_label;
    uint8_t    ecc;                      /* Extended Country Code, FIG 0/9 */
    bool       has_ecc;
    fic_subch_t subch[64];               /* by SubChId */
    fic_cluster_t clusters[256];         /* announcement switching, by cluster id */
    uint32_t   fic_count;                /* fic_parse() calls (one per 24 ms frame) */

    fic_packet_comp_t    packet[FIC_MAX_COMPS];
    int                  n_packet;
    fic_comp_def_t       defs[FIC_MAX_COMPS];
    int                  n_defs;
    fic_user_app_t       uas[FIC_MAX_COMPS];
    int                  n_uas;
    fic_primary_packet_t prim[FIC_MAX_COMPS];
    int                  n_prim;
} fic_state_t;

/* A packet mode component carrying a user application */
typedef struct {
    uint32_t sid;
    uint8_t  subch;
    uint16_t address;
} fic_app_comp_t;

/* Find the packet mode MOT components signalled with user application
 * `type` (e.g. UATYPE_SPI). Returns the number written to out. */
int fic_find_packet_apps(const fic_state_t *s, uint16_t type, fic_app_comp_t *out, int max);

void fic_init(fic_state_t *s);

/* Feed one frame's FIC data (raw FIBs including CRC bytes).
 * Call repeatedly across frames until fic_complete() returns true. */
void fic_parse(fic_state_t *s, const uint8_t *fic, uint16_t fic_len);

/* Returns true when every known SCID has a label and the ensemble label
 * has been seen. n_streams is the
 * number of EST streams seen so we know when coverage is sufficient. */
bool fic_complete(const fic_state_t *s, int n_streams);

/* Announcement types active for a service now (ASw & ASu over its clusters,
 * signalled within the last ~2 s), and the sub-channel carrying it. */
uint16_t fic_active_announcements(const fic_state_t *s, const svc_info_t *sv, int *subch);

/* Look up a service by SCID. Returns NULL if not found. */
const svc_info_t *fic_find_by_scid(const fic_state_t *s, uint8_t scid);

/* Convert len bytes of DAB-encoded text to NUL-terminated UTF-8.
 * charset: 0 = EBU Latin, 6 = UCS-2 BE, 15 = UTF-8 (others copied raw).
 * Output is truncated to fit dst_size. Returns bytes written (excl. NUL). */
size_t dab_text_to_utf8(char *dst, size_t dst_size,
                        const uint8_t *src, size_t len, uint8_t charset);
