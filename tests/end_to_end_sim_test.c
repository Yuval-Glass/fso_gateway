/*
 * tests/end_to_end_sim_test.c — Task 20: Full End-to-End Simulation.
 *
 * Pipeline under test:
 *   generate packets
 *       -> fragment_packet()       [packet -> source symbol_t units]
 *       -> block builder           [group K source symbols per block]
 *       -> fec_encode_block()      [produce M repair symbols per block]
 *       -> interleaver             [symbol-level matrix interleave]
 *       -> burst erasure           [zero out selected symbols]
 *       -> deinterleaver           [sparse block reassembly]
 *       -> fec_decode_block()      [recover source data]
 *       -> packet reassembly       [group source symbols by packet_id]
 *       -> byte-exact comparison   [verify against originals]
 *
 * ==========================================================================
 * Packet accounting model
 * ==========================================================================
 *
 *   generated   — packets created by the synthetic generator.
 *                 Only as many packets are generated as needed to fill
 *                 the SIM_NUM_BLOCKS * K source-symbol budget.  A packet
 *                 that would straddle the budget boundary is not generated.
 *
 *   transmitted — packets where at least one fragment was committed into
 *                 a source block that was encoded and interleaved.
 *                 Derived authoritatively from block_meta at the end of
 *                 the TX pipeline by scanning every slot with
 *                 payload_len > 0 and marking the owning packet_id.
 *
 *   recovered   — transmitted packets reassembled successfully.
 *
 *   exact_match — recovered packets whose bytes and length exactly equal
 *                 the original.
 *
 *   corrupted   — recovered − exact_match.
 *
 *   missing     — transmitted − recovered.  Computed at packet level from
 *                 the per-packet flags; never inferred from failed blocks.
 *
 * ==========================================================================
 * symbol_t dual-id design
 * ==========================================================================
 *
 *   The interleaver routes by (symbol->packet_id % depth) so it uses
 *   packet_id as a block sequence number.  We save the real packet_id in
 *   block_meta, overwrite packet_id with the block index before pushing
 *   into the interleaver, and restore it from block_meta after FEC decode.
 *
 * ==========================================================================
 * Build
 * ==========================================================================
 *   make e2etest
 *
 * Run
 * ==========================================================================
 *   ./build/bin/end_to_end_sim_test
 */

#define _POSIX_C_SOURCE 200112L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wirehair/wirehair.h>

#include "block_builder.h"
#include "deinterleaver.h"
#include "fec_wrapper.h"
#include "interleaver.h"
#include "logging.h"
#include "sim_runner.h"
#include "packet_fragmenter.h"
#include "packet_reassembler.h"
#include "stats.h"
#include "types.h"

/* ==========================================================================
 * Simulation geometry
 * ==========================================================================*/

#define SIM_K            16      /* source symbols per FEC block              */
#define SIM_M             8      /* repair symbols per FEC block              */
#define SIM_N            (SIM_K + SIM_M)    /* 24                            */

#define SIM_DEPTH         8      /* interleave window depth (blocks)          */
#define SIM_SYMBOL_SIZE  1500    /* bytes per symbol (uniform)                */

/* Number of complete interleave windows to simulate. */
#define SIM_NUM_WINDOWS   4
#define SIM_NUM_BLOCKS   (SIM_NUM_WINDOWS * SIM_DEPTH)   /* 32              */
#define SIM_WINDOW_SYMS  (SIM_DEPTH * SIM_N)             /* 192             */
#define SIM_TOTAL_SYMS   (SIM_NUM_BLOCKS * SIM_N)        /* 768             */

/* Total source symbol slots across all blocks. */
#define SIM_TOTAL_SRC_SLOTS  (SIM_NUM_BLOCKS * SIM_K)   /* 512             */

/* Maximum fragments one packet can produce: ceil(9000/1500) = 6. */
#define MAX_FRAGS_PER_PKT  8

/*
 * Burst erasure parameters.
 * SIM_BURST_LEN <= SIM_DEPTH * SIM_M guarantees FEC can recover all blocks.
 */
#define SIM_BURST_LEN    (SIM_DEPTH * SIM_M - 4)   /* 60 symbols           */
#define SIM_BURST_START   20

/* Deinterleaver slot headroom. */
#define DIL_HEADROOM     (SIM_DEPTH * 2)

/* ==========================================================================
 * Packet size table  (mix of small / MTU / jumbo)
 * ==========================================================================*/

static const size_t k_pkt_sizes[] = {
    64, 128, 512, 1500, 2048, 4096, 9000, 256, 1024, 3000
};
#define NUM_PKT_SIZES  ((int)(sizeof(k_pkt_sizes) / sizeof(k_pkt_sizes[0])))

/*
 * Upper bound on packets generated.
 * Worst case: all packets are 1 fragment (64 B), so packets == slots.
 */
#define MAX_PACKETS  (SIM_TOTAL_SRC_SLOTS + 8)

/* ==========================================================================
 * Per-packet record
 * ==========================================================================*/

