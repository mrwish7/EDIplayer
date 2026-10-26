#include "pft.h"
#include <string.h>
#include "edi_log.h"
#include <stdio.h>

/*
 * PFT fragment header layout (ETSI TS 102 821 §7.4.1):
 *
 *  [0-1]   SYNC   = 'PF' (0x50, 0x46)
 *  [2-3]   Pseq   uint16 BE
 *  [4-6]   Findex uint24 BE  (0-based fragment index)
 *  [7-9]   Fcount uint24 BE  (total fragments for this AF packet)
 *  [10-11] b15=FEC  b14=Addr  b13-0=Plen (payload bytes)
 *  [12-13] RSk, RSz           (only if FEC=1)
 *  [12-15] or [14-17] Source, Dest (only if Addr=1, after FEC bytes if any)
 *
 * Fragment spreading: senders interleave fragments from N consecutive AF
 * packets so a single burst loss takes at most one fragment per packet.
 * We therefore keep PFT_WINDOW collectors open simultaneously.
 */

void pft_init(pft_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

/* Wraparound-safe uint16 "less than": is a older than b? */
static inline bool pseq_older(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b) < 0;
}

static pft_slot_t *find_or_alloc_slot(pft_ctx_t *ctx, uint16_t Pseq,
                                       uint16_t Fcount)
{
    /* 1. Already have a slot for this Pseq? */
    for (int i = 0; i < PFT_WINDOW; i++) {
        if (ctx->slots[i].active && ctx->slots[i].Pseq == Pseq)
            return &ctx->slots[i];
    }

    /* 2. Find a free (inactive) slot. */
    for (int i = 0; i < PFT_WINDOW; i++) {
        if (!ctx->slots[i].active) {
            memset(&ctx->slots[i], 0, sizeof(ctx->slots[i]));
            ctx->slots[i].active  = true;
            ctx->slots[i].Pseq    = Pseq;
            ctx->slots[i].Fcount  = Fcount;
            return &ctx->slots[i];
        }
    }

    /* 3. All slots full: evict the oldest incomplete slot. */
    pft_slot_t *evict = NULL;
    for (int i = 0; i < PFT_WINDOW; i++) {
        if (ctx->slots[i].complete) continue; /* keep complete ones */
        if (!evict || pseq_older(ctx->slots[i].Pseq, evict->Pseq))
            evict = &ctx->slots[i];
    }

    /* Fallback: if everything is marked complete, evict oldest overall. */
    if (!evict) {
        evict = &ctx->slots[0];
        for (int i = 1; i < PFT_WINDOW; i++) {
            if (pseq_older(ctx->slots[i].Pseq, evict->Pseq))
                evict = &ctx->slots[i];
        }
    }

    edi_log("PFT: window full, evicting Pseq %u (%u/%u fragments)\n",
            evict->Pseq, evict->n_received, evict->Fcount);

    memset(evict, 0, sizeof(*evict));
    evict->active = true;
    evict->Pseq   = Pseq;
    evict->Fcount = Fcount;
    return evict;
}

static bool any_complete(const pft_ctx_t *ctx)
{
    for (int i = 0; i < PFT_WINDOW; i++) {
        if (ctx->slots[i].active && ctx->slots[i].complete)
            return true;
    }
    return false;
}

int pft_push_fragment(pft_ctx_t *ctx, const uint8_t *pkt, size_t len)
{
    if (len < 14) return 0;
    if (pkt[0] != 'P' || pkt[1] != 'F') return 0;

    uint16_t Pseq   = (uint16_t)((pkt[2] << 8) | pkt[3]);
    uint32_t Findex = ((uint32_t)pkt[4] << 16) | ((uint32_t)pkt[5] << 8) | pkt[6];
    uint32_t Fcount = ((uint32_t)pkt[7] << 16) | ((uint32_t)pkt[8] << 8) | pkt[9];
    bool     fec    = (pkt[10] & 0x80) != 0;
    bool     addr   = (pkt[10] & 0x40) != 0;

    uint16_t Plen   = (uint16_t)(((pkt[10] & 0x3F) << 8) | pkt[11]);

    /* Minimum fixed header (14) = sync(2)+Pseq(2)+Findex(3)+Fcount(3)+flags+Plen(2)+Hcrc(2)
     * HCRC is always present per ETSI TS 102 821 §7.4.1 (cf. PFPACKET_HEADER_LEN=14) */
    size_t hdr_len = 14u + (fec ? 2u : 0u) + (addr ? 4u : 0u);

    if (len < hdr_len + Plen)                      return 0;
    if (Fcount == 0 || Fcount > PFT_MAX_FRAGMENTS) return 0;
    if (Findex >= Fcount)                          return 0;
    if (Plen > PFT_MAX_FRAG_PAYLOAD)               return 0;

    pft_slot_t *slot = find_or_alloc_slot(ctx, Pseq, (uint16_t)Fcount);
    if (!slot) return 0;

    if (slot->Fcount != (uint16_t)Fcount) {
        edi_log("PFT: Fcount mismatch for Pseq %u (%u vs %u)\n",
                Pseq, slot->Fcount, (uint16_t)Fcount);
        return 0;
    }

    if (!slot->frags[Findex].received) {
        slot->frags[Findex].Plen = Plen;
        memcpy(slot->frags[Findex].data, pkt + hdr_len, Plen);
        slot->frags[Findex].received = true;
        slot->n_received++;

        if (slot->n_received == slot->Fcount)
            slot->complete = true;
    }

    return any_complete(ctx) ? 1 : 0;
}

size_t pft_get_af_packet(pft_ctx_t *ctx, uint8_t *buf, size_t buflen)
{
    /* Return the oldest complete slot to preserve frame order. */
    pft_slot_t *oldest = NULL;
    for (int i = 0; i < PFT_WINDOW; i++) {
        if (!ctx->slots[i].active || !ctx->slots[i].complete) continue;
        if (!oldest || pseq_older(ctx->slots[i].Pseq, oldest->Pseq))
            oldest = &ctx->slots[i];
    }
    if (!oldest) return 0;

    size_t offset = 0;
    for (uint16_t i = 0; i < oldest->Fcount; i++) {
        if (!oldest->frags[i].received) {
            edi_log("PFT: gap in complete Pseq %u at Findex %u\n",
                    oldest->Pseq, i);
            oldest->active = false;
            return 0;
        }
        size_t plen = oldest->frags[i].Plen;
        if (offset + plen > buflen) {
            edi_log("PFT: reassembled packet too large for buffer\n");
            oldest->active = false;
            return 0;
        }
        memcpy(buf + offset, oldest->frags[i].data, plen);
        offset += plen;
    }

    oldest->active = false;
    return offset;
}
