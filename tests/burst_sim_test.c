/*
 * tests/burst_sim_test.c — Automated Stress Testing Framework
 *
 * Turns the burst simulation into a reusable diagnostic harness with three
 * sweep modes:
 *
 *   1. Burst Length Sweep
 *   2. Burst Start Sweep
 *   3. Exact Boundary Test (drop M-1 / M / M+1 from one block)
 *
 * Usage:
 *   ./build/burst_sim_test [all|length|start|boundary]
 *
 * Core invariants preserved:
 *   - Erasures are represented by zeroing symbol_t so payload_len == 0
 *     is the erasure sentinel.
 *   - The push loop skips payload_len == 0 symbols.
 *   - deinterleaver_tick(dil, 0.0) is called at end-of-stream to flush
 *     any partial blocks.
 *   - FEC decode uses sparse scan_capacity = block.symbols_per_block.
 *   - fec_decode_block() return codes are tested against the public
 *     FEC_DECODE_OK / FEC_DECODE_ERR / FEC_DECODE_TOO_MANY_HOLES constants.
 *
 * Updated for the new lifecycle:
 *   - deinterleaver_create() now takes 6 arguments:
 *       (max_active, spb, k, symbol_size, stabilization_ms, max_age_ms)
 *   - drain_ready_blocks() calls deinterleaver_mark_result() after each
 *     get_ready_block() to complete the READY_TO_DECODE → EMPTY cycle.
 *   - The inline slot-pressure handler in run_iteration_internal also
 *     calls mark_result() after it drains a slot.
 */

#define _POSIX_C_SOURCE 200112L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wirehair/wirehair.h>

#include "deinterleaver.h"
#include "fec_wrapper.h"
#include "interleaver.h"
#include "logging.h"
#include "symbol.h"
#include "types.h"

/* -------------------------------------------------------------------------- */
/* Default geometry                                                            */
/* -------------------------------------------------------------------------- */

#define DEFAULT_K            64
#define DEFAULT_M            32
#define DEFAULT_DEPTH        100
#define DEFAULT_SYMBOL_SIZE  1500
#define DIL_HEADROOM         20

/*
 * Timing parameters for deinterleaver_create().
 * stabilization_ms = 0 disables the quiet-period path (burst_sim feeds full
 * blocks so stabilization is not needed).
 * max_age_ms = 0 disables automatic age-based transitions (tick(0.0) is used
 * explicitly at end-of-stream for the flush).
 */
#define DIL_STAB_MS    0.0
#define DIL_MAX_AGE_MS 0.0

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* -------------------------------------------------------------------------- */
/* Modes                                                                       */
/* -------------------------------------------------------------------------- */

typedef enum {
    TEST_MODE_ALL = 0,
    TEST_MODE_BURST_LENGTH,
    TEST_MODE_BURST_START,
    TEST_MODE_EXACT_BOUNDARY
} test_mode_t;

typedef enum {
    ERASURE_MODE_BURST = 0,
    ERASURE_MODE_EXACT_BLOCK
} erasure_mode_t;

/* -------------------------------------------------------------------------- */
/* Per-block accounting                                                        */
/* -------------------------------------------------------------------------- */

typedef struct {
    unsigned char *source;
    unsigned char *reconstructed;
    int            symbols_lost;
    int            decoded_ok;
    int            data_match;
} block_record_t;

typedef struct {
    int total_blocks;
    int recovered_blocks;
    int failed_blocks;
    int total_symbols_erased;
    int all_blocks_pass;

    int target_block;
    int target_block_decoded_ok;
    int target_block_data_match;
    int target_block_symbols_lost;
} iteration_result_t;

typedef struct {
    char section[48];
    char label[128];
    int  expected_pass;
    int  actual_pass;
} summary_row_t;

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

static void *alloc_aligned(size_t size)
{
    void *ptr = NULL;

    if (size == 0U || posix_memalign(&ptr, 64U, size) != 0) {
        return NULL;
    }

    memset(ptr, 0, size);
    return ptr;
}