typedef struct {
    uint32_t       packet_id;    /* 1-based, unique                          */
    size_t         packet_len;
    unsigned char *data;         /* heap-allocated                           */
    int            transmitted;  /* 1 iff at least one fragment was encoded  */
    int            recovered;    /* 1 iff fully reassembled after decode     */
    int            exact_match;  /* 1 iff recovered bytes == original        */
} pkt_record_t;

/* ==========================================================================
 * Per-block source-symbol metadata
 *
 * Saved at encode time.  After FEC decode we use this to reconstruct
 * symbol_t objects (with the correct original packet_id, symbol_index,
 * total_symbols, and payload_len) for the packet reassembler.
 * ==========================================================================*/

typedef struct {
    uint32_t  orig_packet_id;    /* true packet id (before il overwrite)     */
    uint16_t  symbol_index;      /* fragment index within the packet          */
    uint16_t  total_symbols;     /* total fragments this packet was split to  */
    uint16_t  payload_len;       /* 0 means padding slot — skip on reassembly */
} slot_meta_t;

typedef struct {
    uint64_t    block_id;        /* block_builder block_id                   */
    slot_meta_t slots[SIM_K];    /* one entry per source symbol slot          */
} block_meta_t;

/* ==========================================================================
 * Per-packet accumulator  (used during RX reassembly)
 * ==========================================================================*/

/* At most K fragments per block arrive together; two blocks can overlap. */
#define MAX_ACCUM  (SIM_K * 4)

typedef struct {
    uint32_t  packet_id;
    int       expected;          /* total_symbols of this packet              */
    int       count;             /* fragments stored so far                   */
    symbol_t  frags[MAX_FRAGS_PER_PKT];
} pkt_accum_t;

/* ==========================================================================
 * Simulation context
 * ==========================================================================*/

typedef struct {
    pkt_record_t  pkts[MAX_PACKETS];
    int           num_generated;
    int           num_transmitted;

    block_meta_t  block_meta[SIM_NUM_BLOCKS];
    int           num_blocks_encoded;

    symbol_t     *tx_buf;        /* SIM_TOTAL_SYMS entries, heap             */
    int           tx_count;

    int  stat_symbols_erased;
    int  stat_blocks_attempted;
    int  stat_blocks_ok;
    int  stat_blocks_failed;
} sim_t;

/* ==========================================================================
 * Utility: 64-byte-aligned zeroed allocation
 * ==========================================================================*/

static void *alloc_aligned(size_t n)
{
    void *p = NULL;

    if (n == 0) {
        return NULL;
    }

    if (posix_memalign(&p, 64U, n) != 0) {
        return NULL;
    }

    memset(p, 0, n);
    return p;
}

/* ==========================================================================
 * Accumulator helpers
 * ==========================================================================*/

static pkt_accum_t *accum_find_or_create(pkt_accum_t *tbl, int *cnt,
                                         uint32_t pid, int expected)
{
    int i;

    for (i = 0; i < *cnt; ++i) {
        if (tbl[i].packet_id == pid) {
            return &tbl[i];
        }
    }

    if (*cnt >= MAX_ACCUM) {
        return NULL;
    }

    tbl[*cnt].packet_id = pid;
    tbl[*cnt].expected  = expected;
    tbl[*cnt].count     = 0;
    return &tbl[(*cnt)++];
}

static void accum_remove(pkt_accum_t *tbl, int *cnt, int idx)
{
    if (idx < 0 || idx >= *cnt) {
        return;
    }

    memmove(&tbl[idx], &tbl[idx + 1],
            sizeof(pkt_accum_t) * (size_t)(*cnt - idx - 1));
    (*cnt)--;
}

/* ==========================================================================
 * Step A: Generate packets
 *
 * Generate exactly as many packets as needed to fill SIM_TOTAL_SRC_SLOTS.
 * A packet that would straddle the budget boundary is not generated,
 * so every generated packet can be fully admitted into the TX pipeline.
 * ==========================================================================*/

static int generate_packets(sim_t *ctx)
{
    int slots_remaining = SIM_TOTAL_SRC_SLOTS;
    int size_idx        = 0;
    int unique_id       = 1;

    ctx->num_generated = 0;

    while (slots_remaining > 0 && ctx->num_generated < MAX_PACKETS) {
        size_t        sz  = k_pkt_sizes[size_idx % NUM_PKT_SIZES];
        pkt_record_t *pr  = &ctx->pkts[ctx->num_generated];
        int           nf  = (int)((sz + SIM_SYMBOL_SIZE - 1) / SIM_SYMBOL_SIZE);
        size_t        j;

        size_idx++;

        /* Skip this packet if its fragments would exceed remaining budget. */
        if (nf > slots_remaining) {
            continue;
        }

        pr->packet_id   = (uint32_t)unique_id++;
        pr->packet_len  = sz;
        pr->transmitted = 0;
        pr->recovered   = 0;
        pr->exact_match = 0;

        pr->data = (unsigned char *)malloc(sz);
        if (!pr->data) {
            LOG_ERROR("[E2E] generate_packets: malloc failed idx=%d",
                      ctx->num_generated);
            return -1;
        }

        for (j = 0; j < sz; ++j) {
            pr->data[j] = (unsigned char)(
                (pr->packet_id * 251U + (uint32_t)j * 37U +
                 (uint32_t)(j >> 8)) & 0xFFU);
        }

        slots_remaining -= nf;
        ctx->num_generated++;
    }

    LOG_INFO("[E2E] Generated %d packets (%d src slots used)",
             ctx->num_generated, SIM_TOTAL_SRC_SLOTS - slots_remaining);
    return 0;
}

