
/* tests/sim_runner.c — Reusable FSO pipeline engine.
 *
 * Implements sim_run_campaign_case().
 * Logic extracted and parameterised from end_to_end_sim_test.c (Task 20).
 *
 * Stats integration:
 *   TX:      stats_inc_ingress(), stats_inc_transmitted()
 *   Channel: stats_record_symbol(), stats_finalize_burst()
 *   RX:      stats_inc_recovered(), stats_inc_failed_packet(),
 *            stats_inc_block_attempt(), stats_inc_block_success(),
 *            stats_inc_block_failure(), stats_record_block()
 *
 * All metrics in sim_result_t come from real pipeline execution only.
 */

#define _POSIX_C_SOURCE 200112L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <wirehair/wirehair.h>

#include "block_builder.h"
#include "deinterleaver.h"
#include "fec_wrapper.h"
#include "interleaver.h"
#include "logging.h"
#include "packet_fragmenter.h"
#include "packet_reassembler.h"
#include "stats.h"
#include "types.h"

#include "sim_runner.h"

/* extra telemetry hooks implemented in src/fec_wrapper.c */
typedef struct fec_decode_telemetry_t {
    uint64_t block_id;
    int      number_of_symbols_received;
    int      number_of_missing_symbols;
    int      number_of_repair_symbols_used;
    int      decode_success;
} fec_decode_telemetry_t;

extern void fec_set_current_decode_block_id(uint64_t block_id);
extern void fec_get_last_decode_telemetry(fec_decode_telemetry_t *out);

#define SR_MAX_FRAGS_PER_PKT   8
#define SR_MAX_ACCUM_FACTOR    4
#define SR_DIL_HEADROOM_FACTOR 2

enum {
    SR_FAIL_NONE = 0,
    SR_FAIL_TOO_MANY_HOLES = 1,
    SR_FAIL_INSUFFICIENT_SYMBOLS = 2,
    SR_FAIL_PACKET_AFTER_BLOCK_DECODE = 3
};

typedef struct {
    uint64_t blocks_attempted;
    uint64_t blocks_passed;
    uint64_t blocks_failed;

    uint64_t fail_too_many_holes;
    uint64_t fail_insufficient_symbols;

    uint64_t packet_fail_missing;
    uint64_t packet_fail_corrupted;
    uint64_t packet_fail_after_successful_block_decode;

    uint64_t max_missing_symbols_in_block;
    double   avg_missing_symbols_in_failed_blocks;

    uint64_t failed_missing_sum;
    uint64_t failed_missing_count;
} sr_run_report_t;

static sr_run_report_t g_last_run_report;

void sim_runner_get_last_run_report(sr_run_report_t *out)
{
    if (out == NULL) {
        return;
    }

    *out = g_last_run_report;
}

const char *sim_runner_failure_reason_name(int reason)
{
    switch (reason) {
        case SR_FAIL_TOO_MANY_HOLES:
            return "too_many_holes";
        case SR_FAIL_INSUFFICIENT_SYMBOLS:
            return "insufficient_symbols";
        case SR_FAIL_PACKET_AFTER_BLOCK_DECODE:
            return "packet_fail_after_block_decode";
        default:
            return "none";
    }
}

typedef struct {
    uint32_t       packet_id;
    size_t         packet_len;
    unsigned char *data;
    int            transmitted;
    int            recovered;
    int            exact_match;
} sr_pkt_record_t;

typedef struct {
    uint32_t orig_packet_id;
    uint16_t symbol_index;
    uint16_t total_symbols;
    uint16_t payload_len;
} sr_slot_meta_t;

typedef struct {
    uint64_t        block_id;
    sr_slot_meta_t *slots;
} sr_block_meta_t;

typedef struct {
    int      active;
    uint32_t packet_id;
    int      expected;
    int      count;
    symbol_t frags[SR_MAX_FRAGS_PER_PKT];
} sr_pkt_accum_t;

typedef struct {
    int k;
    int m;
    int n;
    int depth;
    int symbol_size;
    int num_blocks;
    int total_syms;
    int total_src_slots;

    sr_pkt_record_t *pkts;
    int              num_generated;
    int              num_transmitted;

    sr_block_meta_t *block_meta;
    int              num_blocks_encoded;

    symbol_t *tx_buf;
    int       tx_count;

    uint64_t stat_symbols_erased;
    uint64_t stat_symbols_corrupted;
    uint64_t stat_blocks_attempted;
    uint64_t stat_blocks_ok;
    uint64_t stat_blocks_failed;

    uint64_t packet_fail_after_successful_block_decode_runtime;
} sr_ctx_t;

/* packet size table used by sr_generate_packets() */
static const size_t sr_pkt_sizes[] = {
    64, 128, 512, 1500, 2048, 4096, 9000, 256, 1024, 3000
};

#define SR_NUM_PKT_SIZES \
    ((int)(sizeof(sr_pkt_sizes) / sizeof(sr_pkt_sizes[0])))