static void fill_source(unsigned char *buf, size_t len, uint32_t block_id)
{
    size_t i;

    for (i = 0; i < len; ++i) {
        buf[i] = (unsigned char)(
            (block_id * 251U + (uint32_t)i * 37U + (uint32_t)(i >> 8))
            & 0xFFU);
    }
}

/*
 * drain_ready_blocks() — retrieve all READY_TO_DECODE blocks, decode each
 * with FEC, then call mark_result() to recycle the slot to EMPTY.
 *
 * mark_result() is MANDATORY after every get_ready_block() call under the
 * new lifecycle.  Without it the slot stays READY_TO_DECODE and subsequent
 * get_ready_block() calls would return the same block repeatedly, eventually
 * exhausting the slot pool.
 */
static void drain_ready_blocks(deinterleaver_t *dil,
                               fec_handle_t     fec_dec,
                               block_record_t  *records,
                               int              depth,
                               size_t           source_bytes)
{
    block_t *block;
    int      rc;

    while ((block = deinterleaver_get_ready_block(dil)) != NULL) {
        int blk_id = (int)block->block_id;
        int fec_rc;

        if (blk_id < 0 || blk_id >= depth) {
            deinterleaver_mark_result(dil, (uint32_t)block->block_id, 0);
            continue;
        }

        if (records[blk_id].decoded_ok) {
            deinterleaver_mark_result(dil, (uint32_t)block->block_id, 1);
            continue;
        }

        memset(records[blk_id].reconstructed, 0, source_bytes);

        fec_rc = fec_decode_block(fec_dec,
                                  block->symbols,
                                  block->symbol_count,
                                  block->symbols_per_block,
                                  records[blk_id].reconstructed);

        records[blk_id].decoded_ok = (fec_rc == FEC_DECODE_OK);

        if (fec_rc == FEC_DECODE_OK) {
            records[blk_id].data_match =
                (memcmp(records[blk_id].source,
                        records[blk_id].reconstructed,
                        source_bytes) == 0);
        }

        rc = deinterleaver_mark_result(dil,
                                       (uint32_t)block->block_id,
                                       (fec_rc == FEC_DECODE_OK) ? 1 : 0);
        if (rc != 0) {
            LOG_WARN("[BSIM] drain_ready_blocks: mark_result failed for "
                     "block_id=%u", (unsigned)block->block_id);
        }
    }
}

static int erase_burst(symbol_t       *tx_buffer,
                       int             total_symbols,
                       int             burst_start,
                       int             burst_len,
                       block_record_t *records,
                       int             depth)
{
    int i;
    int erased = 0;

    if (burst_len < 0 || burst_start < 0 ||
        burst_start + burst_len > total_symbols) {
        LOG_ERROR("[BSIM] erase_burst: invalid range start=%d len=%d total=%d",
                  burst_start, burst_len, total_symbols);
        return -1;
    }

    for (i = burst_start; i < burst_start + burst_len; ++i) {
        uint32_t bid = tx_buffer[i].packet_id;

        if ((int)bid >= 0 && (int)bid < depth) {
            records[bid].symbols_lost++;
        }

        memset(&tx_buffer[i], 0, sizeof(symbol_t));
        erased++;
    }

    return erased;
}

