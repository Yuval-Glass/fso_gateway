/*
 * tests/deinterleaver_test.c — Deinterleaver Round-Trip Verification.
 *
 * Tests a full interleave → deinterleave round-trip and a set of edge-case
 * checks.  Updated for the new deinterleaver_create() signature and the
 * mandatory mark_result() lifecycle.
 *
 * Geometry: Depth=4, N=10, Symbol_Size=1500  →  40 symbols total.
 *
 * Key changes from the original:
 *   - deinterleaver_create() now takes 6 arguments:
 *       (max_active, spb, k, symbol_size, stabilization_ms, max_age_ms)
 *   - Every deinterleaver_get_ready_block() MUST be followed by
 *       deinterleaver_mark_result() to recycle the slot to EMPTY.
 *   - verify_blocks() now calls mark_result() after each retrieved block.
 *   - test_duplicate_rejection() calls mark_result() after retrieval.
 */

#define _POSIX_C_SOURCE 200112L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deinterleaver.h"
#include "interleaver.h"
#include "logging.h"
#include "types.h"

/* -------------------------------------------------------------------------- */
/* Test geometry                                                               */
/* -------------------------------------------------------------------------- */

#define TEST_DEPTH       4
#define TEST_N           10
#define TEST_K           7     /* real K for this test (K < N)               */
#define TEST_SYMBOL_SIZE 1500
#define TEST_TOTAL       (TEST_DEPTH * TEST_N)   /* 40 */

/*
 * Stabilization and max-age timing for deinterleaver_create().
 * In this test blocks complete fully (all N symbols), so the timing
 * paths are never exercised — values are provided for correctness.
 */
#define DIL_STAB_MS    5.0
#define DIL_MAX_AGE_MS 500.0

/* -------------------------------------------------------------------------- */
/* Deterministic payload helper                                                */
/* -------------------------------------------------------------------------- */

static unsigned char expected_byte(uint32_t block_id,
                                   uint32_t fec_id,
                                   size_t   offset)
{
    return (unsigned char)((block_id * 7U + fec_id * 13U + (uint32_t)offset)
                           & 0xFFU);
}

static void fill_symbol(symbol_t *sym, uint32_t block_id, uint32_t fec_id)
{
    size_t i;

    sym->packet_id     = block_id;
    sym->fec_id        = fec_id;
    sym->symbol_index  = (uint16_t)fec_id;
    sym->total_symbols = (uint16_t)TEST_N;
    sym->payload_len   = (uint16_t)TEST_SYMBOL_SIZE;

    for (i = 0; i < TEST_SYMBOL_SIZE && i < MAX_SYMBOL_DATA_SIZE; ++i) {
        sym->data[i] = expected_byte(block_id, fec_id, i);
    }
}

/* -------------------------------------------------------------------------- */
/* Result tracking                                                             */
/* -------------------------------------------------------------------------- */

typedef struct { int passed; int failed; } results_t;

static void pass(results_t *r, const char *desc)
{
    printf("  [PASS] %s\n", desc);
    r->passed++;
}

static void fail(results_t *r, const char *desc)
{
    printf("  [FAIL] %s\n", desc);
    r->failed++;
}

/* -------------------------------------------------------------------------- */
/* Phase 1 — build the interleaved stream                                     */
/* -------------------------------------------------------------------------- */