/* ==========================================================================
 * Steps B-E: TX pipeline
 *
 * Fragment -> block build -> FEC encode -> interleave.
 *
 * All SIM_NUM_BLOCKS are fully encoded.  Partial final block is
 * zero-padded.  Every packet whose fragments are placed into an encoded
 * block is marked transmitted.
 * ==========================================================================*/

/*
 * encode_one_block — called whenever a block is full (either naturally or
 * after padding).  Encodes the block, pushes all N symbols into il, and
 * drains any completed window into tx_buf.
 *
 * Returns 0 on success, -1 on error.
 */
static int encode_one_block(sim_t           *ctx,
                            block_builder_t *bb,
                            fec_handle_t     fec,
                            interleaver_t   *il,
                            unsigned char   *src_data,
                            symbol_t        *repair_buf)
{
    int blk_idx = ctx->num_blocks_encoded;
    int s, m;

    if (blk_idx >= SIM_NUM_BLOCKS) {
        LOG_ERROR("[E2E] encode_one_block: blk_idx=%d >= SIM_NUM_BLOCKS",
                  blk_idx);
        return -1;
    }

    ctx->block_meta[blk_idx].block_id = bb->block_id;

    /* Build contiguous source data: concatenate K symbol payloads. */
    for (s = 0; s < SIM_K; ++s) {
        memcpy(src_data + (size_t)s * SIM_SYMBOL_SIZE,
               bb->symbols[s].data, SIM_SYMBOL_SIZE);
    }

    /* Generate M repair symbols. */
    memset(repair_buf, 0, (size_t)SIM_M * sizeof(symbol_t));
    if (fec_encode_block(fec, src_data, repair_buf, SIM_M) != FEC_DECODE_OK) {
        LOG_ERROR("[E2E] fec_encode_block failed block %d", blk_idx);
        return -1;
    }

    /* Push K source symbols into interleaver.
     * packet_id is overwritten with block index for interleaver row routing.
     * payload_len is forced to SIM_SYMBOL_SIZE (padded).
     * fec_id is forced to the slot index even for padding symbols. */
    for (s = 0; s < SIM_K; ++s) {
        symbol_t sym      = bb->symbols[s];
        sym.packet_id     = (uint32_t)blk_idx;
        sym.fec_id        = (uint32_t)s;
        sym.total_symbols = (uint16_t)SIM_N;
        sym.payload_len   = (uint16_t)SIM_SYMBOL_SIZE;

        LOG_DEBUG("[TX] SRC blk=%d slot=%d fec_id=%u pkt=%u",
                  blk_idx, s,
                  (unsigned)sym.fec_id,
                  (unsigned)sym.packet_id);

        if (interleaver_push_symbol(il, &sym) < 0) {
            LOG_ERROR("[E2E] push src sym failed block=%d fec=%d", blk_idx, s);
            return -1;
        }
    }

    /* Push M repair symbols. */
    for (m = 0; m < SIM_M; ++m) {
        repair_buf[m].packet_id     = (uint32_t)blk_idx;
        repair_buf[m].fec_id        = (uint32_t)(SIM_K + m);
        repair_buf[m].total_symbols = (uint16_t)SIM_N;
        repair_buf[m].payload_len   = (uint16_t)SIM_SYMBOL_SIZE;

        LOG_DEBUG("[TX] REPAIR blk=%d idx=%d fec_id=%u",
                  blk_idx, m, (unsigned)repair_buf[m].fec_id);

        if (interleaver_push_symbol(il, &repair_buf[m]) < 0) {
            LOG_ERROR("[E2E] push rep sym failed block=%d idx=%d", blk_idx, m);
            return -1;
        }
    }

    ctx->num_blocks_encoded++;
    block_builder_reset(bb);

    LOG_INFO("[TX] encoded block=%d tx_count=%d ready=%d",
             blk_idx, ctx->tx_count, interleaver_is_ready(il));

    /* Drain any completed window(s) into tx_buf. */
    while (interleaver_is_ready(il)) {
        int pr;

        if (ctx->tx_count >= SIM_TOTAL_SYMS) {
            LOG_ERROR("[E2E] tx_buf overflow at block %d", blk_idx);
            return -1;
        }

        pr = interleaver_pop_ready_symbol(il, &ctx->tx_buf[ctx->tx_count]);
        if (pr < 0) {
            break;
        }
        ctx->tx_count++;
        if (pr == 1) {
            break;
        }
    }

    return 0;
}