static int erase_exact_block_losses(symbol_t       *tx_buffer,
                                    int             total_symbols,
                                    int             target_block,
                                    int             losses_to_drop,
                                    block_record_t *records,
                                    int             depth,
                                    int             n)
{
    int *pos_by_fec = NULL;
    int  i, f;
    int  erased = 0;

    if (target_block < 0 || target_block >= depth ||
        losses_to_drop < 0 || losses_to_drop > n) {
        LOG_ERROR("[BSIM] erase_exact_block_losses: invalid params "
                  "block=%d losses=%d n=%d",
                  target_block, losses_to_drop, n);
        return -1;
    }

    pos_by_fec = (int *)malloc((size_t)n * sizeof(int));
    if (pos_by_fec == NULL) {
        LOG_ERROR("[BSIM] erase_exact_block_losses: malloc failed");
        return -1;
    }

    for (f = 0; f < n; ++f) { pos_by_fec[f] = -1; }

    for (i = 0; i < total_symbols; ++i) {
        if ((int)tx_buffer[i].packet_id == target_block) {
            int fec_id = (int)tx_buffer[i].fec_id;
            if (fec_id >= 0 && fec_id < n && pos_by_fec[fec_id] < 0) {
                pos_by_fec[fec_id] = i;
            }
        }
    }

    for (f = 0; f < losses_to_drop; ++f) {
        int pos = pos_by_fec[f];

        if (pos < 0) {
            LOG_ERROR("[BSIM] erase_exact_block_losses: missing "
                      "block=%d fec_id=%d in tx_buffer",
                      target_block, f);
            free(pos_by_fec);
            return -1;
        }

        if (tx_buffer[pos].payload_len == 0) {
            LOG_ERROR("[BSIM] erase_exact_block_losses: duplicate erase "
                      "pos=%d block=%d fec_id=%d",
                      pos, target_block, f);
            free(pos_by_fec);
            return -1;
        }

        records[target_block].symbols_lost++;
        memset(&tx_buffer[pos], 0, sizeof(symbol_t));
        erased++;
    }

    free(pos_by_fec);
    return erased;
}

/* -------------------------------------------------------------------------- */
/* Core simulation                                                             */
/* -------------------------------------------------------------------------- */