static int build_interleaved_stream(symbol_t *out_stream, results_t *r)
{
    interleaver_t *il;
    symbol_t       sym;
    int            b, f, i, rc;
    char           desc[128];

    il = interleaver_create(TEST_DEPTH, TEST_N, TEST_SYMBOL_SIZE);
    if (il == NULL) {
        fail(r, "Phase 1: interleaver_create() returned NULL");
        return -1;
    }

    for (b = 0; b < TEST_DEPTH; ++b) {
        for (f = 0; f < TEST_N; ++f) {
            fill_symbol(&sym, (uint32_t)b, (uint32_t)f);
            rc = interleaver_push_symbol(il, &sym);
            if (rc < 0) {
                snprintf(desc, sizeof(desc),
                         "Phase 1: push_symbol(block=%d fec=%d) failed", b, f);
                fail(r, desc);
                interleaver_destroy(il);
                return -1;
            }
        }
    }

    if (!interleaver_is_ready(il)) {
        fail(r, "Phase 1: interleaver not ready after full push");
        interleaver_destroy(il);
        return -1;
    }

    for (i = 0; i < TEST_TOTAL; ++i) {
        rc = interleaver_pop_ready_symbol(il, &out_stream[i]);
        if (rc < 0) {
            snprintf(desc, sizeof(desc),
                     "Phase 1: pop_ready_symbol() failed at i=%d", i);
            fail(r, desc);
            interleaver_destroy(il);
            return -1;
        }
    }

    pass(r, "Phase 1: interleaved stream built (40 symbols, column-major)");
    interleaver_destroy(il);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Phase 2 — push shuffled stream into the deinterleaver                     */
/* -------------------------------------------------------------------------- */

static int push_to_deinterleaver(deinterleaver_t *dil,
                                  const symbol_t  *stream,
                                  results_t       *r)
{
    int  i, rc;
    char desc[128];

    printf("\n  Shuffled input to deinterleaver:\n");
    printf("  %-6s  %-10s  %-12s\n", "Order", "block_id", "fec_id");
    printf("  %-6s  %-10s  %-12s\n", "-----", "--------", "------");

    for (i = 0; i < TEST_TOTAL; ++i) {
        printf("  %-6d  %-10u  %-12u\n",
               i,
               (unsigned)stream[i].packet_id,
               (unsigned)stream[i].fec_id);

        rc = deinterleaver_push_symbol(dil, &stream[i]);

        if (rc < 0) {
            snprintf(desc, sizeof(desc),
                     "Phase 2: push_symbol() returned -1 at stream[%d] "
                     "(block=%u fec=%u)",
                     i,
                     (unsigned)stream[i].packet_id,
                     (unsigned)stream[i].fec_id);
            fail(r, desc);
            return -1;
        }
    }

    pass(r, "Phase 2: all 40 interleaved symbols accepted by deinterleaver");
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Phase 3 — retrieve and verify reconstructed blocks                        */
/* -------------------------------------------------------------------------- */

static void verify_blocks(deinterleaver_t *dil, results_t *r)
{
    block_t  block;
    int      retrieved = 0;
    int      all_ok    = 1;
    char     desc[128];

    printf("\n  Reconstructed blocks:\n");

    while (deinterleaver_get_ready_block(dil, &block) == 0) {
        int      sym_count_ok = 1;
        int      order_ok     = 1;
        int      payload_ok   = 1;
        int      f;
        uint32_t bid          = (uint32_t)block.block_id;

        printf("\n  Block %u (%d/%d symbols):\n",
               (unsigned)bid, block.symbol_count, TEST_N);

        if (block.symbol_count != TEST_N) {
            snprintf(desc, sizeof(desc),
                     "Block %u: symbol_count=%d expected=%d",
                     (unsigned)bid, block.symbol_count, TEST_N);
            fail(r, desc);
            sym_count_ok = 0;
            all_ok       = 0;
        }

        for (f = 0; f < TEST_N; ++f) {
            const symbol_t *s   = &block.symbols[f];
            uint32_t        fid = (uint32_t)f;
            size_t          j;

            printf("    fec_id=%u  block_id=%u  payload[0]=0x%02X\n",
                   (unsigned)s->fec_id,
                   (unsigned)s->packet_id,
                   s->data[0]);

            if (s->fec_id != fid) {
                snprintf(desc, sizeof(desc),
                         "Block %u symbol[%d]: fec_id=%u expected=%u",
                         (unsigned)bid, f,
                         (unsigned)s->fec_id, (unsigned)fid);
                fail(r, desc);
                order_ok = 0;
                all_ok   = 0;
            }

            if (s->packet_id != bid) {
                snprintf(desc, sizeof(desc),
                         "Block %u symbol[%d]: packet_id=%u expected=%u",
                         (unsigned)bid, f,
                         (unsigned)s->packet_id, (unsigned)bid);
                fail(r, desc);
                all_ok = 0;
            }

            for (j = 0;
                 j < TEST_SYMBOL_SIZE && j < MAX_SYMBOL_DATA_SIZE;
                 ++j)
            {
                unsigned char want = expected_byte(bid, fid, j);
                if (s->data[j] != want) {
                    snprintf(desc, sizeof(desc),
                             "Block %u symbol[%d] byte[%zu]: "
                             "got=0x%02X expected=0x%02X",
                             (unsigned)bid, f, j, s->data[j], want);
                    fail(r, desc);
                    payload_ok = 0;
                    all_ok     = 0;
                    break;
                }
            }

            (void)sym_count_ok;
            (void)order_ok;
            (void)payload_ok;
        }

        /*
         * MANDATORY: acknowledge and recycle the slot.
         * Without this call the slot stays READY_TO_DECODE forever and
         * the while loop above would retrieve the same block repeatedly.
         */
        deinterleaver_mark_result(dil, bid, 1);

        retrieved++;
    }

    snprintf(desc, sizeof(desc),
             "Phase 3: retrieved %d/%d blocks", retrieved, TEST_DEPTH);
    if (retrieved == TEST_DEPTH) {
        pass(r, desc);
    } else {
        fail(r, desc);
        all_ok = 0;
    }

    if (all_ok) {
        pass(r, "Phase 3: all blocks byte-exact and correctly ordered");
    }
}

/* -------------------------------------------------------------------------- */
/* Test: duplicate symbol rejection                                            */
/* -------------------------------------------------------------------------- */

static void test_duplicate_rejection(results_t *r)
{
    deinterleaver_t *dil;
    symbol_t         sym;
    int              rc1, rc2;

    printf("\n--- Test: duplicate symbol rejection ---\n\n");

    dil = deinterleaver_create(TEST_DEPTH, TEST_N, TEST_K,
                               TEST_SYMBOL_SIZE, DIL_STAB_MS, DIL_MAX_AGE_MS);
    if (dil == NULL) {
        fail(r, "Dup test: deinterleaver_create() failed");
        return;
    }

    fill_symbol(&sym, 0U, 3U);

    rc1 = deinterleaver_push_symbol(dil, &sym);   /* first arrival  */
    rc2 = deinterleaver_push_symbol(dil, &sym);   /* exact duplicate */

    if (rc1 >= 0 && rc2 == 0) {
        pass(r, "Duplicate symbol silently dropped (rc=0)");
    } else {
        char desc[128];
        snprintf(desc, sizeof(desc),
                 "Dup test: rc1=%d rc2=%d (expected rc2=0)", rc1, rc2);
        fail(r, desc);
    }

    {
        block_t block;
        int     found = 0;
        int     i;

        /* Force-complete the block so we can retrieve it */
        for (i = 0; i < TEST_N; ++i) {
            if (i == 3) { continue; }
            fill_symbol(&sym, 0U, (uint32_t)i);
            deinterleaver_push_symbol(dil, &sym);
        }

        if (deinterleaver_get_ready_block(dil, &block) == 0) {
            found = 1;

            if (block.symbol_count == TEST_N) {
                pass(r, "Dup test: block symbol_count correct after duplicate");
            } else {
                char desc[128];
                snprintf(desc, sizeof(desc),
                         "Dup test: symbol_count=%d expected=%d",
                         block.symbol_count, TEST_N);
                fail(r, desc);
            }

            /* MANDATORY: recycle the slot */
            deinterleaver_mark_result(dil, (uint32_t)block.block_id, 1);
        }

        if (!found) {
            fail(r, "Dup test: could not retrieve completed block");
        }
    }

    deinterleaver_destroy(dil);
}

/* -------------------------------------------------------------------------- */
/* Test: out-of-range fec_id rejected                                         */
/* -------------------------------------------------------------------------- */

static void test_invalid_fec_id(results_t *r)
{
    deinterleaver_t *dil;
    symbol_t         sym;
    int              rc;

    printf("\n--- Test: out-of-range fec_id rejected ---\n\n");

    dil = deinterleaver_create(TEST_DEPTH, TEST_N, TEST_K,
                               TEST_SYMBOL_SIZE, DIL_STAB_MS, DIL_MAX_AGE_MS);
    if (dil == NULL) {
        fail(r, "Invalid-fec test: deinterleaver_create() failed");
        return;
    }

    fill_symbol(&sym, 0U, 0U);
    sym.fec_id = (uint32_t)TEST_N;   /* one past valid range */

    rc = deinterleaver_push_symbol(dil, &sym);

    if (rc == -1) {
        pass(r, "push_symbol() correctly rejected fec_id == N");
    } else {
        char desc[128];
        snprintf(desc, sizeof(desc),
                 "push_symbol() returned %d for fec_id==N (expected -1)", rc);
        fail(r, desc);
    }

    deinterleaver_destroy(dil);
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                                 */
/* -------------------------------------------------------------------------- */

int main(void)
{
    results_t        r = { 0, 0 };
    symbol_t        *stream;
    deinterleaver_t *dil;

    log_init();

    printf("============================================================\n");
    printf("  Deinterleaver Round-Trip Verification\n");
    printf("  Depth=%d  N=%d  K=%d  Symbol_Size=%d  Total=%d\n",
           TEST_DEPTH, TEST_N, TEST_K, TEST_SYMBOL_SIZE, TEST_TOTAL);
    printf("============================================================\n");

    printf("\n--- Phase 1: Build interleaved stream ---\n\n");

    stream = (symbol_t *)malloc(sizeof(symbol_t) * TEST_TOTAL);
    if (stream == NULL) {
        fprintf(stderr, "malloc failed for stream\n");
        return 1;
    }

    if (build_interleaved_stream(stream, &r) != 0) {
        free(stream);
        return 1;
    }

    printf("\n--- Phase 2: Push shuffled stream into deinterleaver ---\n");

    dil = deinterleaver_create(TEST_DEPTH, TEST_N, TEST_K,
                               TEST_SYMBOL_SIZE, DIL_STAB_MS, DIL_MAX_AGE_MS);
    if (dil == NULL) {
        fail(&r, "deinterleaver_create() returned NULL");
        free(stream);
        return 1;
    }

    if (push_to_deinterleaver(dil, stream, &r) != 0) {
        deinterleaver_destroy(dil);
        free(stream);
        return 1;
    }

    printf("\n--- Phase 3: Retrieve and verify reconstructed blocks ---\n");
    verify_blocks(dil, &r);

    deinterleaver_destroy(dil);
    free(stream);

    test_duplicate_rejection(&r);
    test_invalid_fec_id(&r);

    printf("\n============================================================\n");
    printf("  Results: %d passed, %d failed\n", r.passed, r.failed);
    printf("============================================================\n\n");

    return (r.failed == 0) ? 0 : 1;
}