static int run_tx_pipeline(sim_t *ctx)
{
    symbol_t       *frag_buf   = NULL;
    symbol_t       *repair_buf = NULL;
    unsigned char  *src_data   = NULL;
    interleaver_t  *il         = NULL;
    fec_handle_t    fec        = NULL;
    block_builder_t bb;
    int             result     = -1;
    int             block_slot = 0;   /* next free fec_id slot in current block (0..K-1) */
    int             pi;

    ctx->num_blocks_encoded = 0;
    ctx->tx_count           = 0;

    frag_buf    = (symbol_t *)calloc((size_t)MAX_FRAGS_PER_PKT, sizeof(symbol_t));
    repair_buf  = (symbol_t *)calloc((size_t)SIM_M, sizeof(symbol_t));
    src_data    = (unsigned char *)alloc_aligned((size_t)SIM_K * SIM_SYMBOL_SIZE);
    ctx->tx_buf = (symbol_t *)calloc((size_t)SIM_TOTAL_SYMS, sizeof(symbol_t));

    if (!frag_buf || !repair_buf || !src_data || !ctx->tx_buf) {
        LOG_ERROR("[E2E] run_tx_pipeline: allocation failed");
        goto cleanup;
    }

    memset(&bb, 0, sizeof(bb));
    if (block_builder_init(&bb, SIM_K) != 0) {
        LOG_ERROR("[E2E] block_builder_init failed");
        goto cleanup;
    }

    il = interleaver_create(SIM_DEPTH, SIM_N, SIM_SYMBOL_SIZE);
    if (!il) {
        LOG_ERROR("[E2E] interleaver_create failed");
        goto cleanup_bb;
    }

    fec = fec_create(SIM_K, SIM_SYMBOL_SIZE);
    if (!fec) {
        LOG_ERROR("[E2E] fec_create(tx) failed");
        goto cleanup_il;
    }

    /* ------------------------------------------------------------------ */
    /* Fragment each packet and feed into block builder.                  */
    /* ------------------------------------------------------------------ */
    for (pi = 0; pi < ctx->num_generated; ++pi) {
        pkt_record_t *pr = &ctx->pkts[pi];
        int           nf, fi;

        if (ctx->num_blocks_encoded >= SIM_NUM_BLOCKS) {
            break;
        }

        memset(frag_buf, 0, (size_t)MAX_FRAGS_PER_PKT * sizeof(symbol_t));

        nf = fragment_packet(pr->data, pr->packet_len,
                             pr->packet_id,
                             (uint16_t)SIM_SYMBOL_SIZE,
                             frag_buf,
                             (uint16_t)MAX_FRAGS_PER_PKT);
        if (nf < 0) {
            LOG_ERROR("[E2E] fragment_packet failed packet_id=%u",
                      pr->packet_id);
            goto cleanup_fec;
        }

        for (fi = 0; fi < nf; ++fi) {
            symbol_t     *sym = &frag_buf[fi];
            block_meta_t *bm;
            slot_meta_t  *sm;
            int           rc;

            if (ctx->num_blocks_encoded >= SIM_NUM_BLOCKS) {
                goto tx_done;
            }

            /* Assign fec_id = slot index within current block. */
            sym->fec_id = (uint32_t)block_slot;

            /* Zero-pad short payload to SIM_SYMBOL_SIZE for FEC uniformity.
             * payload_len retains the true byte count. */
            if (sym->payload_len < (uint16_t)SIM_SYMBOL_SIZE) {
                memset(sym->data + sym->payload_len, 0,
                       (size_t)(SIM_SYMBOL_SIZE - sym->payload_len));
            }

            /* Save metadata for post-decode reassembly. */
            bm = &ctx->block_meta[ctx->num_blocks_encoded];
            sm = &bm->slots[block_slot];
            sm->orig_packet_id = sym->packet_id;
            sm->symbol_index   = sym->symbol_index;
            sm->total_symbols  = sym->total_symbols;
            sm->payload_len    = sym->payload_len;

            rc = block_builder_add_symbol(&bb, sym);
            if (rc < 0) {
                LOG_ERROR("[E2E] block_builder_add_symbol failed "
                          "block=%d slot=%d", ctx->num_blocks_encoded,
                          block_slot);
                goto cleanup_fec;
            }

            block_slot++;

            if (rc == 1) {
                /* Block full — encode, interleave, reset. */
                if (encode_one_block(ctx, &bb, fec, il,
                                     src_data, repair_buf) != 0) {
                    goto cleanup_fec;
                }
                block_slot = 0;
            }
        }
    }

tx_done:
    /* ------------------------------------------------------------------ */
    /* Flush final partial block with zero-padding if needed.             */
    /* ------------------------------------------------------------------ */
    if (block_slot > 0 && ctx->num_blocks_encoded < SIM_NUM_BLOCKS) {
        /* Padding slots: slot_meta remains zeroed (payload_len == 0). */
        block_builder_finalize_with_padding(&bb);
        if (encode_one_block(ctx, &bb, fec, il,
                             src_data, repair_buf) != 0) {
            goto cleanup_fec;
        }
        block_slot = 0;
    }

    /* Drain any remaining interleaver content. */
    while (interleaver_is_ready(il)) {
        int pr2;

        if (ctx->tx_count >= SIM_TOTAL_SYMS) {
            LOG_ERROR("[E2E] tx_buf overflow during final drain");
            goto cleanup_fec;
        }

        pr2 = interleaver_pop_ready_symbol(il, &ctx->tx_buf[ctx->tx_count]);
        if (pr2 < 0) {
            break;
        }
        ctx->tx_count++;
        if (pr2 == 1) {
            break;
        }
    }

    LOG_INFO("[TX] FINAL tx_count=%d expected=%d",
             ctx->tx_count, SIM_TOTAL_SYMS);

    LOG_INFO("[E2E] TX pipeline: %d blocks encoded, %d tx symbols",
             ctx->num_blocks_encoded, ctx->tx_count);

    if (ctx->num_blocks_encoded != SIM_NUM_BLOCKS) {
        LOG_ERROR("[E2E] TX block count mismatch: got=%d expected=%d",
                  ctx->num_blocks_encoded, SIM_NUM_BLOCKS);
        goto cleanup_fec;
    }

    if (ctx->tx_count != SIM_TOTAL_SYMS) {
        LOG_ERROR("[E2E] TX symbol count mismatch: got=%d expected=%d",
                  ctx->tx_count, SIM_TOTAL_SYMS);
        goto cleanup_fec;
    }

    result = 0;

cleanup_fec:
    fec_destroy(fec);
cleanup_il:
    interleaver_destroy(il);
cleanup_bb:
    block_builder_destroy(&bb);
cleanup:
    free(frag_buf);
    free(repair_buf);
    free(src_data);
    return result;
}