static int run_iteration_internal(int                 burst_len,
                                  int                 burst_start,
                                  int                 depth,
                                  int                 k,
                                  int                 m,
                                  erasure_mode_t      erasure_mode,
                                  int                 target_block,
                                  int                 exact_losses,
                                  int                 crc_enabled,
                                  iteration_result_t *out)
{
    const int        n             = k + m;
    const int        total_symbols = depth * n;
    const size_t     source_bytes  = (size_t)k * DEFAULT_SYMBOL_SIZE;
    const int        dil_capacity  = depth + DIL_HEADROOM;

    fec_handle_t     fec_enc   = NULL;
    fec_handle_t     fec_dec   = NULL;
    interleaver_t   *il        = NULL;
    deinterleaver_t *dil       = NULL;
    symbol_t        *tx_buffer = NULL;
    block_record_t  *records   = NULL;

    int ready_signal_seen = 0;
    int tx_count          = 0;
    int erased_count      = 0;
    int result            = -1;
    int b, i, rc;

    if (out == NULL) { return -1; }

    memset(out, 0, sizeof(*out));
    out->total_blocks = depth;
    out->target_block = target_block;

    if (depth < 1 || k < 2 || m < 0) {
        LOG_ERROR("[BSIM] invalid geometry depth=%d k=%d m=%d", depth, k, m);
        return -1;
    }

    if (erasure_mode == ERASURE_MODE_BURST) {
        if (burst_len < 0 || burst_start < 0 ||
            burst_start + burst_len > total_symbols) {
            LOG_ERROR("[BSIM] invalid burst config start=%d len=%d total=%d",
                      burst_start, burst_len, total_symbols);
            return -1;
        }
    }

    tx_buffer = (symbol_t *)calloc((size_t)total_symbols, sizeof(symbol_t));
    records   = (block_record_t *)calloc((size_t)depth, sizeof(block_record_t));

    if (tx_buffer == NULL || records == NULL) {
        LOG_ERROR("[BSIM] calloc failed");
        goto cleanup;
    }

    for (b = 0; b < depth; ++b) {
        records[b].source        = (unsigned char *)alloc_aligned(source_bytes);
        records[b].reconstructed = (unsigned char *)alloc_aligned(source_bytes);

        if (records[b].source == NULL || records[b].reconstructed == NULL) {
            LOG_ERROR("[BSIM] alloc_aligned failed for block %d", b);
            goto cleanup;
        }
    }

    fec_enc = fec_create(k, DEFAULT_SYMBOL_SIZE);
    fec_dec = fec_create(k, DEFAULT_SYMBOL_SIZE);
    if (fec_enc == NULL || fec_dec == NULL) {
        LOG_ERROR("[BSIM] fec_create failed");
        goto cleanup;
    }

    il = interleaver_create(depth, n, DEFAULT_SYMBOL_SIZE, 0);

    /*
     * deinterleaver_create() — updated 6-argument signature:
     *   (max_active, symbols_per_block, k, symbol_size,
     *    stabilization_ms, block_max_age_ms)
     *
     * DIL_STAB_MS  = 0.0 → stabilization quiet-period disabled.
     * DIL_MAX_AGE_MS = 0.0 → age-based auto-transition disabled;
     *                         the explicit tick(0.0) at end-of-stream
     *                         performs the flush.
     */
    dil = deinterleaver_create(dil_capacity, n, k,
                               DEFAULT_SYMBOL_SIZE,
                               DIL_STAB_MS, DIL_MAX_AGE_MS);

    if (il == NULL || dil == NULL) {
        LOG_ERROR("[BSIM] interleaver/deinterleaver create failed");
        goto cleanup;
    }

    /* ------------------------------------------------------------------ */
    /* Encode and interleave all blocks                                    */
    /* ------------------------------------------------------------------ */
    for (b = 0; b < depth; ++b) {
        symbol_t *repair_syms = NULL;
        int       f;

        repair_syms = (symbol_t *)calloc((size_t)m, sizeof(symbol_t));
        if (repair_syms == NULL && m > 0) {
            LOG_ERROR("[BSIM] repair_syms calloc failed for block %d", b);
            goto cleanup;
        }

        fill_source(records[b].source, source_bytes, (uint32_t)b);

        if (m > 0 &&
            fec_encode_block(fec_enc, records[b].source,
                             repair_syms, m) != FEC_DECODE_OK) {
            free(repair_syms);
            LOG_ERROR("[BSIM] fec_encode_block failed for block %d", b);
            goto cleanup;
        }

        for (f = 0; f < k; ++f) {
            symbol_t sym;
            memset(&sym, 0, sizeof(sym));
            sym.packet_id     = (uint32_t)b;
            sym.fec_id        = (uint32_t)f;
            sym.payload_len   = (uint16_t)DEFAULT_SYMBOL_SIZE;
            sym.total_symbols = (uint16_t)n;
            memcpy(sym.data,
                   records[b].source + (size_t)f * DEFAULT_SYMBOL_SIZE,
                   (size_t)DEFAULT_SYMBOL_SIZE);

            /* Stamp CRC after all metadata and payload are finalised. */
            if (crc_enabled) {
                symbol_compute_crc(&sym);
            }

            rc = interleaver_push_symbol(il, &sym);
            if (rc < 0) {
                free(repair_syms);
                LOG_ERROR("[BSIM] push source failed block=%d fec=%d", b, f);
                goto cleanup;
            }
        }

        for (f = 0; f < m; ++f) {
            repair_syms[f].packet_id     = (uint32_t)b;
            repair_syms[f].total_symbols = (uint16_t)n;

            if (repair_syms[f].payload_len == 0) {
                free(repair_syms);
                LOG_ERROR("[BSIM] repair payload_len=0 block=%d idx=%d", b, f);
                goto cleanup;
            }

            /* Stamp CRC on repair symbol after all metadata is set. */
            if (crc_enabled) {
                symbol_compute_crc(&repair_syms[f]);
            }

            rc = interleaver_push_symbol(il, &repair_syms[f]);
            if (rc < 0) {
                unsigned int bad_fec = (unsigned)repair_syms[f].fec_id;
                free(repair_syms);
                LOG_ERROR("[BSIM] push repair failed block=%d idx=%d fec_id=%u",
                          b, f, bad_fec);
                goto cleanup;
            }

            if (rc == 1) { ready_signal_seen = 1; }
        }

        if (m == 0 && b == depth - 1) { ready_signal_seen = 1; }

        free(repair_syms);
    }

    if (!ready_signal_seen || !interleaver_is_ready(il)) {
        LOG_ERROR("[BSIM] interleaver not ready after full load");
        goto cleanup;
    }

    /* ------------------------------------------------------------------ */
    /* Drain interleaver → tx_buffer                                      */
    /* ------------------------------------------------------------------ */
    while (tx_count < total_symbols) {
        rc = interleaver_pop_ready_symbol(il, &tx_buffer[tx_count]);
        if (rc < 0) {
            LOG_ERROR("[BSIM] pop failed at tx_count=%d", tx_count);
            goto cleanup;
        }
        tx_count++;
        if (rc == 1) { break; }
    }

    if (tx_count != total_symbols) {
        LOG_ERROR("[BSIM] drained %d instead of %d symbols",
                  tx_count, total_symbols);
        goto cleanup;
    }

    /* ------------------------------------------------------------------ */
    /* Apply erasures                                                      */
    /* ------------------------------------------------------------------ */
    if (erasure_mode == ERASURE_MODE_BURST) {
        erased_count = erase_burst(tx_buffer, total_symbols,
                                   burst_start, burst_len, records, depth);
    } else {
        erased_count = erase_exact_block_losses(tx_buffer, total_symbols,
                                                target_block, exact_losses,
                                                records, depth, n);
    }

    if (erased_count < 0) { goto cleanup; }

    out->total_symbols_erased = erased_count;

    /* ------------------------------------------------------------------ */
    /* Push received symbols into deinterleaver                           */
    /* ------------------------------------------------------------------ */
    {
        int skipped = 0;
        int pushed  = 0;

        for (i = 0; i < total_symbols; ++i) {
            if (tx_buffer[i].payload_len == 0) {
                skipped++;
                continue;
            }

            /*
             * CRC pre-filter: verify integrity before pushing to deinterleaver.
             * Only active when crc_enabled != 0.
             * In the burst-erasure model, no corruption is injected, so a CRC
             * failure here indicates a software bug rather than a channel event.
             * Drop the symbol and log an error so the problem is visible.
             */
            if (crc_enabled && !symbol_verify_crc(&tx_buffer[i])) {
                LOG_ERROR("[BSIM] CRC verification failed for symbol at index=%d "
                          "block_id=%u fec_id=%u — dropping (unexpected in erasure-only test)",
                          i,
                          (unsigned)tx_buffer[i].packet_id,
                          (unsigned)tx_buffer[i].fec_id);
                skipped++;
                continue;
            }

            rc = deinterleaver_push_symbol(dil, &tx_buffer[i]);

            if (rc < 0) {
                /*
                 * Slot pool exhausted — drain one ready block to free a slot,
                 * then retry.  Under the new lifecycle, drain_ready_blocks()
                 * calls mark_result() for every retrieved block.
                 */
                drain_ready_blocks(dil, fec_dec, records, depth, source_bytes);

                rc = deinterleaver_push_symbol(dil, &tx_buffer[i]);
                if (rc < 0) {
                    LOG_ERROR("[BSIM] symbol permanently rejected "
                              "block=%u fec=%u",
                              (unsigned)tx_buffer[i].packet_id,
                              (unsigned)tx_buffer[i].fec_id);
                    goto cleanup;
                }
            }

            pushed++;
        }

        if (skipped != erased_count) {
            LOG_ERROR("[BSIM] skipped=%d but erased=%d", skipped, erased_count);
            goto cleanup;
        }

        if (pushed + skipped != total_symbols) {
            LOG_ERROR("[BSIM] accounting error pushed=%d skipped=%d total=%d",
                      pushed, skipped, total_symbols);
            goto cleanup;
        }
    }

    /* ------------------------------------------------------------------ */
    /* End-of-stream flush + final drain                                  */
    /* ------------------------------------------------------------------ */
    (void)deinterleaver_tick(dil, 0.0);
    drain_ready_blocks(dil, fec_dec, records, depth, source_bytes);

    /* ------------------------------------------------------------------ */
    /* Tally results                                                       */
    /* ------------------------------------------------------------------ */
    for (b = 0; b < depth; ++b) {
        if (records[b].decoded_ok && records[b].data_match) {
            out->recovered_blocks++;
        } else {
            out->failed_blocks++;
        }
    }

    out->all_blocks_pass = (out->failed_blocks == 0);

    if (target_block >= 0 && target_block < depth) {
        out->target_block_decoded_ok   = records[target_block].decoded_ok;
        out->target_block_data_match   = records[target_block].data_match;
        out->target_block_symbols_lost = records[target_block].symbols_lost;
    }

    result = 0;

cleanup:
    if (records != NULL) {
        for (b = 0; b < depth; ++b) {
            free(records[b].source);
            free(records[b].reconstructed);
        }
    }

    free(records);
    free(tx_buffer);

    deinterleaver_destroy(dil);
    interleaver_destroy(il);
    fec_destroy(fec_enc);
    fec_destroy(fec_dec);

    return result;
}