static void sr_reset_run_report(void)
{
    memset(&g_last_run_report, 0, sizeof(g_last_run_report));
}

static void *sr_alloc_aligned(size_t n)
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

static int sr_max_packets(const sr_ctx_t *ctx)
{
    return ctx->total_src_slots + 8;
}

static void sr_pkt_accum_reset(sr_pkt_accum_t *pa)
{
    if (pa == NULL) {
        return;
    }

    memset(pa, 0, sizeof(*pa));
}

static int sr_pkt_accum_find(const sr_pkt_accum_t *accum,
                             int                   accum_count,
                             uint32_t              packet_id)
{
    int i;

    for (i = 0; i < accum_count; ++i) {
        if (accum[i].active && accum[i].packet_id == packet_id) {
            return i;
        }
    }

    return -1;
}

static int sr_pkt_accum_has_symbol_index(const sr_pkt_accum_t *pa,
                                         uint16_t              symbol_index)
{
    int i;

    if (pa == NULL || !pa->active) {
        return 0;
    }

    for (i = 0; i < pa->count; ++i) {
        if (pa->frags[i].symbol_index == symbol_index) {
            return 1;
        }
    }

    return 0;
}

static void sr_pkt_accum_remove(sr_pkt_accum_t *accum,
                                int            *accum_count,
                                int             remove_idx)
{
    int count;

    if (accum == NULL || accum_count == NULL) {
        return;
    }

    count = *accum_count;

    if (remove_idx < 0 || remove_idx >= count) {
        return;
    }

    if (remove_idx < count - 1) {
        memmove(&accum[remove_idx],
                &accum[remove_idx + 1],
                sizeof(sr_pkt_accum_t) * (size_t)(count - remove_idx - 1));
    }

    sr_pkt_accum_reset(&accum[count - 1]);
    *accum_count = count - 1;
}

static sr_pkt_record_t *sr_find_packet_record(sr_ctx_t *ctx, uint32_t packet_id)
{
    int p;

    if (ctx == NULL) {
        return NULL;
    }

    for (p = 0; p < ctx->num_generated; ++p) {
        if (ctx->pkts[p].packet_id == packet_id) {
            return &ctx->pkts[p];
        }
    }

    return NULL;
}

static int sr_classify_block_failure(const sr_ctx_t *ctx,
                                     const fec_decode_telemetry_t *telemetry)
{
    if (telemetry == NULL) {
        return SR_FAIL_INSUFFICIENT_SYMBOLS;
    }

    if (telemetry->number_of_missing_symbols > ctx->m) {
        return SR_FAIL_TOO_MANY_HOLES;
    }

    return SR_FAIL_INSUFFICIENT_SYMBOLS;
}

static void sr_update_run_report_for_block(const sr_ctx_t *ctx,
                                           const fec_decode_telemetry_t *telemetry,
                                           int decode_success)
{
    uint64_t missing = 0U;
    int failure_reason;

    g_last_run_report.blocks_attempted++;

    if (telemetry != NULL && telemetry->number_of_missing_symbols > 0) {
        missing = (uint64_t)telemetry->number_of_missing_symbols;
    }

    if (missing > g_last_run_report.max_missing_symbols_in_block) {
        g_last_run_report.max_missing_symbols_in_block = missing;
    }

    if (decode_success) {
        g_last_run_report.blocks_passed++;
        return;
    }

    g_last_run_report.blocks_failed++;
    g_last_run_report.failed_missing_sum += missing;
    g_last_run_report.failed_missing_count++;

    failure_reason = sr_classify_block_failure(ctx, telemetry);

    if (failure_reason == SR_FAIL_TOO_MANY_HOLES) {
        g_last_run_report.fail_too_many_holes++;
    } else {
        g_last_run_report.fail_insufficient_symbols++;
    }
}

static void sr_finalize_run_report(const sr_ctx_t *ctx)
{
    if (g_last_run_report.failed_missing_count > 0U) {
        g_last_run_report.avg_missing_symbols_in_failed_blocks =
            (double)g_last_run_report.failed_missing_sum /
            (double)g_last_run_report.failed_missing_count;
    } else {
        g_last_run_report.avg_missing_symbols_in_failed_blocks = 0.0;
    }

    g_last_run_report.packet_fail_after_successful_block_decode =
        ctx->packet_fail_after_successful_block_decode_runtime;
}

static void sr_print_channel_events(const sim_run_request_t *req, int total_symbols)
{
    int e;

    printf("[CHANNEL EVENTS]\n");
    printf("- event_count: %d\n", req->num_events);

    for (e = 0; e < req->num_events; ++e) {
        const channel_event_t *ev = &req->events[e];
        int end_index = ev->start_symbol + ev->length_symbols - 1;

        if (end_index >= total_symbols) {
            end_index = total_symbols - 1;
        }

        printf("- event[%d]: type=%s start_index=%d end_index=%d length=%d\n",
               e,
               (ev->type == CHANNEL_EVENT_ERASURE) ? "ERASURE" : "CORRUPTION",
               ev->start_symbol,
               end_index,
               ev->length_symbols);
    }
}