/* ==========================================================================
 * Recount transmitted packets from block_meta (authoritative pass)
 *
 * Scans every slot in every encoded block.  A slot with payload_len > 0
 * contributed a real fragment; its orig_packet_id packet is marked
 * transmitted.  This is the single authoritative source of truth for
 * the "transmitted" counter — it is immune to early-exit edge cases in
 * the fragmentation loop.
 * ==========================================================================*/

static void recount_transmitted(sim_t *ctx)
{
    int b, s, p;

    /* Reset all transmitted flags. */
    for (p = 0; p < ctx->num_generated; ++p) {
        ctx->pkts[p].transmitted = 0;
    }

    for (b = 0; b < ctx->num_blocks_encoded; ++b) {
        block_meta_t *bm = &ctx->block_meta[b];
        for (s = 0; s < SIM_K; ++s) {
            slot_meta_t *sm = &bm->slots[s];
            if (sm->payload_len == 0) {
                continue;
            }

            for (p = 0; p < ctx->num_generated; ++p) {
                if (ctx->pkts[p].packet_id == sm->orig_packet_id) {
                    ctx->pkts[p].transmitted = 1;
                    break;
                }
            }
        }
    }

    ctx->num_transmitted = 0;
    for (p = 0; p < ctx->num_generated; ++p) {
        if (ctx->pkts[p].transmitted) {
            ctx->num_transmitted++;
        }
    }

    LOG_INFO("[E2E] Transmitted: %d / %d generated",
             ctx->num_transmitted, ctx->num_generated);
}

/* ==========================================================================
 * Step F: Burst erasure
 * ==========================================================================*/

static void apply_burst_erasure(sim_t *ctx, int start, int len)
{
    int end = start + len;
    int i;

    if (start >= ctx->tx_count) {
        return;
    }
    if (end > ctx->tx_count) {
        end = ctx->tx_count;
    }

    for (i = start; i < end; ++i) {
        memset(&ctx->tx_buf[i], 0, sizeof(symbol_t));
    }

    ctx->stat_symbols_erased = end - start;
    LOG_INFO("[E2E] Burst erasure: start=%d len=%d (erased=%d of %d)",
             start, len, ctx->stat_symbols_erased, ctx->tx_count);
}

/* ==========================================================================
 * deliver_block
 *
 * Called after a block is successfully FEC-decoded.  Re-builds source
 * symbol_t objects from the raw reconstructed bytes and saved slot_meta,
 * feeds them into per-packet accumulators, and when a packet is complete
 * calls reassemble_packet() and records the outcome.
 * ==========================================================================*/