static int run_iteration(int burst_len, int burst_start,
                         int depth, int k, int m,
                         int crc_enabled,
                         iteration_result_t *out)
{
    return run_iteration_internal(burst_len, burst_start, depth, k, m,
                                  ERASURE_MODE_BURST, -1, 0,
                                  crc_enabled, out);
}

static int run_exact_boundary_iteration(int target_block, int losses,
                                        int depth, int k, int m,
                                        int crc_enabled,
                                        iteration_result_t *out)
{
    return run_iteration_internal(0, 0, depth, k, m,
                                  ERASURE_MODE_EXACT_BLOCK,
                                  target_block, losses,
                                  crc_enabled, out);
}

/* -------------------------------------------------------------------------- */
/* Summary helpers                                                             */
/* -------------------------------------------------------------------------- */

static void append_summary(summary_row_t *rows, int *row_count,
                           const char *section, const char *label,
                           int expected_pass, int actual_pass)
{
    summary_row_t *row = &rows[*row_count];
    snprintf(row->section, sizeof(row->section), "%s", section);
    snprintf(row->label,   sizeof(row->label),   "%s", label);
    row->expected_pass = expected_pass;
    row->actual_pass   = actual_pass;
    (*row_count)++;
}

static void print_summary(const summary_row_t *rows, int row_count)
{
    int i, matched = 0, mismatches = 0;

    printf("\n===============================================================\n");
    printf("Automated Stress Test Summary\n");
    printf("===============================================================\n");
    printf("%-18s %-36s %-10s %-10s\n",
           "Section", "Configuration", "Expected", "Actual");

    for (i = 0; i < row_count; ++i) {
        printf("%-18s %-36s %-10s %-10s%s\n",
               rows[i].section, rows[i].label,
               rows[i].expected_pass ? "PASS" : "FAIL",
               rows[i].actual_pass   ? "PASS" : "FAIL",
               (rows[i].expected_pass == rows[i].actual_pass)
                   ? "" : "  <-- MISMATCH");

        if (rows[i].expected_pass == rows[i].actual_pass) { matched++;    }
        else                                               { mismatches++; }
    }

    printf("---------------------------------------------------------------\n");
    printf("Checks matched expectation : %d / %d\n", matched, row_count);
    printf("Expectation mismatches     : %d\n", mismatches);
    printf("===============================================================\n\n");
}

