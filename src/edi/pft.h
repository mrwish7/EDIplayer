#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * PFT (Protection, Fragmentation and Transport) reassembly.
 * ETSI TS 102 821, section 7.
 *
 * EDI senders typically use "fragment spreading": fragments from consecutive
 * AF packets are interleaved so that a single burst loss affects at most one
 * fragment per packet. This requires holding PFT_WINDOW collectors open at
 * the same time.
 *
 * Reed-Solomon error correction is not implemented; all Fcount fragments must
 * arrive for a packet to be reassembled.
 */

#define PFT_WINDOW          8    /* concurrent Pseq collectors */
#define PFT_MAX_FRAGMENTS   64   /* fragments per AF packet (>64 is exotic) */
#define PFT_MAX_FRAG_PAYLOAD 1472 /* conservative UDP MTU */

typedef struct {
    uint16_t Plen;
    uint8_t  data[PFT_MAX_FRAG_PAYLOAD];
    bool     received;
} pft_frag_t;

typedef struct {
    bool       active;
    bool       complete;
    uint16_t   Pseq;
    uint16_t   Fcount;
    uint32_t   n_received;
    pft_frag_t frags[PFT_MAX_FRAGMENTS];
} pft_slot_t;

typedef struct {
    pft_slot_t slots[PFT_WINDOW];
} pft_ctx_t;

void pft_init(pft_ctx_t *ctx);

/* Push one raw PF UDP packet.
 * Returns 1 when any slot has all fragments (call pft_get_af_packet next),
 * 0 otherwise. */
int pft_push_fragment(pft_ctx_t *ctx, const uint8_t *pkt, size_t len);

/* Copy the oldest complete AF packet into buf.
 * Returns bytes written, or 0 if no complete packet is ready. */
size_t pft_get_af_packet(pft_ctx_t *ctx, uint8_t *buf, size_t buflen);