static void deliver_block(sim_t         *ctx,
                          int            blk_idx,
                          unsigned char *recon,
                          pkt_accum_t   *accum,
                          int           *accum_count)
{
    block_meta_t *bm = &ctx->block_meta[blk_idx];
    int           s;

    for (s = 0; s < SIM_K; ++s) {
        slot_meta_t *sm = &bm->slots[s];
        pkt_accum_t *pa;
        int          ai;

        if (sm->payload_len == 0) {
            continue;
        }

        /* Reconstruct symbol_t from decoded bytes + saved metadata. */
        symbol_t sym;
        memset(&sym, 0, sizeof(sym));
        sym.packet_id     = sm->orig_packet_id;
        sym.fec_id        = (uint32_t)s;
        sym.symbol_index  = sm->symbol_index;
        sym.total_symbols = sm->total_symbols;
        sym.payload_len   = sm->payload_len;
        memcpy(sym.data,
               recon + (size_t)s * SIM_SYMBOL_SIZE,
               sm->payload_len);

        pa = accum_find_or_create(accum, accum_count,
                                  sm->orig_packet_id,
                                  (int)sm->total_symbols);
        if (!pa) {
            LOG_WARN("[E2E] accum full — dropping frag packet_id=%u",
                     sm->orig_packet_id);
            continue;
        }

        if (pa->count >= MAX_FRAGS_PER_PKT) {
            LOG_WARN("[E2E] too many frags for packet_id=%u",
                     sm->orig_packet_id);
            continue;
        }

        pa->frags[pa->count++] = sym;

        /* Packet complete? */
        if (pa->count == pa->expected) {
            unsigned char recon_pkt[9000];
            int           recon_len;
            int           p;

            ai = (int)(pa - accum);   /* index before potential pointer move */

            memset(recon_pkt, 0, sizeof(recon_pkt));
            recon_len = reassemble_packet(pa->frags,
                                          (uint16_t)pa->count,
                                          recon_pkt,
                                          sizeof(recon_pkt));

            /* Find and update the packet record. */
            for (p = 0; p < ctx->num_generated; ++p) {
                pkt_record_t *pr = &ctx->pkts[p];
                if (pr->packet_id != sm->orig_packet_id) {
                    continue;
                }

                if (recon_len > 0) {
                    pr->recovered = 1;
                    if ((size_t)recon_len == pr->packet_len &&
                        memcmp(recon_pkt, pr->data, pr->packet_len) == 0) {
                        pr->exact_match = 1;
                    } else {
                        LOG_WARN("[E2E] CORRUPT packet_id=%u "
                                 "orig_len=%zu recon_len=%d",
                                 pr->packet_id, pr->packet_len, recon_len);
                    }
                } else {
                    LOG_WARN("[E2E] reassemble_packet failed "
                             "packet_id=%u", pr->packet_id);
                }
                break;
            }

            accum_remove(accum, accum_count, ai);
        }
    }
}

/* ==========================================================================
 * Steps G-J: RX pipeline
 *
 * Deinterleave -> FEC decode -> reassemble -> compare.
 * ==========================================================================*/

static int run_rx_pipeline(sim_t *ctx)
{
    deinterleaver_t *dil         = NULL;
    fec_handle_t     fec         = NULL;
    unsigned char   *recon       = NULL;
    pkt_accum_t     *accum       = NULL;
    int              accum_count = 0;
    int              result      = -1;
    int              i;

    dil = deinterleaver_create(ctx->num_blocks_encoded + DIL_HEADROOM,
                               SIM_N, SIM_K,
                               SIM_SYMBOL_SIZE,
                               0.0,   /* stabilization_ms — disabled          */
                               0.0);  /* block_max_age_ms — use tick(0.0)     */
    if (!dil) {
        LOG_ERROR("[E2E] deinterleaver_create failed");
        return -1;
    }

    fec = fec_create(SIM_K, SIM_SYMBOL_SIZE);
    if (!fec) {
        LOG_ERROR("[E2E] fec_create(rx) failed");
        goto cleanup;
    }

    recon = (unsigned char *)alloc_aligned((size_t)SIM_K * SIM_SYMBOL_SIZE);
    if (!recon) {
        LOG_ERROR("[E2E] alloc_aligned(recon) failed");
        goto cleanup;
    }

    accum = (pkt_accum_t *)calloc((size_t)MAX_ACCUM, sizeof(pkt_accum_t));
    if (!accum) {
        LOG_ERROR("[E2E] calloc(accum) failed");
        goto cleanup;
    }

    /* ------------------------------------------------------------------ */
    /* Push surviving symbols into deinterleaver.                         */
    /* ------------------------------------------------------------------ */
    for (i = 0; i < ctx->tx_count; ++i) {
        symbol_t *sym = &ctx->tx_buf[i];
        int       rc;
        block_t   blk;

        if (sym->payload_len == 0) {
            stats_record_symbol(true);
            continue;
        }

        stats_record_symbol(false);
        rc = deinterleaver_push_symbol(dil, sym);

        if (rc < 0) {
            /*
             * Pool exhausted — drain ready blocks to free slots, retry.
             */
            while (deinterleaver_get_ready_block(dil, &blk) == 0) {
                int      blk_idx = (int)blk.block_id;
                uint64_t holes   = 0U;

                if (blk.symbols_per_block > blk.symbol_count) {
                    holes = (uint64_t)(blk.symbols_per_block - blk.symbol_count);
                }

                ctx->stat_blocks_attempted++;
                stats_record_block(holes);
                memset(recon, 0, (size_t)SIM_K * SIM_SYMBOL_SIZE);

                if (fec_decode_block(fec,
                                     blk.symbols,
                                     blk.symbol_count,
                                     blk.symbols_per_block,
                                     recon) == FEC_DECODE_OK) {
                    ctx->stat_blocks_ok++;
                    if (blk_idx >= 0 &&
                        blk_idx < ctx->num_blocks_encoded) {
                        deliver_block(ctx, blk_idx, recon,
                                      accum, &accum_count);
                    }
                    deinterleaver_mark_result(dil,
                                              (uint32_t)blk.block_id, 1);
                } else {
                    ctx->stat_blocks_failed++;
                    deinterleaver_mark_result(dil,
                                              (uint32_t)blk.block_id, 0);
                }
            }

            rc = deinterleaver_push_symbol(dil, sym);
            if (rc < 0) {
                LOG_WARN("[E2E] symbol permanently rejected i=%d "
                         "block_id=%u fec_id=%u",
                         i,
                         (unsigned)sym->packet_id,
                         (unsigned)sym->fec_id);
            }
        }
    }

    stats_finalize_burst();

    /* ------------------------------------------------------------------ */
    /* End-of-stream flush.                                               */
    /* ------------------------------------------------------------------ */
    deinterleaver_tick(dil, 0.0);

    {
        block_t blk;
        while (deinterleaver_get_ready_block(dil, &blk) == 0) {
            int      blk_idx = (int)blk.block_id;
            uint64_t holes   = 0U;

            if (blk.symbols_per_block > blk.symbol_count) {
                holes = (uint64_t)(blk.symbols_per_block - blk.symbol_count);
            }

            ctx->stat_blocks_attempted++;
            stats_record_block(holes);
            memset(recon, 0, (size_t)SIM_K * SIM_SYMBOL_SIZE);

            if (fec_decode_block(fec,
                                 blk.symbols,
                                 blk.symbol_count,
                                 blk.symbols_per_block,
                                 recon) == FEC_DECODE_OK) {
                ctx->stat_blocks_ok++;
                if (blk_idx >= 0 &&
                    blk_idx < ctx->num_blocks_encoded) {
                    deliver_block(ctx, blk_idx, recon,
                                  accum, &accum_count);
                }
                deinterleaver_mark_result(dil,
                                          (uint32_t)blk.block_id, 1);
            } else {
                ctx->stat_blocks_failed++;
                deinterleaver_mark_result(dil,
                                          (uint32_t)blk.block_id, 0);
            }
        }
    }

    result = 0;

cleanup:
    free(accum);
    free(recon);
    fec_destroy(fec);
    deinterleaver_destroy(dil);
    return result;
}