/* -------------------------------------------------------------------------- */
/* Test suites                                                                 */
/* -------------------------------------------------------------------------- */

static int run_burst_length_sweep(summary_row_t *rows, int *row_count,
                                  int crc_enabled)
{
    static const int lengths[] = {1, 10, 32, 33, 50, 100, 200, 500,
                                   3200, 3201};
    int failures = 0, i;

    printf("\n=== Burst Length Sweep ===\n");
    printf("%-12s %-12s %-12s %-12s %-12s\n",
           "BurstLen", "Expected", "Actual", "Recovered", "Failed");

    for (i = 0; i < ARRAY_LEN(lengths); ++i) {
        iteration_result_t out;
        char               label[128];
        int                expected_pass = (lengths[i] <= 3200);
        int                actual_pass;

        memset(&out, 0, sizeof(out));

        if (run_iteration(lengths[i], 0,
                          DEFAULT_DEPTH, DEFAULT_K, DEFAULT_M,
                          crc_enabled, &out) != 0) {
            actual_pass = 0;
        } else {
            actual_pass = out.all_blocks_pass;
        }

        printf("%-12d %-12s %-12s %-12d %-12d\n",
               lengths[i],
               expected_pass ? "PASS" : "FAIL",
               actual_pass   ? "PASS" : "FAIL",
               out.recovered_blocks, out.failed_blocks);

        snprintf(label, sizeof(label),
                 "burst_len=%d start=0", lengths[i]);
        append_summary(rows, row_count, "Burst Length", label,
                       expected_pass, actual_pass);

        if (actual_pass != expected_pass) { failures++; }
    }

    return failures;
}

