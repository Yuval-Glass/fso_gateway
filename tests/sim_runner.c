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
#define SR_BLOCK_FINAL_UNKNOWN (-1)

static sr_run_report_t         g_last_run_report;
static sr_oracle_block_diag_t *g_last_oracle_block_diags = NULL;
static size_t                  g_last_oracle_block_count  = 0U;

void sim_runner_get_last_run_report(sr_run_report_t *out)
{
    if (out == NULL) {
        return;
    }

    *out = g_last_run_report;
}

void sim_runner_get_last_oracle_block_diags(const sr_oracle_block_diag_t **out_blocks,
                                            size_t                       *out_count)
{
    if (out_blocks != NULL) {
        *out_blocks = g_last_oracle_block_diags;
    }

    if (out_count != NULL) {
        *out_count = g_last_oracle_block_count;
    }
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

const char *sim_runner_block_final_reason_name(int reason)
{
    switch (reason) {
        case DIL_BLOCK_FINAL_DECODE_SUCCESS:
            return "DECODE_SUCCESS";
        case DIL_BLOCK_FINAL_DECODE_FAILED:
            return "DECODE_FAILED";
        case DIL_BLOCK_FINAL_DISCARDED_TIMEOUT_BEFORE_DECODE:
            return "DISCARDED_TIMEOUT_BEFORE_DECODE";
        case DIL_BLOCK_FINAL_DISCARDED_TOO_MANY_HOLES_BEFORE_DECODE:
            return "DISCARDED_TOO_MANY_HOLES_BEFORE_DECODE";
        case DIL_BLOCK_FINAL_DISCARDED_EVICTED_BEFORE_DECODE:
            return "DISCARDED_EVICTED_BEFORE_DECODE";
        case DIL_BLOCK_FINAL_DISCARDED_READY_EVICTED_BEFORE_MARK:
            return "DISCARDED_READY_EVICTED_BEFORE_MARK";
        default:
            return "UNKNOWN";
    }
}

typedef struct {
    uint32_t       packet_id;
    size_t         packet_len;
    unsigned char *data;
    int            transmitted;
    int            recovered;
    int            exact_match;
    int            num_contributing_blocks;
    int            contributing_blocks[SR_MAX_FRAGS_PER_PKT];
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

    int *block_final_state;

    dil_eviction_info_t *block_eviction_info;   /* NULL = no eviction trace  */
    bool                *block_has_eviction;

    uint64_t failed_missing_sum;
    uint64_t failed_missing_count;
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

static void sr_reset_oracle_block_diags(void)
{
    free(g_last_oracle_block_diags);
    g_last_oracle_block_diags = NULL;
    g_last_oracle_block_count = 0U;
}

static int sr_run_has_corruption_events(const sim_run_request_t *req)
{
    int i;

    if (req == NULL) {
        return 0;
    }

    for (i = 0; i < req->num_events; ++i) {
        if (req->events[i].type == CHANNEL_EVENT_CORRUPTION) {
            return 1;
        }
    }

    return 0;
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

static void sr_record_packet_block_dependency(sr_ctx_t *ctx,
                                              uint32_t  packet_id,
                                              int       block_idx)
{
    sr_pkt_record_t *pr;
    int              i;

    if (ctx == NULL) {
        return;
    }

    pr = sr_find_packet_record(ctx, packet_id);
    if (pr == NULL) {
        return;
    }

    for (i = 0; i < pr->num_contributing_blocks; ++i) {
        if (pr->contributing_blocks[i] == block_idx) {
            return;
        }
    }

    if (pr->num_contributing_blocks >= SR_MAX_FRAGS_PER_PKT) {
        return;
    }

    pr->contributing_blocks[pr->num_contributing_blocks++] = block_idx;
}

static int sr_classify_block_failure(const sr_ctx_t *ctx,
                                     const fec_decode_telemetry_t *telemetry)
{
    if (ctx == NULL || telemetry == NULL) {
        return SR_FAIL_NONE;
    }

    if (telemetry->number_of_missing_symbols > ctx->m) {
        return SR_FAIL_TOO_MANY_HOLES;
    }

    return SR_FAIL_INSUFFICIENT_SYMBOLS;
}

static void sr_update_decode_telemetry(sr_ctx_t *ctx,
                                       const fec_decode_telemetry_t *telemetry,
                                       int decode_success)
{
    uint64_t missing = 0U;

    if (telemetry != NULL && telemetry->number_of_missing_symbols > 0) {
        missing = (uint64_t)telemetry->number_of_missing_symbols;
    }

    if (missing > g_last_run_report.max_missing_symbols_in_block) {
        g_last_run_report.max_missing_symbols_in_block = missing;
    }

    if (!decode_success) {
        ctx->failed_missing_sum += missing;
        ctx->failed_missing_count++;
    }
}

static void sr_note_block_final_reason(sr_ctx_t *ctx,
                                       uint32_t  block_id,
                                       deinterleaver_block_final_reason_t reason)
{
    int idx = (int)block_id;

    if (ctx == NULL) {
        return;
    }

    if (idx < 0 || idx >= ctx->num_blocks_encoded) {
        LOG_WARN("[sim_runner] block final callback out of range block_id=%u encoded=%d",
                 (unsigned)block_id,
                 ctx->num_blocks_encoded);
        return;
    }

    if (ctx->block_final_state == NULL) {
        return;
    }

    if (ctx->block_final_state[idx] != SR_BLOCK_FINAL_UNKNOWN) {
        LOG_WARN("[sim_runner] duplicate final reason for block_id=%u old=%d new=%d",
                 (unsigned)block_id,
                 ctx->block_final_state[idx],
                 (int)reason);
        return;
    }

    ctx->block_final_state[idx] = (int)reason;
}

static void sr_deinterleaver_block_final_cb(
    uint32_t                           block_id,
    deinterleaver_block_final_reason_t reason,
    void                              *user)
{
    sr_ctx_t *ctx = (sr_ctx_t *)user;
    sr_note_block_final_reason(ctx, block_id, reason);
}

static void sr_deinterleaver_eviction_cb(
    uint32_t                            evicted_block_id,
    deinterleaver_block_final_reason_t  reason,
    const dil_eviction_info_t          *info,
    void                               *user)
{
    sr_ctx_t *ctx = (sr_ctx_t *)user;
    int        idx = (int)evicted_block_id;

    (void)reason;

    if (ctx == NULL || info == NULL) {
        return;
    }

    if (idx < 0 || idx >= ctx->num_blocks_encoded) {
        return;
    }

    if (ctx->block_eviction_info == NULL || ctx->block_has_eviction == NULL) {
        return;
    }

    ctx->block_eviction_info[idx] = *info;
    ctx->block_has_eviction[idx]  = true;
}

static int sr_packet_missing_due_to_missing_blocks(const sr_ctx_t        *ctx,
                                                   const sr_pkt_record_t *pr)
{
    int i;

    if (ctx == NULL || pr == NULL || pr->num_contributing_blocks <= 0) {
        return 1;
    }

    for (i = 0; i < pr->num_contributing_blocks; ++i) {
        int blk_idx = pr->contributing_blocks[i];
        int state;

        if (blk_idx < 0 || blk_idx >= ctx->num_blocks_encoded) {
            return 1;
        }

        state = ctx->block_final_state[blk_idx];
        if (state != DIL_BLOCK_FINAL_DECODE_SUCCESS) {
            return 1;
        }
    }

    return 0;
}

static void sr_compute_recoverability_oracle(const sr_ctx_t *ctx)
{
    int i;

    if (ctx == NULL || ctx->num_blocks_encoded <= 0) {
        return;
    }

    sr_reset_oracle_block_diags();

    g_last_oracle_block_diags = (sr_oracle_block_diag_t *)calloc(
        (size_t)ctx->num_blocks_encoded,
        sizeof(*g_last_oracle_block_diags)
    );
    if (g_last_oracle_block_diags == NULL) {
        LOG_WARN("[sim_runner] recoverability oracle allocation failed");
        return;
    }

    g_last_oracle_block_count = (size_t)ctx->num_blocks_encoded;

    for (i = 0; i < ctx->num_blocks_encoded; ++i) {
        g_last_oracle_block_diags[i].block_id = (uint64_t)i;
        g_last_oracle_block_diags[i].expected_symbols = (uint64_t)ctx->n;
        g_last_oracle_block_diags[i].final_reason = ctx->block_final_state[i];
    }

    for (i = 0; i < ctx->tx_count; ++i) {
        const symbol_t *sym = &ctx->tx_buf[i];
        uint32_t        blk_idx;

        if (sym->payload_len == 0) {
            continue;
        }

        blk_idx = sym->packet_id;
        if ((int)blk_idx < 0 || (int)blk_idx >= ctx->num_blocks_encoded) {
            continue;
        }

        g_last_oracle_block_diags[blk_idx].survived_symbols++;
        if (sym->fec_id < (uint32_t)ctx->k) {
            g_last_oracle_block_diags[blk_idx].source_symbols_survived++;
        } else if (sym->fec_id < (uint32_t)ctx->n) {
            g_last_oracle_block_diags[blk_idx].repair_symbols_survived++;
        }
    }

    for (i = 0; i < ctx->num_blocks_encoded; ++i) {
        sr_oracle_block_diag_t *diag = &g_last_oracle_block_diags[i];
        uint64_t expected = diag->expected_symbols;
        uint64_t survived = diag->survived_symbols;
        int      state    = diag->final_reason;
        int      success  = (state == DIL_BLOCK_FINAL_DECODE_SUCCESS);

        diag->missing_symbols = (survived <= expected) ? (expected - survived) : 0U;
        diag->oracle_theoretically_recoverable = (survived >= (uint64_t)ctx->k);

        diag->discarded_timeout_before_decode =
            (state == DIL_BLOCK_FINAL_DISCARDED_TIMEOUT_BEFORE_DECODE);
        diag->discarded_too_many_holes_before_decode =
            (state == DIL_BLOCK_FINAL_DISCARDED_TOO_MANY_HOLES_BEFORE_DECODE);
        diag->discarded_evicted_before_decode =
            (state == DIL_BLOCK_FINAL_DISCARDED_EVICTED_BEFORE_DECODE);
        diag->discarded_ready_evicted_before_mark =
            (state == DIL_BLOCK_FINAL_DISCARDED_READY_EVICTED_BEFORE_MARK);
        diag->discarded_before_decode =
            (diag->discarded_timeout_before_decode ||
             diag->discarded_too_many_holes_before_decode ||
             diag->discarded_evicted_before_decode ||
             diag->discarded_ready_evicted_before_mark);

        /* Eviction trace */
        if (ctx->block_has_eviction != NULL &&
            ctx->block_eviction_info != NULL &&
            ctx->block_has_eviction[i]) {
            const dil_eviction_info_t *ei = &ctx->block_eviction_info[i];
            uint32_t                   sc;
            uint32_t                   j;

            diag->has_eviction_trace            = true;
            diag->eviction_incoming_block_id    = ei->incoming_block_id;
            diag->eviction_slot_index           = ei->slot_index;
            diag->eviction_valid_symbols        = ei->valid_symbols;
            diag->eviction_holes                = ei->holes;
            diag->eviction_expected_symbols     = ei->expected_symbols;
            diag->eviction_active_blocks        = ei->active_blocks;
            diag->eviction_max_active_blocks    = ei->max_active_blocks;

            sc = ei->snapshot_count;
            if (sc > 16U) { sc = 16U; }
            diag->eviction_snapshot_count = sc;
            for (j = 0; j < sc; ++j) {
                diag->eviction_snapshot_indices[j]       = ei->snapshot_indices[j];
                diag->eviction_snapshot_states[j]        = ei->snapshot_states[j];
                diag->eviction_snapshot_block_ids[j]     = ei->snapshot_block_ids[j];
                diag->eviction_snapshot_valid_symbols[j] = ei->snapshot_valid_symbols[j];
            }
        } else {
            diag->has_eviction_trace = false;
        }

        diag->suspicious_oracle_recoverable_but_discarded_before_decode =
            (g_last_run_report.oracle_valid_for_run &&
             diag->oracle_theoretically_recoverable &&
             diag->discarded_before_decode);

        if (diag->oracle_theoretically_recoverable) {
            g_last_run_report.oracle_blocks_theoretically_recoverable++;

            if (g_last_run_report.oracle_valid_for_run && !success) {
                g_last_run_report.oracle_recoverable_but_not_decoded_successfully++;

                if (state == DIL_BLOCK_FINAL_DECODE_FAILED) {
                    g_last_run_report.oracle_recoverable_but_decode_failed++;
                } else if (diag->discarded_before_decode) {
                    g_last_run_report.oracle_recoverable_but_discarded_before_decode++;
                }
            }
        } else {
            g_last_run_report.oracle_blocks_theoretically_unrecoverable++;

            if (!success) {
                g_last_run_report.oracle_unrecoverable_and_not_decoded_successfully++;
            }
        }
    }
}

static int sr_finalize_run_report(const sr_ctx_t *ctx)
{
    int i;
    uint64_t discarded_sum;
    uint64_t class_sum;

    if (ctx == NULL) {
        return -1;
    }

    g_last_run_report.blocks_total_expected = (uint64_t)ctx->num_blocks_encoded;

    if (ctx->failed_missing_count > 0U) {
        g_last_run_report.avg_missing_symbols_in_failed_blocks =
            (double)ctx->failed_missing_sum /
            (double)ctx->failed_missing_count;
    } else {
        g_last_run_report.avg_missing_symbols_in_failed_blocks = 0.0;
    }

    sr_compute_recoverability_oracle(ctx);

    for (i = 0; i < ctx->num_blocks_encoded; ++i) {
        int state = ctx->block_final_state[i];

        switch (state) {
            case DIL_BLOCK_FINAL_DECODE_SUCCESS:
                g_last_run_report.blocks_decode_success++;
                break;
            case DIL_BLOCK_FINAL_DECODE_FAILED:
                g_last_run_report.blocks_decode_failed++;
                break;
            case DIL_BLOCK_FINAL_DISCARDED_TIMEOUT_BEFORE_DECODE:
                g_last_run_report.blocks_discarded_before_decode++;
                g_last_run_report.blocks_discarded_timeout_before_decode++;
                break;
            case DIL_BLOCK_FINAL_DISCARDED_TOO_MANY_HOLES_BEFORE_DECODE:
                g_last_run_report.blocks_discarded_before_decode++;
                g_last_run_report.blocks_discarded_too_many_holes_before_decode++;
                break;
            case DIL_BLOCK_FINAL_DISCARDED_EVICTED_BEFORE_DECODE:
                g_last_run_report.blocks_discarded_before_decode++;
                g_last_run_report.blocks_discarded_evicted_before_decode++;
                break;
            case DIL_BLOCK_FINAL_DISCARDED_READY_EVICTED_BEFORE_MARK:
                g_last_run_report.blocks_discarded_before_decode++;
                g_last_run_report.blocks_discarded_ready_evicted_before_mark++;
                break;
            default:
                LOG_ERROR("[sim_runner] block %d missing final reason", i);
                return -1;
        }
    }

    discarded_sum =
        g_last_run_report.blocks_discarded_timeout_before_decode +
        g_last_run_report.blocks_discarded_too_many_holes_before_decode +
        g_last_run_report.blocks_discarded_evicted_before_decode +
        g_last_run_report.blocks_discarded_ready_evicted_before_mark;

    if (discarded_sum != g_last_run_report.blocks_discarded_before_decode) {
        LOG_ERROR("[sim_runner] discarded block accounting mismatch: total=%" PRIu64
                  " sub_sum=%" PRIu64,
                  g_last_run_report.blocks_discarded_before_decode,
                  discarded_sum);
        return -1;
    }

    class_sum =
        g_last_run_report.blocks_decode_success +
        g_last_run_report.blocks_decode_failed +
        g_last_run_report.blocks_discarded_before_decode;

    if (class_sum != g_last_run_report.blocks_total_expected) {
        LOG_ERROR("[sim_runner] block accounting invariant violated: expected=%" PRIu64
                  " classified=%" PRIu64,
                  g_last_run_report.blocks_total_expected,
                  class_sum);
        return -1;
    }

    if (g_last_run_report.blocks_attempted_for_decode !=
        (g_last_run_report.blocks_decode_success +
         g_last_run_report.blocks_decode_failed)) {
        LOG_ERROR("[sim_runner] decoder attempt accounting mismatch: attempted=%" PRIu64
                  " success+failed=%" PRIu64,
                  g_last_run_report.blocks_attempted_for_decode,
                  g_last_run_report.blocks_decode_success +
                  g_last_run_report.blocks_decode_failed);
        return -1;
    }

    if ((g_last_run_report.oracle_blocks_theoretically_recoverable +
         g_last_run_report.oracle_blocks_theoretically_unrecoverable) !=
        g_last_run_report.blocks_total_expected) {
        LOG_ERROR("[sim_runner] oracle block accounting mismatch: recoverable=%" PRIu64
                  " unrecoverable=%" PRIu64 " expected=%" PRIu64,
                  g_last_run_report.oracle_blocks_theoretically_recoverable,
                  g_last_run_report.oracle_blocks_theoretically_unrecoverable,
                  g_last_run_report.blocks_total_expected);
        return -1;
    }

    if (g_last_run_report.oracle_valid_for_run &&
        g_last_run_report.oracle_recoverable_but_not_decoded_successfully !=
        (g_last_run_report.oracle_recoverable_but_discarded_before_decode +
         g_last_run_report.oracle_recoverable_but_decode_failed)) {
        LOG_ERROR("[sim_runner] oracle recoverable mismatch: not_decoded=%" PRIu64
                  " discarded=%" PRIu64 " decode_failed=%" PRIu64,
                  g_last_run_report.oracle_recoverable_but_not_decoded_successfully,
                  g_last_run_report.oracle_recoverable_but_discarded_before_decode,
                  g_last_run_report.oracle_recoverable_but_decode_failed);
        return -1;
    }

    if (!g_last_run_report.oracle_valid_for_run &&
        (g_last_run_report.oracle_recoverable_but_not_decoded_successfully != 0U ||
         g_last_run_report.oracle_recoverable_but_discarded_before_decode != 0U ||
         g_last_run_report.oracle_recoverable_but_decode_failed != 0U)) {
        LOG_ERROR("[sim_runner] oracle mismatch counters must be zero when oracle is invalid");
        return -1;
    }

    return 0;
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
    printf("- blocks_total_expected: %" PRIu64 "\n",
           g_last_run_report.blocks_total_expected);
    printf("- blocks_attempted_for_decode: %" PRIu64 "\n",
           g_last_run_report.blocks_attempted_for_decode);
    printf("- blocks_decode_success: %" PRIu64 "\n",
           g_last_run_report.blocks_decode_success);
    printf("- blocks_decode_failed: %" PRIu64 "\n",
           g_last_run_report.blocks_decode_failed);
    printf("- blocks_discarded_before_decode: %" PRIu64 "\n",
           g_last_run_report.blocks_discarded_before_decode);
    printf("- blocks_discarded_timeout_before_decode: %" PRIu64 "\n",
           g_last_run_report.blocks_discarded_timeout_before_decode);
    printf("- blocks_discarded_too_many_holes_before_decode: %" PRIu64 "\n",
           g_last_run_report.blocks_discarded_too_many_holes_before_decode);
    printf("- blocks_discarded_evicted_before_decode: %" PRIu64 "\n",
           g_last_run_report.blocks_discarded_evicted_before_decode);
    printf("- blocks_discarded_ready_evicted_before_mark: %" PRIu64 "\n",
           g_last_run_report.blocks_discarded_ready_evicted_before_mark);
    printf("Packet-level failures:\n");
    printf("- packet_fail_missing: %" PRIu64 "\n",
           g_last_run_report.packet_fail_missing);
    printf("- packet_fail_corrupted: %" PRIu64 "\n",
           g_last_run_report.packet_fail_corrupted);
    printf("- packet_fail_due_to_missing_blocks: %" PRIu64 "\n",
           g_last_run_report.packet_fail_due_to_missing_blocks);
    printf("- packet_fail_after_successful_block_decode: %" PRIu64 "\n",
           g_last_run_report.packet_fail_after_successful_block_decode);
    printf("- max_missing_symbols_in_block: %" PRIu64 "\n",
           g_last_run_report.max_missing_symbols_in_block);
    printf("- avg_missing_symbols_in_failed_blocks: %.2f\n",
           g_last_run_report.avg_missing_symbols_in_failed_blocks);
    printf("[ERASURE ORACLE SUMMARY]\n");
    printf("- oracle_valid_for_run: %s\n",
           g_last_run_report.oracle_valid_for_run ? "true" : "false");
    printf("- oracle_blocks_theoretically_recoverable: %" PRIu64 "\n",
           g_last_run_report.oracle_blocks_theoretically_recoverable);
    printf("- oracle_blocks_theoretically_unrecoverable: %" PRIu64 "\n",
           g_last_run_report.oracle_blocks_theoretically_unrecoverable);
    printf("- oracle_recoverable_but_not_decoded_successfully: %" PRIu64 "\n",
           g_last_run_report.oracle_recoverable_but_not_decoded_successfully);
    printf("- oracle_unrecoverable_and_not_decoded_successfully: %" PRIu64 "\n",
           g_last_run_report.oracle_unrecoverable_and_not_decoded_successfully);
    printf("- oracle_recoverable_but_discarded_before_decode: %" PRIu64 "\n",
           g_last_run_report.oracle_recoverable_but_discarded_before_decode);
    printf("- oracle_recoverable_but_decode_failed: %" PRIu64 "\n",
           g_last_run_report.oracle_recoverable_but_decode_failed);
}

/* =========================================================================
 * Packet generation
 * =========================================================================*/

static int sr_generate_packets(sr_ctx_t *ctx, uint32_t seed)
{
    int    slots_remaining;
    int    max_packets;
    int    i;

    if (ctx == NULL) {
        return -1;
    }

    slots_remaining = ctx->total_src_slots;
    max_packets     = sr_max_packets(ctx);

    for (i = 0; i < max_packets && slots_remaining > 0; ++i) {
        size_t           sz;
        int              nf;
        sr_pkt_record_t *pr;
        size_t           j;

        sz = sr_pkt_sizes[i % SR_NUM_PKT_SIZES];

        nf = (int)((sz + (size_t)ctx->symbol_size - 1U) /
                   (size_t)ctx->symbol_size);
        if (nf <= 0) {
            nf = 1;
        }

        if (nf > SR_MAX_FRAGS_PER_PKT) {
            nf = SR_MAX_FRAGS_PER_PKT;
            sz = (size_t)nf * (size_t)ctx->symbol_size;
        }

        if (nf > slots_remaining) {
            continue;
        }

        pr = &ctx->pkts[ctx->num_generated];
        pr->packet_id = (uint32_t)(ctx->num_generated + 1);
        pr->packet_len = sz;
        pr->transmitted = 0;
        pr->recovered = 0;
        pr->exact_match = 0;
        pr->num_contributing_blocks = 0;

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
    block_builder_t  bb;
    interleaver_t   *il         = NULL;
    fec_handle_t     fec        = NULL;
    symbol_t        *frag_buf   = NULL;
    symbol_t        *repair_buf = NULL;
    unsigned char   *src_data   = NULL;
    int              block_slot = 0;
    int              result     = -1;
    int              pi;

    if (ctx == NULL) {
        return -1;
    }

    memset(&bb, 0, sizeof(bb));

    ctx->tx_buf = (symbol_t *)calloc((size_t)ctx->total_syms, sizeof(symbol_t));
    if (!ctx->tx_buf) {
        return -1;
    }

    frag_buf = (symbol_t *)calloc((size_t)SR_MAX_FRAGS_PER_PKT, sizeof(symbol_t));
    repair_buf = (symbol_t *)calloc((size_t)ctx->m, sizeof(symbol_t));
    src_data = (unsigned char *)sr_alloc_aligned(
        (size_t)ctx->k * (size_t)ctx->symbol_size
    );

    if (!frag_buf || !repair_buf || !src_data) {
        goto cleanup;
    }

    if (block_builder_init(&bb, (uint16_t)ctx->k) != 0) {
        goto cleanup_bb;
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

            sr_record_packet_block_dependency(ctx,
                                              sym->packet_id,
                                              ctx->num_blocks_encoded);

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
                }

                stats_inc_recovered(pr->packet_len);
            } else {
                stats_inc_failed_packet();
            }
        }

        sr_pkt_accum_remove(accum, accum_count, idx);
    }
}

/* =========================================================================
 * RX pipeline
 * =========================================================================*/

static int sr_service_ready_blocks(sr_ctx_t *ctx,
                                   deinterleaver_t *dil,
                                   fec_handle_t fec,
                                   unsigned char *recon,
                                   sr_pkt_accum_t *accum,
                                   int *accum_count,
                                   int max_accum)
{
    block_t blk;

    if (ctx == NULL || dil == NULL || fec == NULL || recon == NULL ||
        accum == NULL || accum_count == NULL) {
        return -1;
    }

    while (deinterleaver_get_ready_block(dil, &blk) == 0) {
        int                    blk_idx = (int)blk.block_id;
        uint64_t               holes = 0U;
        fec_decode_telemetry_t telemetry;
        int                    decode_rc;
        int                    failure_reason = SR_FAIL_NONE;

        if (blk.symbols_per_block > blk.symbol_count) {
            holes = (uint64_t)(blk.symbols_per_block - blk.symbol_count);
        }

        ctx->stat_blocks_attempted++;
        g_last_run_report.blocks_attempted_for_decode++;
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

        sr_update_decode_telemetry(ctx, &telemetry, decode_rc == FEC_DECODE_OK);
        sr_print_decoder_telemetry((uint64_t)blk.block_id, &telemetry, ctx->m);
        sr_print_block_analysis((uint64_t)blk.block_id,
                                blk.symbols_per_block,
                                &telemetry,
                                failure_reason);

        if (decode_rc == FEC_DECODE_OK) {
            ctx->stat_blocks_ok++;
            stats_inc_block_success();

            if (blk_idx >= 0 && blk_idx < ctx->num_blocks_encoded) {
                sr_deliver_block(ctx, blk_idx, recon, accum, accum_count, max_accum);
            }

            if (deinterleaver_mark_result(dil, (uint32_t)blk.block_id, 1) != 0) {
                return -1;
            }
        } else {
            ctx->stat_blocks_failed++;
            stats_inc_block_failure();
            if (deinterleaver_mark_result(dil, (uint32_t)blk.block_id, 0) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

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

    dil_slots = ctx->depth * SR_DIL_HEADROOM_FACTOR;

    dil = deinterleaver_create(dil_slots,
                               ctx->n,
                               ctx->k,
                               (size_t)ctx->symbol_size,
                               0.0,
                               1.0e9);
    if (!dil) {
        goto cleanup;
    }

    if (deinterleaver_set_block_final_callback(dil,
                                               sr_deinterleaver_block_final_cb,
                                               ctx) != 0) {
        goto cleanup;
    }

    if (deinterleaver_set_eviction_callback(dil,
                                            sr_deinterleaver_eviction_cb,
                                            ctx) != 0) {
        goto cleanup;
    }

    fec = fec_create(ctx->k, ctx->symbol_size);
    if (!fec) {
        goto cleanup;
    }

    recon = (unsigned char *)sr_alloc_aligned(
        (size_t)ctx->k * (size_t)ctx->symbol_size
    );
    accum = (sr_pkt_accum_t *)calloc((size_t)max_accum, sizeof(sr_pkt_accum_t));

    if (!recon || !accum) {
        goto cleanup;
    }

    for (i = 0; i < ctx->tx_count; ++i) {
        symbol_t *sym = &ctx->tx_buf[i];
        int       rc;

        if (deinterleaver_ready_count(dil) > 0) {
            if (sr_service_ready_blocks(ctx,
                                        dil,
                                        fec,
                                        recon,
                                        accum,
                                        &accum_count,
                                        max_accum) != 0) {
                goto cleanup;
            }
        }

        /*
         * Skip symbols for block_ids that have already been finalized.
         *
         * With immediate-promotion (stabilization_ms == 0.0), a block can
         * be promoted to READY_TO_DECODE, decoded, and its slot cleared via
         * mark_result() before all interleaved symbols for that block have
         * arrived.  Without this guard, a late-arriving symbol would cause
         * alloc_slot() to open a fresh slot for an already-finished block_id.
         * That re-opened slot is then recycled by the end-of-run tick(0.0),
         * which fires the block_final_cb a second time for the same block_id
         * and produces a spurious "duplicate final reason" warning.
         *
         * Dropping the late symbol here is correct: the block was already
         * decoded successfully, so the symbol carries no new information.
         */
        {
            int blk_id = (int)sym->packet_id;

            if (blk_id >= 0 && blk_id < ctx->num_blocks_encoded &&
                ctx->block_final_state != NULL &&
                ctx->block_final_state[blk_id] != SR_BLOCK_FINAL_UNKNOWN) {
                /* Block already finalized — drop the late symbol silently. */
                continue;
            }
        }

        rc = deinterleaver_push_symbol(dil, sym);
        if (rc < 0) {
            if (deinterleaver_ready_count(dil) > 0) {
                if (sr_service_ready_blocks(ctx,
                                            dil,
                                            fec,
                                            recon,
                                            accum,
                                            &accum_count,
                                            max_accum) != 0) {
                    goto cleanup;
                }
            }
            continue;
        }

        if (rc > 0 || deinterleaver_ready_count(dil) > 0) {
            if (sr_service_ready_blocks(ctx,
                                        dil,
                                        fec,
                                        recon,
                                        accum,
                                        &accum_count,
                                        max_accum) != 0) {
                goto cleanup;
            }
        }
    }

    deinterleaver_tick(dil, 0.0);
    if (sr_service_ready_blocks(ctx,
                                dil,
                                fec,
                                recon,
                                accum,
                                &accum_count,
                                max_accum) != 0) {
        goto cleanup;
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
                if (sr_packet_missing_due_to_missing_blocks(ctx, pr)) {
                    g_last_run_report.packet_fail_due_to_missing_blocks++;
                } else {
                    g_last_run_report.packet_fail_after_successful_block_decode++;
                }
                stats_inc_failed_packet();
            } else if (!pr->exact_match) {
                g_last_run_report.packet_fail_corrupted++;
            }
        }
    }

    if (sr_finalize_run_report(ctx) != 0) {
        goto cleanup;
    }

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
    sr_reset_oracle_block_diags();

    if (out_channel) {
        memset(out_channel, 0, sizeof(*out_channel));
    }

    g_last_run_report.oracle_valid_for_run = !sr_run_has_corruption_events(req);

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

    ctx.block_final_state = (int *)malloc((size_t)ctx.num_blocks * sizeof(int));
    if (!ctx.block_final_state) {
        goto done;
    }
    for (b = 0; b < ctx.num_blocks; ++b) {
        ctx.block_final_state[b] = SR_BLOCK_FINAL_UNKNOWN;
    }

    ctx.block_eviction_info = (dil_eviction_info_t *)calloc(
        (size_t)ctx.num_blocks, sizeof(dil_eviction_info_t));
    if (!ctx.block_eviction_info) {
        goto done;
    }

    ctx.block_has_eviction = (bool *)calloc((size_t)ctx.num_blocks, sizeof(bool));
    if (!ctx.block_has_eviction) {
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

    free(ctx.block_final_state);
    free(ctx.block_eviction_info);
    free(ctx.block_has_eviction);
    free(ctx.tx_buf);

    return rc;
}