/* ==========================================================================
 * Step K: Final report
 * ==========================================================================*/

static void print_report(const sim_t *ctx)
{
    int generated   = ctx->num_generated;
    int transmitted = ctx->num_transmitted;
    int recovered   = 0;
    int exact_match = 0;
    int corrupted   = 0;
    int missing     = 0;
    int p;

    for (p = 0; p < ctx->num_generated; ++p) {
        const pkt_record_t *pr = &ctx->pkts[p];
        if (!pr->transmitted) {
            continue;
        }

        if (pr->recovered) {
            recovered++;
            if (pr->exact_match) {
                exact_match++;
            } else {
                corrupted++;
            }
        } else {
            missing++;
        }
    }

    {
        double recovery_pct   = transmitted > 0
                              ? 100.0 * recovered / transmitted : 0.0;
        double exactmatch_pct = transmitted > 0
                              ? 100.0 * exact_match / transmitted : 0.0;
        int all_ok = (transmitted > 0 &&
                      missing == 0 &&
                      corrupted == 0 &&
                      exact_match == transmitted);

        printf("\n");
        printf("================================================================\n");
        printf("  End-to-End Simulation Report — Task 20\n");
        printf("  K=%d  M=%d  N=%d  Depth=%d  SymSize=%d  Blocks=%d\n",
               SIM_K, SIM_M, SIM_N, SIM_DEPTH, SIM_SYMBOL_SIZE,
               ctx->num_blocks_encoded);
        printf("  Burst: start=%d len=%d\n", SIM_BURST_START, SIM_BURST_LEN);
        printf("================================================================\n");
        printf("  Packets generated     : %d\n",   generated);
        printf("  Packets transmitted   : %d\n",   transmitted);
        printf("  Packets recovered     : %d\n",   recovered);
        printf("  Packets exact match   : %d\n",   exact_match);
        printf("  Packets corrupted     : %d\n",   corrupted);
        printf("  Packets missing       : %d\n",   missing);
        printf("  Recovery rate         : %.1f%%  (recovered / transmitted)\n",
               recovery_pct);
        printf("  Exact-match rate      : %.1f%%  (exact_match / transmitted)\n",
               exactmatch_pct);
        printf("----------------------------------------------------------------\n");
        printf("  Symbols erased        : %d\n",   ctx->stat_symbols_erased);
        printf("  Blocks attempted      : %d\n",   ctx->stat_blocks_attempted);
        printf("  Blocks decoded OK     : %d\n",   ctx->stat_blocks_ok);
        printf("  Blocks failed         : %d\n",   ctx->stat_blocks_failed);
        printf("================================================================\n");
        printf("  RESULT: %s\n", all_ok ? "PASS" : "FAIL");
        printf("================================================================\n\n");
    }
}