static int run_burst_start_sweep(summary_row_t *rows, int *row_count,
                                 int crc_enabled)
{
    const int n         = DEFAULT_K + DEFAULT_M;
    const int total     = DEFAULT_DEPTH * n;
    const int burst_len = 50;
    const int starts[]  = {
        0,
        1,
        DEFAULT_DEPTH - 1,
        DEFAULT_DEPTH,
        DEFAULT_DEPTH + 1,
        total / 2,
        total - burst_len - 1
    };
    int failures = 0, i;

    printf("\n=== Burst Start Sweep (burst_len=%d) ===\n", burst_len);
    printf("%-12s %-12s %-12s %-12s %-12s\n",
           "Start", "Expected", "Actual", "Recovered", "Failed");

    for (i = 0; i < ARRAY_LEN(starts); ++i) {
        iteration_result_t out;
        char               label[128];
        int                actual_pass;

        memset(&out, 0, sizeof(out));

        if (run_iteration(burst_len, starts[i],
                          DEFAULT_DEPTH, DEFAULT_K, DEFAULT_M,
                          crc_enabled, &out) != 0) {
            actual_pass = 0;
        } else {
            actual_pass = out.all_blocks_pass;
        }

        printf("%-12d %-12s %-12s %-12d %-12d\n",
               starts[i], "PASS",
               actual_pass ? "PASS" : "FAIL",
               out.recovered_blocks, out.failed_blocks);

        snprintf(label, sizeof(label),
                 "burst_len=%d start=%d", burst_len, starts[i]);
        append_summary(rows, row_count, "Burst Start", label, 1, actual_pass);

        if (!actual_pass) { failures++; }
    }

    return failures;
}

static int run_exact_boundary_tests(summary_row_t *rows, int *row_count,
                                    int crc_enabled)
{
    const int target_block = 17;
    const int losses[]     = {DEFAULT_M - 1, DEFAULT_M, DEFAULT_M + 1};
    int failures = 0, i;

    printf("\n=== Exact Boundary Test (block=%d) ===\n", target_block);
    printf("%-12s %-12s %-12s %-12s %-12s %-12s\n",
           "Losses", "Expected", "Actual", "Recovered", "Failed", "Target");

    for (i = 0; i < ARRAY_LEN(losses); ++i) {
        iteration_result_t out;
        char               label[128];
        int                expected_pass = (losses[i] <= DEFAULT_M);
        int                actual_pass;

        memset(&out, 0, sizeof(out));

        if (run_exact_boundary_iteration(target_block, losses[i],
                                         DEFAULT_DEPTH, DEFAULT_K, DEFAULT_M,
                                         crc_enabled, &out) != 0) {
            actual_pass = 0;
        } else {
            actual_pass =
                (out.target_block_decoded_ok && out.target_block_data_match);
        }

        printf("%-12d %-12s %-12s %-12d %-12d %-12s\n",
               losses[i],
               expected_pass ? "PASS" : "FAIL",
               actual_pass   ? "PASS" : "FAIL",
               out.recovered_blocks, out.failed_blocks,
               (out.target_block_decoded_ok && out.target_block_data_match)
                   ? "MATCH" : "FAIL");

        snprintf(label, sizeof(label),
                 "block=%d losses=%d", target_block, losses[i]);
        append_summary(rows, row_count, "Exact Boundary", label,
                       expected_pass, actual_pass);

        if (actual_pass != expected_pass) { failures++; }
    }

    return failures;
}