static void sr_print_decoder_telemetry(uint64_t block_id,
                                       const fec_decode_telemetry_t *telemetry,
                                       int m_limit)
{
    printf("[REAL DECODER TELEMETRY]\n");
    printf("Block %" PRIu64 ":\n", block_id);
    printf("- received: %d\n", telemetry->number_of_symbols_received);
    printf("- missing: %d\n", telemetry->number_of_missing_symbols);
    printf("- repair: %d\n", telemetry->number_of_repair_symbols_used);
    printf("- success: %s\n", telemetry->decode_success ? "YES" : "NO");

    if (!telemetry->decode_success) {
        printf("[REAL ROOT CAUSE]\n");
        printf("Block %" PRIu64 " failed:\n", block_id);
        printf("missing=%d, M=%d -> %s capacity\n",
               telemetry->number_of_missing_symbols,
               m_limit,
               (telemetry->number_of_missing_symbols > m_limit) ? "exceeded" : "within");
    }
}

static void sr_print_block_analysis(uint64_t block_id,
                                    int expected_symbols,
                                    const fec_decode_telemetry_t *telemetry,
                                    int failure_reason)
{
    printf("[BLOCK ANALYSIS]\n");
    printf("Block %" PRIu64 ":\n", block_id);
    printf("- expected_symbols: %d\n", expected_symbols);
    printf("- received_symbols: %d\n", telemetry->number_of_symbols_received);
    printf("- missing_symbols: %d\n", telemetry->number_of_missing_symbols);
    printf("- repair_symbols_used: %d\n", telemetry->number_of_repair_symbols_used);
    printf("- decode_success: %s\n", telemetry->decode_success ? "YES" : "NO");
    printf("- result: %s\n", telemetry->decode_success ? "PASS" : "FAIL");

    if (!telemetry->decode_success) {
        printf("- reason: %s\n", sim_runner_failure_reason_name(failure_reason));
    }
}

static void sr_print_run_summary(void)
{
    printf("[RUN SUMMARY]\n");
    printf("- total_blocks_attempted: %" PRIu64 "\n", g_last_run_report.blocks_attempted);
    printf("- total_blocks_passed: %" PRIu64 "\n", g_last_run_report.blocks_passed);
    printf("- total_blocks_failed: %" PRIu64 "\n", g_last_run_report.blocks_failed);
    printf("- fail_too_many_holes: %" PRIu64 "\n", g_last_run_report.fail_too_many_holes);
    printf("- fail_insufficient_symbols: %" PRIu64 "\n", g_last_run_report.fail_insufficient_symbols);
    printf("Packet-level failures:\n");
    printf("- packet_fail_missing: %" PRIu64 "\n", g_last_run_report.packet_fail_missing);
    printf("- packet_fail_corrupted: %" PRIu64 "\n", g_last_run_report.packet_fail_corrupted);
    printf("- packet_fail_after_successful_block_decode: %" PRIu64 "\n",
           g_last_run_report.packet_fail_after_successful_block_decode);
    printf("- packet_level_failure_after_successful_block_decode: %s\n",
           (g_last_run_report.packet_fail_after_successful_block_decode > 0U) ? "YES" : "NO");
    printf("- max_missing_symbols_in_block: %" PRIu64 "\n",
           g_last_run_report.max_missing_symbols_in_block);
    printf("- avg_missing_symbols_in_failed_blocks: %.2f\n",
           g_last_run_report.avg_missing_symbols_in_failed_blocks);
}

/* =========================================================================
 * Packet generation
 * =========================================================================*/

static int sr_generate_packets(sr_ctx_t *ctx, uint32_t seed)
{
    int    slots_remaining = ctx->total_src_slots;
    int    size_idx        = 0;
    int    unique_id       = 1;
    int    max_pkts        = sr_max_packets(ctx);

    ctx->num_generated = 0;

    while (slots_remaining > 0 && ctx->num_generated < max_pkts) {
        size_t           sz = sr_pkt_sizes[size_idx % SR_NUM_PKT_SIZES];
        sr_pkt_record_t *pr = &ctx->pkts[ctx->num_generated];
        int              nf = (int)((sz + (size_t)ctx->symbol_size - 1U) /
                                    (size_t)ctx->symbol_size);
        size_t           j;

        size_idx++;

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
            return -1;
        }

        for (j = 0; j < sz; ++j) {
            pr->data[j] = (unsigned char)(
                (pr->packet_id * 251U +
                 (uint32_t)j * 37U +
                 (uint32_t)(j >> 8) +
                 seed) & 0xFFU
            );
        }

        stats_inc_ingress(sz);

        slots_remaining -= nf;
        ctx->num_generated++;
    }

    return 0;
}

/* =========================================================================
 * Encode one full/padded block and drain ready interleaver output
 * =========================================================================*/