/* ==========================================================================
 * Finalize statistics from authoritative simulation truth
 * ==========================================================================*/

static void finalize_packet_stats(sim_t *ctx)
{
    int p;
    int i;

    if (ctx == NULL) {
        return;
    }

    for (p = 0; p < ctx->num_generated; ++p) {
        pkt_record_t *pr = &ctx->pkts[p];

        stats_inc_ingress(pr->packet_len);

        if (pr->transmitted) {
            stats_inc_transmitted(pr->packet_len);
        }

        if (pr->recovered) {
            stats_inc_recovered(pr->packet_len);
        }

        if (pr->transmitted && !pr->recovered) {
            stats_inc_failed_packet();
        }
    }

    for (i = 0; i < ctx->stat_blocks_attempted; ++i) {
        stats_inc_block_attempt();
    }

    for (i = 0; i < ctx->stat_blocks_ok; ++i) {
        stats_inc_block_success();
    }

    for (i = 0; i < ctx->stat_blocks_failed; ++i) {
        stats_inc_block_failure();
    }
}

/* ==========================================================================
 * sim_runner integration glue
 *
 * Task 20 remains source-of-truth and still executes its original internal
 * TX/RX pipeline.  We initialize shared runtime state through sim_runner so
 * campaign and Task 20 tests use the same global setup path.
 * ==========================================================================*/

static int init_task20_runtime(void)
{
    if (sim_runner_global_init() != 0) {
        return -1;
    }

    stats_set_burst_fec_span((uint64_t)(SIM_M * SIM_DEPTH));
    return 0;
}

/* ==========================================================================
 * main
 * ==========================================================================*/

int main(void)
{
    sim_t ctx;
    int   rc;
    int   exit_code = 1;
    int   p;

    memset(&ctx, 0, sizeof(ctx));

    log_init();

    printf("================================================================\n");
    printf("  FSO Gateway — Task 20: Full End-to-End Simulation\n");
    printf("  K=%d  M=%d  N=%d  Depth=%d  SymSize=%d\n",
           SIM_K, SIM_M, SIM_N, SIM_DEPTH, SIM_SYMBOL_SIZE);
    printf("  Blocks=%d  Windows=%d  BurstLen=%d  BurstStart=%d\n",
           SIM_NUM_BLOCKS, SIM_NUM_WINDOWS, SIM_BURST_LEN, SIM_BURST_START);
    printf("================================================================\n\n");

    if (init_task20_runtime() != 0) {
        fprintf(stderr, "[E2E] FATAL: sim_runner_global_init() failed\n");
        return 1;
    }

    /* Step A */
    printf("Step A: Generating packets...\n");
    if (generate_packets(&ctx) != 0) {
        fprintf(stderr, "[E2E] generate_packets failed\n");
        goto done;
    }
    printf("  -> %d packets generated\n\n", ctx.num_generated);

    /* Steps B-E */
    printf("Steps B-E: TX pipeline...\n");
    if (run_tx_pipeline(&ctx) != 0) {
        fprintf(stderr, "[E2E] run_tx_pipeline failed\n");
        goto done;
    }
    printf("  -> %d blocks, %d tx symbols\n\n",
           ctx.num_blocks_encoded, ctx.tx_count);

    /* Authoritative transmitted count from block_meta. */
    recount_transmitted(&ctx);
    printf("  -> %d / %d packets transmitted\n\n",
           ctx.num_transmitted, ctx.num_generated);

    /* Step F */
    printf("Step F: Burst erasure (start=%d len=%d)...\n",
           SIM_BURST_START, SIM_BURST_LEN);
    apply_burst_erasure(&ctx, SIM_BURST_START, SIM_BURST_LEN);
    printf("  -> %d symbols erased\n\n", ctx.stat_symbols_erased);

    /* Steps G-J */
    printf("Steps G-J: RX pipeline...\n");
    if (run_rx_pipeline(&ctx) != 0) {
        fprintf(stderr, "[E2E] run_rx_pipeline failed\n");
        goto done;
    }
    printf("  -> %d blocks OK, %d failed\n\n",
           ctx.stat_blocks_ok, ctx.stat_blocks_failed);

    /* Step K */
    print_report(&ctx);

    finalize_packet_stats(&ctx);
    stats_report();

    /* exit code */
    {
        int exact = 0, miss = 0;
        for (p = 0; p < ctx.num_generated; ++p) {
            if (!ctx.pkts[p].transmitted) {
                continue;
            }
            if (ctx.pkts[p].exact_match) {
                exact++;
            } else if (!ctx.pkts[p].recovered) {
                miss++;
            }
        }
        exit_code = (ctx.num_transmitted > 0 &&
                     miss  == 0 &&
                     exact == ctx.num_transmitted) ? 0 : 1;
    }

    rc = exit_code;   /* suppress unused-variable warning */
    (void)rc;

done:
    for (p = 0; p < ctx.num_generated; ++p) {
        free(ctx.pkts[p].data);
    }
    free(ctx.tx_buf);
    return exit_code;
}