/* -------------------------------------------------------------------------- */
/* Main                                                                        */
/* -------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    summary_row_t rows[32];
    int           row_count   = 0;
    int           failures    = 0;
    test_mode_t   mode        = TEST_MODE_ALL;
    int           crc_enabled = 1;   /* default: CRC enabled */
    int           i;

    memset(rows, 0, sizeof(rows));

    log_init();

    if (wirehair_init() != Wirehair_Success) {
        LOG_ERROR("[BSIM] wirehair_init() failed");
        return 1;
    }

    /*
     * Parse command-line arguments.
     *
     * Accepted forms:
     *   burst_sim_test [mode] [--crc 0|1]
     *
     * The optional mode argument (positional) must come before --crc if both
     * are supplied.  --crc may appear in any remaining position.
     *
     * Examples:
     *   burst_sim_test                    # all modes, CRC enabled
     *   burst_sim_test length             # length sweep, CRC enabled
     *   burst_sim_test --crc 0            # all modes, CRC disabled
     *   burst_sim_test boundary --crc 1   # boundary tests, CRC enabled
     */
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--crc") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --crc requires a value (0 or 1)\n");
                fprintf(stderr,
                        "Usage: %s [all|length|start|boundary] [--crc 0|1]\n",
                        argv[0]);
                return 2;
            }
            ++i;
            if (strcmp(argv[i], "0") == 0) {
                crc_enabled = 0;
            } else if (strcmp(argv[i], "1") == 0) {
                crc_enabled = 1;
            } else {
                fprintf(stderr,
                        "Error: --crc value must be 0 or 1 (got '%s')\n",
                        argv[i]);
                fprintf(stderr,
                        "Usage: %s [all|length|start|boundary] [--crc 0|1]\n",
                        argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "length") == 0) {
            mode = TEST_MODE_BURST_LENGTH;
        } else if (strcmp(argv[i], "start") == 0) {
            mode = TEST_MODE_BURST_START;
        } else if (strcmp(argv[i], "boundary") == 0) {
            mode = TEST_MODE_EXACT_BOUNDARY;
        } else if (strcmp(argv[i], "all") == 0) {
            mode = TEST_MODE_ALL;
        } else {
            fprintf(stderr, "Error: unknown argument '%s'\n", argv[i]);
            fprintf(stderr,
                    "Usage: %s [all|length|start|boundary] [--crc 0|1]\n",
                    argv[0]);
            return 2;
        }
    }

    printf("\n--- Running automated burst/interleaver/FEC stress tests ---\n");
    printf("--- CRC: %s ---\n\n", crc_enabled ? "ENABLED" : "DISABLED");

    if (mode == TEST_MODE_ALL || mode == TEST_MODE_BURST_LENGTH) {
        failures += run_burst_length_sweep(rows, &row_count, crc_enabled);
    }
    if (mode == TEST_MODE_ALL || mode == TEST_MODE_BURST_START) {
        failures += run_burst_start_sweep(rows, &row_count, crc_enabled);
    }
    if (mode == TEST_MODE_ALL || mode == TEST_MODE_EXACT_BOUNDARY) {
        failures += run_exact_boundary_tests(rows, &row_count, crc_enabled);
    }

    print_summary(rows, row_count);

    return (failures == 0) ? 0 : 1;
}