static int sr_encode_one_block(sr_ctx_t        *ctx,
                               block_builder_t *bb,
                               fec_handle_t     fec,
                               interleaver_t   *il,
                               unsigned char   *src_data,
                               symbol_t        *repair_buf)
{
    int blk_idx = ctx->num_blocks_encoded;
    int s;
    int m;

    if (blk_idx >= ctx->num_blocks) {
        return -1;
    }

    ctx->block_meta[blk_idx].block_id = bb->block_id;

    for (s = 0; s < ctx->k; ++s) {
        memcpy(src_data + (size_t)s * (size_t)ctx->symbol_size,
               bb->symbols[s].data,
               (size_t)ctx->symbol_size);
    }

    memset(repair_buf, 0, (size_t)ctx->m * sizeof(symbol_t));
    if (fec_encode_block(fec, src_data, repair_buf, ctx->m) != FEC_DECODE_OK) {
        return -1;
    }

    for (s = 0; s < ctx->k; ++s) {
        symbol_t sym = bb->symbols[s];

        sym.packet_id     = (uint32_t)blk_idx;
        sym.fec_id        = (uint32_t)s;
        sym.total_symbols = (uint16_t)ctx->n;
        sym.payload_len   = (uint16_t)ctx->symbol_size;

        if (interleaver_push_symbol(il, &sym) < 0) {
            return -1;
        }
    }

    for (m = 0; m < ctx->m; ++m) {
        repair_buf[m].packet_id     = (uint32_t)blk_idx;
        repair_buf[m].fec_id        = (uint32_t)(ctx->k + m);
        repair_buf[m].total_symbols = (uint16_t)ctx->n;
        repair_buf[m].payload_len   = (uint16_t)ctx->symbol_size;

        if (interleaver_push_symbol(il, &repair_buf[m]) < 0) {
            return -1;
        }
    }

    ctx->num_blocks_encoded++;
    block_builder_reset(bb);

    while (interleaver_is_ready(il)) {
        int pr;

        if (ctx->tx_count >= ctx->total_syms) {
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

/* =========================================================================
 * TX pipeline
 * =========================================================================*/

static int sr_run_tx_pipeline(sr_ctx_t *ctx)
{
    symbol_t       *frag_buf   = NULL;
    symbol_t       *repair_buf = NULL;
    unsigned char  *src_data   = NULL;
    interleaver_t  *il         = NULL;
    fec_handle_t    fec        = NULL;
    block_builder_t bb;
    int             result     = -1;
    int             block_slot = 0;
    int             pi;

    ctx->num_blocks_encoded = 0;
    ctx->tx_count           = 0;

    frag_buf = (symbol_t *)calloc((size_t)SR_MAX_FRAGS_PER_PKT, sizeof(symbol_t));
    repair_buf = (symbol_t *)calloc((size_t)ctx->m, sizeof(symbol_t));
    src_data = (unsigned char *)sr_alloc_aligned(
        (size_t)ctx->k * (size_t)ctx->symbol_size
    );
    ctx->tx_buf = (symbol_t *)calloc((size_t)ctx->total_syms, sizeof(symbol_t));

    if (!frag_buf || !repair_buf || !src_data || !ctx->tx_buf) {
        goto cleanup;
    }

    memset(&bb, 0, sizeof(bb));
    if (block_builder_init(&bb, ctx->k) != 0) {
        goto cleanup;
    }

    il = interleaver_create(ctx->depth, ctx->n, ctx->symbol_size);
    if (!il) {
        goto cleanup_bb;
    }

    fec = fec_create(ctx->k, ctx->symbol_size);
    if (!fec) {
        goto cleanup_il;
    }

    for (pi = 0; pi < ctx->num_generated; ++pi) {
        sr_pkt_record_t *pr = &ctx->pkts[pi];
        int              nf;
        int              fi;

        if (ctx->num_blocks_encoded >= ctx->num_blocks) {
            break;
        }

        memset(frag_buf, 0, (size_t)SR_MAX_FRAGS_PER_PKT * sizeof(symbol_t));

        nf = fragment_packet(pr->data,
                             pr->packet_len,
                             pr->packet_id,
                             (uint16_t)ctx->symbol_size,
                             frag_buf,
                             (uint16_t)SR_MAX_FRAGS_PER_PKT);
        if (nf < 0) {
            goto cleanup_fec;
        }

        for (fi = 0; fi < nf; ++fi) {
            symbol_t        *sym = &frag_buf[fi];
            sr_block_meta_t *bm;
            sr_slot_meta_t  *sm;
            int              rc;

            if (ctx->num_blocks_encoded >= ctx->num_blocks) {
                goto tx_done;
            }

            sym->fec_id = (uint32_t)block_slot;

            if (sym->payload_len < (uint16_t)ctx->symbol_size) {
                memset(sym->data + sym->payload_len,
                       0,
                       (size_t)(ctx->symbol_size - sym->payload_len));
            }

            bm = &ctx->block_meta[ctx->num_blocks_encoded];
            sm = &bm->slots[block_slot];

            sm->orig_packet_id = sym->packet_id;
            sm->symbol_index   = sym->symbol_index;
            sm->total_symbols  = sym->total_symbols;
            sm->payload_len    = sym->payload_len;

            rc = block_builder_add_symbol(&bb, sym);
            if (rc < 0) {
                goto cleanup_fec;
            }

            block_slot++;

            if (rc == 1) {
                if (sr_encode_one_block(ctx, &bb, fec, il, src_data, repair_buf) != 0) {
                    goto cleanup_fec;
                }
                block_slot = 0;
            }
        }
    }

tx_done:
    if (block_slot > 0 && ctx->num_blocks_encoded < ctx->num_blocks) {
        block_builder_finalize_with_padding(&bb);
        if (sr_encode_one_block(ctx, &bb, fec, il, src_data, repair_buf) != 0) {
            goto cleanup_fec;
        }
    }

    while (interleaver_is_ready(il)) {
        int pr2;

        if (ctx->tx_count >= ctx->total_syms) {
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

static void sr_recount_transmitted(sr_ctx_t *ctx)
{
    int b;
    int s;
    int p;

    for (p = 0; p < ctx->num_generated; ++p) {
        ctx->pkts[p].transmitted = 0;
    }

    for (b = 0; b < ctx->num_blocks_encoded; ++b) {
        sr_block_meta_t *bm = &ctx->block_meta[b];

        for (s = 0; s < ctx->k; ++s) {
            sr_slot_meta_t *sm = &bm->slots[s];

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
            stats_inc_transmitted(ctx->pkts[p].packet_len);
        }
    }
}

static void sr_apply_channel_events(sr_ctx_t                *ctx,
                                    const sim_run_request_t *req,
                                    channel_apply_result_t  *out_ch)
{
    int e;

    ctx->stat_symbols_erased    = 0U;
    ctx->stat_symbols_corrupted = 0U;

    sr_print_channel_events(req, ctx->total_syms);

    for (e = 0; e < req->num_events; ++e) {
        const channel_event_t *ev = &req->events[e];
        int start = ev->start_symbol;
        int end   = start + ev->length_symbols;
        int i;

        if (start >= ctx->tx_count) {
            continue;
        }

        if (end > ctx->tx_count) {
            end = ctx->tx_count;
        }

        if (ev->type == CHANNEL_EVENT_ERASURE) {
            for (i = start; i < end; ++i) {
                if (ctx->tx_buf[i].payload_len != 0) {
                    memset(&ctx->tx_buf[i], 0, sizeof(symbol_t));
                    ctx->stat_symbols_erased++;
                }
            }
        } else if (ev->type == CHANNEL_EVENT_CORRUPTION) {
            for (i = start; i < end; ++i) {
                symbol_t *sym = &ctx->tx_buf[i];

                if (sym->payload_len > 0) {
                    size_t j;

                    for (j = 0;
                         j < (size_t)sym->payload_len && j < MAX_SYMBOL_DATA_SIZE;
                         ++j) {
                        sym->data[j] ^= 0xA5U;
                    }

                    ctx->stat_symbols_corrupted++;
                }
            }
        }
    }

    {
        int i;

        for (i = 0; i < ctx->tx_count; ++i) {
            bool lost = (ctx->tx_buf[i].payload_len == 0);
            stats_record_symbol(lost);
        }

        stats_finalize_burst();
    }

    if (out_ch) {
        out_ch->total_tx_symbols  = (uint64_t)ctx->tx_count;
        out_ch->lost_symbols      = ctx->stat_symbols_erased;
        out_ch->corrupted_symbols = ctx->stat_symbols_corrupted;
    }
}

/* =========================================================================
 * Deliver one decoded block into packet accumulators
 * =========================================================================*/

static void sr_deliver_block(sr_ctx_t       *ctx,
                             int             blk_idx,
                             unsigned char  *recon,
                             sr_pkt_accum_t *accum,
                             int            *accum_count,
                             int             max_accum)
{
    sr_block_meta_t *bm = &ctx->block_meta[blk_idx];
    int              s;

    for (s = 0; s < ctx->k; ++s) {
        sr_slot_meta_t  *sm = &bm->slots[s];
        sr_pkt_record_t *pr;
        int              idx;
        sr_pkt_accum_t  *pa;
        symbol_t         sym;
        int              recon_len;
        unsigned char    recon_pkt[9000];

        if (sm->payload_len == 0) {
            continue;
        }

        pr = sr_find_packet_record(ctx, sm->orig_packet_id);
        if (pr == NULL) {
            continue;
        }

        if (!pr->transmitted) {
            continue;
        }

        if (pr->recovered) {
            continue;
        }

        idx = sr_pkt_accum_find(accum, *accum_count, sm->orig_packet_id);
        if (idx < 0) {
            if (*accum_count >= max_accum) {
                continue;
            }

            idx = *accum_count;
            pa  = &accum[idx];
            sr_pkt_accum_reset(pa);
            pa->active    = 1;
            pa->packet_id = sm->orig_packet_id;
            pa->expected  = (int)sm->total_symbols;
            pa->count     = 0;
            (*accum_count)++;
        } else {
            pa = &accum[idx];
        }

        if (!pa->active) {
            sr_pkt_accum_reset(pa);
            pa->active    = 1;
            pa->packet_id = sm->orig_packet_id;
            pa->expected  = (int)sm->total_symbols;
            pa->count     = 0;
        }

        if (pa->expected != (int)sm->total_symbols) {
            sr_pkt_accum_remove(accum, accum_count, idx);
            if (*accum_count >= max_accum) {
                continue;
            }

            idx = *accum_count;
            pa  = &accum[idx];
            sr_pkt_accum_reset(pa);
            pa->active    = 1;
            pa->packet_id = sm->orig_packet_id;
            pa->expected  = (int)sm->total_symbols;
            pa->count     = 0;
            (*accum_count)++;
        }

        if (pa->expected <= 0 || pa->expected > SR_MAX_FRAGS_PER_PKT) {
            sr_pkt_accum_remove(accum, accum_count, idx);
            continue;
        }

        if (sr_pkt_accum_has_symbol_index(pa, sm->symbol_index)) {
            continue;
        }

        if (pa->count >= SR_MAX_FRAGS_PER_PKT || pa->count >= pa->expected) {
            sr_pkt_accum_remove(accum, accum_count, idx);
            continue;
        }

        memset(&sym, 0, sizeof(sym));
        sym.packet_id     = sm->orig_packet_id;
        sym.fec_id        = (uint32_t)s;
        sym.symbol_index  = sm->symbol_index;
        sym.total_symbols = sm->total_symbols;
        sym.payload_len   = sm->payload_len;

        memcpy(sym.data,
               recon + (size_t)s * (size_t)ctx->symbol_size,
               sm->payload_len);

        pa->frags[pa->count] = sym;
        pa->count++;

        if (pa->count != pa->expected) {
            continue;
        }

        memset(recon_pkt, 0, sizeof(recon_pkt));
        recon_len = reassemble_packet(pa->frags,
                                      (uint16_t)pa->count,
                                      recon_pkt,
                                      sizeof(recon_pkt));

        if (!pr->recovered) {
            if (recon_len > 0) {
                pr->recovered = 1;

                if ((size_t)recon_len == pr->packet_len &&
                    memcmp(recon_pkt, pr->data, pr->packet_len) == 0) {
                    pr->exact_match = 1;
                } else {
                    ctx->packet_fail_after_successful_block_decode_runtime++;
                }

                stats_inc_recovered(pr->packet_len);
            } else {
                ctx->packet_fail_after_successful_block_decode_runtime++;
                stats_inc_failed_packet();
            }
        }

        sr_pkt_accum_remove(accum, accum_count, idx);
    }
}

/* =========================================================================
 * RX pipeline
 * =========================================================================*/

static int sr_run_rx_pipeline(sr_ctx_t *ctx)
{
    deinterleaver_t        *dil         = NULL;
    fec_handle_t            fec         = NULL;
    unsigned char          *recon       = NULL;
    sr_pkt_accum_t         *accum       = NULL;
    int                     accum_count = 0;
    int                     max_accum   = ctx->k * SR_MAX_ACCUM_FACTOR;
    int                     dil_slots;
    int                     result      = -1;
    int                     i;

    dil_slots = ctx->num_blocks_encoded * SR_DIL_HEADROOM_FACTOR;
    if (dil_slots < ctx->depth + 4) {
        dil_slots = ctx->depth + 4;
    }

    dil = deinterleaver_create(dil_slots,
                               ctx->n,
                               ctx->k,
                               (size_t)ctx->symbol_size,
                               0.0,
                               0.0);
    if (!dil) {
        return -1;
    }

    fec = fec_create(ctx->k, ctx->symbol_size);
    if (!fec) {
        goto cleanup;
    }

    recon = (unsigned char *)sr_alloc_aligned(
        (size_t)ctx->k * (size_t)ctx->symbol_size
    );
    if (!recon) {
        goto cleanup;
    }

    accum = (sr_pkt_accum_t *)calloc((size_t)max_accum, sizeof(sr_pkt_accum_t));
    if (!accum) {
        goto cleanup;
    }

    for (i = 0; i < ctx->tx_count; ++i) {
        symbol_t *sym = &ctx->tx_buf[i];
        int       rc;
        block_t   blk;

        if (sym->payload_len == 0) {
            continue;
        }

        rc = deinterleaver_push_symbol(dil, sym);

        if (rc < 0) {
            while (deinterleaver_get_ready_block(dil, &blk) == 0) {
                int                     blk_idx = (int)blk.block_id;
                uint64_t                holes   = 0U;
                fec_decode_telemetry_t  telemetry;
                int                     decode_rc;
                int                     failure_reason = SR_FAIL_NONE;

                if (blk.symbols_per_block > blk.symbol_count) {
                    holes = (uint64_t)(blk.symbols_per_block - blk.symbol_count);
                }

                ctx->stat_blocks_attempted++;
                stats_inc_block_attempt();
                stats_record_block(holes);

                memset(recon, 0, (size_t)ctx->k * (size_t)ctx->symbol_size);

                fec_set_current_decode_block_id((uint64_t)blk.block_id);
                decode_rc = fec_decode_block(fec,
                                             blk.symbols,
                                             blk.symbol_count,
                                             blk.symbols_per_block,
                                             recon);
                fec_get_last_decode_telemetry(&telemetry);

                if (decode_rc != FEC_DECODE_OK) {
                    failure_reason = sr_classify_block_failure(ctx, &telemetry);
                }

                sr_update_run_report_for_block(ctx, &telemetry, decode_rc == FEC_DECODE_OK);
                sr_print_decoder_telemetry((uint64_t)blk.block_id, &telemetry, ctx->m);
                sr_print_block_analysis((uint64_t)blk.block_id,
                                        blk.symbols_per_block,
                                        &telemetry,
                                        failure_reason);

                if (decode_rc == FEC_DECODE_OK) {
                    ctx->stat_blocks_ok++;
                    stats_inc_block_success();

                    if (blk_idx >= 0 && blk_idx < ctx->num_blocks_encoded) {
                        sr_deliver_block(ctx, blk_idx, recon, accum, &accum_count, max_accum);
                    }

                    deinterleaver_mark_result(dil, (uint32_t)blk.block_id, 1);
                } else {
                    ctx->stat_blocks_failed++;
                    stats_inc_block_failure();
                    deinterleaver_mark_result(dil, (uint32_t)blk.block_id, 0);
                }
            }

            rc = deinterleaver_push_symbol(dil, sym);
            (void)rc;
        }
    }

    deinterleaver_tick(dil, 0.0);

    {
        block_t blk;

        while (deinterleaver_get_ready_block(dil, &blk) == 0) {
            int                     blk_idx = (int)blk.block_id;
            uint64_t                holes   = 0U;
            fec_decode_telemetry_t  telemetry;
            int                     decode_rc;
            int                     failure_reason = SR_FAIL_NONE;

            if (blk.symbols_per_block > blk.symbol_count) {
                holes = (uint64_t)(blk.symbols_per_block - blk.symbol_count);
            }

            ctx->stat_blocks_attempted++;
            stats_inc_block_attempt();
            stats_record_block(holes);

            memset(recon, 0, (size_t)ctx->k * (size_t)ctx->symbol_size);

            fec_set_current_decode_block_id((uint64_t)blk.block_id);
            decode_rc = fec_decode_block(fec,
                                         blk.symbols,
                                         blk.symbol_count,
                                         blk.symbols_per_block,
                                         recon);
            fec_get_last_decode_telemetry(&telemetry);

            if (decode_rc != FEC_DECODE_OK) {
                failure_reason = sr_classify_block_failure(ctx, &telemetry);
            }

            sr_update_run_report_for_block(ctx, &telemetry, decode_rc == FEC_DECODE_OK);
            sr_print_decoder_telemetry((uint64_t)blk.block_id, &telemetry, ctx->m);
            sr_print_block_analysis((uint64_t)blk.block_id,
                                    blk.symbols_per_block,
                                    &telemetry,
                                    failure_reason);

            if (decode_rc == FEC_DECODE_OK) {
                ctx->stat_blocks_ok++;
                stats_inc_block_success();

                if (blk_idx >= 0 && blk_idx < ctx->num_blocks_encoded) {
                    sr_deliver_block(ctx, blk_idx, recon, accum, &accum_count, max_accum);
                }

                deinterleaver_mark_result(dil, (uint32_t)blk.block_id, 1);
            } else {
                ctx->stat_blocks_failed++;
                stats_inc_block_failure();
                deinterleaver_mark_result(dil, (uint32_t)blk.block_id, 0);
            }
        }
    }

    {
        int p;

        for (p = 0; p < ctx->num_generated; ++p) {
            sr_pkt_record_t *pr = &ctx->pkts[p];

            if (!pr->transmitted) {
                continue;
            }

            if (!pr->recovered) {
                g_last_run_report.packet_fail_missing++;
                if (ctx->stat_blocks_failed == 0U) {
                    ctx->packet_fail_after_successful_block_decode_runtime++;
                }
                stats_inc_failed_packet();
            } else if (!pr->exact_match) {
                g_last_run_report.packet_fail_corrupted++;
            }
        }
    }

    sr_finalize_run_report(ctx);
    sr_print_run_summary();

    result = 0;

cleanup:
    free(accum);
    free(recon);
    fec_destroy(fec);
    deinterleaver_destroy(dil);
    return result;
}

/* =========================================================================
 * Public API
 * =========================================================================*/

int sim_runner_global_init(void)
{
    stats_init();

    if (wirehair_init() != Wirehair_Success) {
        fprintf(stderr, "[sim_runner] FATAL: wirehair_init() failed\n");
        return -1;
    }

    return 0;
}

int sim_run_campaign_case(const sim_config_t      *cfg,
                          const sim_run_request_t *req,
                          sim_result_t            *out_result,
                          channel_apply_result_t  *out_channel)
{
    sr_ctx_t ctx;
    int      rc      = -1;
    int      p;
    int      b;
    uint32_t seed;
    int      max_pkts;

    if (!cfg || !req || !out_result) {
        return -1;
    }

    memset(&ctx, 0, sizeof(ctx));
    memset(out_result, 0, sizeof(*out_result));
    sr_reset_run_report();

    if (out_channel) {
        memset(out_channel, 0, sizeof(*out_channel));
    }

    stats_reset();
    stats_set_burst_fec_span((uint64_t)(cfg->m * cfg->depth));

    ctx.k               = cfg->k;
    ctx.m               = cfg->m;
    ctx.n               = cfg->k + cfg->m;
    ctx.depth           = cfg->depth;
    ctx.symbol_size     = cfg->symbol_size;
    ctx.num_blocks      = cfg->num_windows * cfg->depth;
    ctx.total_syms      = ctx.num_blocks * ctx.n;
    ctx.total_src_slots = ctx.num_blocks * ctx.k;

    seed     = (req->seed_override != 0U) ? req->seed_override : cfg->seed;
    max_pkts = ctx.total_src_slots + 8;

    ctx.pkts = (sr_pkt_record_t *)calloc((size_t)max_pkts, sizeof(sr_pkt_record_t));
    if (!ctx.pkts) {
        goto done;
    }

    ctx.block_meta = (sr_block_meta_t *)calloc((size_t)ctx.num_blocks, sizeof(sr_block_meta_t));
    if (!ctx.block_meta) {
        goto done;
    }

    for (b = 0; b < ctx.num_blocks; ++b) {
        ctx.block_meta[b].slots = (sr_slot_meta_t *)calloc((size_t)ctx.k, sizeof(sr_slot_meta_t));
        if (!ctx.block_meta[b].slots) {
            goto done;
        }
    }

    if (sr_generate_packets(&ctx, seed) != 0) {
        goto done;
    }

    if (sr_run_tx_pipeline(&ctx) != 0) {
        goto done;
    }

    sr_recount_transmitted(&ctx);

    sr_apply_channel_events(&ctx, req, out_channel);

    if (sr_run_rx_pipeline(&ctx) != 0) {
        goto done;
    }

    {
        uint64_t recovered   = 0U;
        uint64_t exact_match = 0U;
        uint64_t corrupted   = 0U;
        uint64_t missing     = 0U;

        for (p = 0; p < ctx.num_generated; ++p) {
            const sr_pkt_record_t *pr = &ctx.pkts[p];

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

        out_result->generated_packets   = (uint64_t)ctx.num_generated;
        out_result->transmitted_packets = (uint64_t)ctx.num_transmitted;
        out_result->recovered_packets   = recovered;
        out_result->exact_match_packets = exact_match;
        out_result->corrupted_packets   = corrupted;
        out_result->missing_packets     = missing;

        out_result->total_tx_symbols  = (uint64_t)ctx.tx_count;
        out_result->lost_symbols      = ctx.stat_symbols_erased;
        out_result->corrupted_symbols = ctx.stat_symbols_corrupted;

        out_result->blocks_attempted = ctx.stat_blocks_attempted;
        out_result->blocks_recovered = ctx.stat_blocks_ok;
        out_result->blocks_failed    = ctx.stat_blocks_failed;

        out_result->lost_bytes = ctx.stat_symbols_erased * (uint64_t)cfg->symbol_size;
        out_result->lost_equiv_1500_pkts =
            (out_result->lost_bytes + 1499U) / 1500U;

        {
            double sym_time_us = (double)cfg->symbol_size * 8.0 / 10000.0;
            out_result->fade_duration_us = (double)ctx.stat_symbols_erased * sym_time_us;
        }

        out_result->recovery_rate =
            (ctx.num_transmitted > 0)
                ? (double)recovered / (double)ctx.num_transmitted
                : 1.0;

        out_result->exact_match_rate =
            (ctx.num_transmitted > 0)
                ? (double)exact_match / (double)ctx.num_transmitted
                : 1.0;
    }

    rc = 0;

done:
    if (ctx.block_meta) {
        for (b = 0; b < ctx.num_blocks; ++b) {
            free(ctx.block_meta[b].slots);
        }
        free(ctx.block_meta);
    }

    if (ctx.pkts) {
        for (p = 0; p < ctx.num_generated; ++p) {
            free(ctx.pkts[p].data);
        }
        free(ctx.pkts);
    }

    free(ctx.tx_buf);

    return rc;
}
