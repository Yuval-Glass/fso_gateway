/*
 * tests/receiver_robustness_test.c — Block Lifecycle FSM Robustness Suite.
 *
 * Verifies the Option B (3 observable states) implementation of the
 * deinterleaver and the public FEC error-code API.
 *
 * FSM contract under test
 * -----------------------
 *   EMPTY → FILLING → READY_TO_DECODE → mark_result() → EMPTY
 *   FILLING → EMPTY  (irrecoverable blocks auto-recycled by tick())
 *   No implicit resets.  No guessed parameters.  No ambiguous states.
 *
 * Eviction policy under test (Option B):
 *   1. BLOCK_EMPTY
 *   2. oldest BLOCK_READY_TO_DECODE  (recycled silently)
 *   3. oldest BLOCK_FILLING          (data loss — LOG_WARN)
 *   4. no candidate                  (symbol rejected)
 *
 * Test list
 * ---------
 *   T01  Stabilization quiet period → READY_TO_DECODE.
 *   T02  Duplicate rejection (bitmap O(1)); no symbol_count inflation.
 *   T03  Freeze Rule: late arrival for READY_TO_DECODE block dropped.
 *   T04  Freeze Rule: new (unseen) fec_id rejected after freeze.
 *   T05  holes > M at timeout → irrecoverable, auto-recycled, never READY.
 *   T06  Slot reuse: acknowledged READY slots recycled on pressure.
 *   T07  Hard timeout + valid < K → irrecoverable, auto-recycled to EMPTY.
 *   T08  Hard timeout + valid >= K + holes <= M → READY_TO_DECODE.
 *   T09  fec_decode_block() returns FEC_DECODE_ERR (-1) when valid symbols
 *        are fewer than K (insufficient symbols guard fires before holes check).
 *   T10  Full N-symbol block: last push returns 1; READY immediately.
 *   T11  READY_TO_DECODE is evictable under slot pressure.  A READY slot
 *        in a full pool is evicted to make room for a new block_id, and
 *        mark_result() on the evicted block returns -1 (slot gone).
 *   T12  mark_result() recycles READY → EMPTY; second call returns -1.
 *   T13  Late arrival after freeze: received_mask and symbol_count unchanged.
 *   T14  payload_len == 0 erasure sentinel dropped; no slot allocated.
 *   T15  fec_id >= N rejected with -1.
 *   T16  get_ready_block() is idempotent (slot stays READY until mark).
 *   T17  fec_decode_block() returns FEC_DECODE_OK with exactly K valid
 *        symbols (holes == M — boundary recoverable case).
 *   T18  dil_stats_t accumulates drop counters correctly.
 *   T19  Good symbol passes symbol_verify_crc() and is accepted by
 *        the deinterleaver.
 *   T20  Corrupted symbol fails symbol_verify_crc(); when discarded before
 *        push it behaves as an erasure (slot count unaffected).
 *   T21  End-to-end: CRC detects one corrupted source symbol, discard
 *        converts it to an erasure, FEC recovers with repair symbol,
 *        reconstruction is bit-exact.
 *
 * Build:
 *   gcc -std=c11 -Iinclude -Wall -Wextra -Wpedantic -pthread           \
 *       -g -fsanitize=address,undefined                                  \
 *       -o build/bin/receiver_robustness_test                           \
 *       tests/receiver_robustness_test.c                                \
 *       build/obj/deinterleaver.o                                       \
 *       build/obj/fec_wrapper.o                                         \
 *       build/obj/logging.o                                             \
 *       -lwirehair -pthread
 */

#define _POSIX_C_SOURCE 200112L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <wirehair/wirehair.h>

#include "deinterleaver.h"
#include "fec_wrapper.h"
#include "logging.h"
#include "symbol.h"
#include "types.h"

/* -------------------------------------------------------------------------- */
/* Test geometry                                                               */
/* -------------------------------------------------------------------------- */

#define TEST_K            4      /* source symbols per block                  */
#define TEST_M            2      /* repair symbols  (M = N - K)               */
#define TEST_N            (TEST_K + TEST_M)   /* 6                            */
#define TEST_SYMBOL_SIZE  64     /* bytes per symbol                          */
#define TEST_DEPTH        8      /* slot pool size                            */

/*
 * FEC geometry used in T09 / T17.
 * Wirehair requires k >= 2 and k * symbol_size >= some minimum.
 * Use a larger symbol size here to ensure Wirehair accepts the geometry.
 */
#define FEC_TEST_K           8
#define FEC_TEST_M           4
#define FEC_TEST_N           (FEC_TEST_K + FEC_TEST_M)   /* 12 */
#define FEC_TEST_SYMBOL_SIZE 1500

#define STAB_MS           5.0   /* stabilization quiet period (ms)            */
#define MAX_AGE_MS        200.0 /* hard-timeout threshold (ms)                */

/* -------------------------------------------------------------------------- */
/* Result tracking                                                             */
/* -------------------------------------------------------------------------- */

typedef struct { int passed; int failed; } results_t;

static void pass(results_t *r, const char *d) { printf("  [PASS] %s\n", d); r->passed++; }
static void fail(results_t *r, const char *d) { printf("  [FAIL] %s\n", d); r->failed++; }
static void check(results_t *r, int cond, const char *d)
{
    if (cond) { pass(r, d); } else { fail(r, d); }
}

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

static void make_symbol(symbol_t *sym, uint32_t block_id, uint32_t fec_id,
                        int sym_size, int n)
{
    size_t i;
    memset(sym, 0, sizeof(symbol_t));
    sym->packet_id     = block_id;
    sym->fec_id        = fec_id;
    sym->symbol_index  = (uint16_t)fec_id;
    sym->total_symbols = (uint16_t)n;
    sym->payload_len   = (uint16_t)sym_size;
    for (i = 0; i < (size_t)sym_size && i < MAX_SYMBOL_DATA_SIZE; ++i) {
        sym->data[i] = (unsigned char)(
            (block_id * 7U + fec_id * 13U + (uint32_t)i) & 0xFFU);
    }
}

/* push fec_ids [start, start+count) for block_id */
static int push_range(deinterleaver_t *dil, uint32_t block_id,
                      int fec_start, int count, int sym_size, int n)
{
    symbol_t sym;
    int      rc = 0, i;
    for (i = 0; i < count; ++i) {
        make_symbol(&sym, block_id, (uint32_t)(fec_start + i), sym_size, n);
        rc = deinterleaver_push_symbol(dil, &sym);
        if (rc < 0) { return -99; }
    }
    return rc;
}

static void sleep_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* Convenience: create a standard 6-arg deinterleaver */
static deinterleaver_t *make_dil(void)
{
    return deinterleaver_create(TEST_DEPTH, TEST_N, TEST_K,
                                TEST_SYMBOL_SIZE, STAB_MS, MAX_AGE_MS);
}

/* ========================================================================== */
/* T01 — Stabilization quiet period                                           */
/* ========================================================================== */

static void t01_stabilization(results_t *r)
{
    deinterleaver_t *dil;
    block_t          blk;
    int              rc;

    printf("\n--- T01: Stabilization quiet period ---\n");

    dil = make_dil();
    if (!dil) { fail(r, "T01: create failed"); return; }

    /* K source symbols — K-complete, M holes == M <= M → recoverable */
    rc = push_range(dil, 0U, 0, TEST_K, TEST_SYMBOL_SIZE, TEST_N);
    check(r, rc == 0, "T01: K symbols pushed, not yet READY (rc=0)");
    check(r, deinterleaver_ready_count(dil) == 0,
          "T01: ready_count == 0 immediately");

    /* Wait > STAB_MS then tick */
    sleep_ms((long)(STAB_MS * 2));
    deinterleaver_tick(dil, -1.0);

    check(r, deinterleaver_ready_count(dil) == 1,
          "T01: ready_count == 1 after stabilization");

    rc = deinterleaver_get_ready_block(dil, &blk);
    check(r, rc == 0,              "T01: get_ready_block() succeeded");
    check(r, blk.symbol_count == TEST_K,
          "T01: block.symbol_count == K");

    rc = deinterleaver_mark_result(dil, (uint32_t)blk.block_id, 1);
    check(r, rc == 0,              "T01: mark_result() succeeded");
    check(r, deinterleaver_active_blocks(dil) == 0,
          "T01: slot EMPTY after mark_result");

    deinterleaver_destroy(dil);
}

/* ========================================================================== */
/* T02 — Duplicate rejection                                                  */
/* ========================================================================== */

static void t02_duplicate_rejection(results_t *r)
{
    deinterleaver_t *dil;
    symbol_t         sym;
    block_t          blk;
    int              rc1, rc2;
    dil_stats_t      stats;

    printf("\n--- T02: Duplicate symbol rejection ---\n");

    dil = make_dil();
    if (!dil) { fail(r, "T02: create failed"); return; }

    make_symbol(&sym, 1U, 2U, TEST_SYMBOL_SIZE, TEST_N);
    rc1 = deinterleaver_push_symbol(dil, &sym);
    rc2 = deinterleaver_push_symbol(dil, &sym);

    check(r, rc1 >= 0,  "T02: first push accepted");
    check(r, rc2 == 0,  "T02: duplicate dropped (rc=0)");

    {
        int i;
        for (i = 0; i < TEST_N; ++i) {
            if (i == 2) continue;
            make_symbol(&sym, 1U, (uint32_t)i, TEST_SYMBOL_SIZE, TEST_N);
            deinterleaver_push_symbol(dil, &sym);
        }
    }

    rc1 = deinterleaver_get_ready_block(dil, &blk);
    check(r, rc1 == 0,              "T02: block retrieved");
    check(r, blk.symbol_count == TEST_N,
          "T02: symbol_count == N (no inflation)");

    deinterleaver_get_stats(dil, &stats);
    check(r, stats.dropped_symbols_duplicate >= 1,
          "T02: dropped_symbols_duplicate >= 1");

    deinterleaver_mark_result(dil, (uint32_t)blk.block_id, 1);
    deinterleaver_destroy(dil);
}

/* ========================================================================== */
/* T03 — Freeze Rule: same fec_id dropped post-freeze                        */
/* ========================================================================== */

static void t03_frozen_same_fec(results_t *r)
{
    deinterleaver_t *dil;
    symbol_t         sym;
    block_t          blk;
    int              sc_before, sc_after;
    int              rc;
    dil_stats_t      stats;

    printf("\n--- T03: Freeze Rule — same fec_id dropped post-freeze ---\n");

    dil = make_dil();
    if (!dil) { fail(r, "T03: create failed"); return; }

    push_range(dil, 2U, 0, TEST_N, TEST_SYMBOL_SIZE, TEST_N);
    check(r, deinterleaver_ready_count(dil) == 1, "T03: block READY");

    deinterleaver_get_ready_block(dil, &blk);
    sc_before = blk.symbol_count;

    /* Late arrival for fec_id=0 on frozen block */
    make_symbol(&sym, 2U, 0U, TEST_SYMBOL_SIZE, TEST_N);
    rc = deinterleaver_push_symbol(dil, &sym);
    check(r, rc == 0, "T03: late symbol dropped (rc=0)");

    deinterleaver_get_ready_block(dil, &blk);
    sc_after = blk.symbol_count;
    check(r, sc_after == sc_before, "T03: symbol_count unchanged");

    deinterleaver_get_stats(dil, &stats);
    check(r, stats.dropped_symbols_frozen >= 1,
          "T03: dropped_symbols_frozen >= 1");

    deinterleaver_mark_result(dil, (uint32_t)blk.block_id, 1);
    deinterleaver_destroy(dil);
}

/* ========================================================================== */
/* T04 — Freeze Rule: new unseen fec_id dropped post-freeze                  */
/* ========================================================================== */

static void t04_frozen_new_fec(results_t *r)
{
    deinterleaver_t *dil;
    symbol_t         sym;
    block_t          blk;
    int              sc_before, sc_after;
    int              rc;

    printf("\n--- T04: Freeze Rule — new fec_id dropped after freeze ---\n");

    /* Disable stab and max-age so tick(0.0) is the only flush mechanism */
    dil = deinterleaver_create(TEST_DEPTH, TEST_N, TEST_K,
                               TEST_SYMBOL_SIZE, 0.0, 0.0);
    if (!dil) { fail(r, "T04: create failed"); return; }

    push_range(dil, 3U, 0, TEST_K, TEST_SYMBOL_SIZE, TEST_N);
    deinterleaver_tick(dil, 0.0);   /* → READY (valid=K, holes=M≤M) */

    check(r, deinterleaver_ready_count(dil) == 1, "T04: block READY");

    deinterleaver_get_ready_block(dil, &blk);
    sc_before = blk.symbol_count;

    /* Push a brand-new repair symbol (fec_id=K, not yet received) */
    make_symbol(&sym, 3U, (uint32_t)TEST_K, TEST_SYMBOL_SIZE, TEST_N);
    rc = deinterleaver_push_symbol(dil, &sym);
    check(r, rc == 0, "T04: new fec_id for frozen block dropped (rc=0)");

    deinterleaver_get_ready_block(dil, &blk);
    sc_after = blk.symbol_count;
    check(r, sc_after == sc_before,
          "T04: symbol_count unchanged after new fec_id rejected");

    deinterleaver_mark_result(dil, (uint32_t)blk.block_id, 1);
    deinterleaver_destroy(dil);
}

/* ========================================================================== */
/* T05 — holes > M → irrecoverable, auto-recycled, never READY               */
/* ========================================================================== */

static void t05_holes_gt_m_recycled(results_t *r)
{
    deinterleaver_t *dil;
    dil_stats_t      stats;

    printf("\n--- T05: holes > M → irrecoverable, auto-recycled, never READY ---\n");

    /* K=4 M=2. Push only K-1=3 symbols → holes=3 > M=2 */
    dil = make_dil();
    if (!dil) { fail(r, "T05: create failed"); return; }

    push_range(dil, 4U, 0, TEST_K - 1, TEST_SYMBOL_SIZE, TEST_N);

    check(r, deinterleaver_ready_count(dil) == 0,
          "T05: not READY after K-1 symbols");

    deinterleaver_tick(dil, 0.0);

    check(r, deinterleaver_ready_count(dil) == 0,
          "T05: irrecoverable block not in ready_count");
    check(r, deinterleaver_active_blocks(dil) == 0,
          "T05: slot auto-recycled to EMPTY");

    deinterleaver_get_stats(dil, &stats);
    check(r, (stats.blocks_failed_holes + stats.blocks_failed_timeout) >= 1,
          "T05: failure counter incremented");

    deinterleaver_destroy(dil);
}

/* ========================================================================== */
/* T06 — Slot reuse: acknowledged READY slots recycled under pressure        */
/* ========================================================================== */

static void t06_slot_reuse(results_t *r)
{
    deinterleaver_t *dil;
    block_t          blk;
    int              rc;

    printf("\n--- T06: Slot reuse — READY slots recycled after mark_result ---\n");

    dil = deinterleaver_create(2, TEST_N, TEST_K,
                               TEST_SYMBOL_SIZE, 0.0, 0.0);
    if (!dil) { fail(r, "T06: create failed"); return; }

    /* Fill both slots fully */
    push_range(dil, 10U, 0, TEST_N, TEST_SYMBOL_SIZE, TEST_N);
    push_range(dil, 11U, 0, TEST_N, TEST_SYMBOL_SIZE, TEST_N);
    check(r, deinterleaver_ready_count(dil) == 2, "T06: both READY");

    /* Acknowledge both → EMPTY */
    deinterleaver_get_ready_block(dil, &blk);
    deinterleaver_mark_result(dil, (uint32_t)blk.block_id, 1);
    deinterleaver_get_ready_block(dil, &blk);
    deinterleaver_mark_result(dil, (uint32_t)blk.block_id, 1);

    check(r, deinterleaver_active_blocks(dil) == 0,
          "T06: active_blocks == 0 after both mark_result");

    /*
     * 3rd block must be accepted.  With stab_ms=0.0 the block promotes to
     * READY as soon as valid_symbols >= K and holes <= M.  push_range()
     * returns the rc of the last symbol pushed; symbols arriving after the
     * READY transition are frozen-dropped (rc=0).  Accepting the block is
     * confirmed by rc >= 0 (not rejected with -99 or -1).
     */
    rc = push_range(dil, 12U, 0, TEST_N, TEST_SYMBOL_SIZE, TEST_N);
    check(r, rc >= 0, "T06: 3rd block accepted after slots recycled (rc>=0)");

    deinterleaver_get_ready_block(dil, &blk);
    deinterleaver_mark_result(dil, (uint32_t)blk.block_id, 1);

    deinterleaver_destroy(dil);
}

/* ========================================================================== */
/* T07 — Hard timeout + valid < K → irrecoverable, auto-recycled             */
/* ========================================================================== */

static void t07_timeout_irrecoverable(results_t *r)
{
    deinterleaver_t *dil;
    symbol_t         sym;
    int              rc;

    printf("\n--- T07: Hard timeout + valid < K → irrecoverable, auto-recycled ---\n");

    dil = make_dil();
    if (!dil) { fail(r, "T07: create failed"); return; }

    /* 1 symbol, valid=1 < K=4, holes=5 > M=2 */
    make_symbol(&sym, 20U, 0U, TEST_SYMBOL_SIZE, TEST_N);
    rc = deinterleaver_push_symbol(dil, &sym);
    check(r, rc == 0, "T07: single symbol accepted");

    deinterleaver_tick(dil, 0.0);

    check(r, deinterleaver_ready_count(dil) == 0,
          "T07: irrecoverable block not in ready_count");
    check(r, deinterleaver_active_blocks(dil) == 0,
          "T07: slot auto-recycled to EMPTY");

    deinterleaver_destroy(dil);
}

/* ========================================================================== */
/* T08 — Hard timeout + valid >= K + holes <= M → READY_TO_DECODE           */
/* ========================================================================== */

static void t08_timeout_ready(results_t *r)
{
    deinterleaver_t *dil;
    block_t          blk;
    int              rc;

    printf("\n--- T08: Hard timeout + valid >= K + holes <= M → READY ---\n");

    dil = make_dil();
    if (!dil) { fail(r, "T08: create failed"); return; }

    /* K source symbols: holes == M == 2 ≤ M → recoverable */
    push_range(dil, 21U, 0, TEST_K, TEST_SYMBOL_SIZE, TEST_N);
    check(r, deinterleaver_ready_count(dil) == 0, "T08: not READY yet");

    rc = deinterleaver_tick(dil, 0.0);
    check(r, rc >= 1, "T08: tick transitioned at least 1 slot");
    check(r, deinterleaver_ready_count(dil) == 1, "T08: READY after timeout");

    rc = deinterleaver_get_ready_block(dil, &blk);
    check(r, rc == 0, "T08: get_ready_block succeeded");
    check(r, blk.symbol_count == TEST_K, "T08: symbol_count == K");

    rc = deinterleaver_mark_result(dil, (uint32_t)blk.block_id, 1);
    check(r, rc == 0, "T08: mark_result succeeded");

    deinterleaver_destroy(dil);
}

/* ========================================================================== */
/* T09 — fec_decode_block() returns FEC_DECODE_ERR for insufficient symbols  */
/* ========================================================================== */

static void t09_fec_too_many_holes(results_t *r)
{
    fec_handle_t   fec;
    symbol_t       syms[MAX_SYMBOLS_PER_BLOCK];
    unsigned char *out;
    int            i, rc;

    printf("\n--- T09: fec_decode_block returns FEC_DECODE_ERR for insufficient symbols ---\n");

    fec = fec_create(FEC_TEST_K, FEC_TEST_SYMBOL_SIZE);
    if (!fec) { fail(r, "T09: fec_create failed"); return; }

    out = (unsigned char *)calloc((size_t)FEC_TEST_K * FEC_TEST_SYMBOL_SIZE, 1);
    if (!out) { fail(r, "T09: calloc failed"); fec_destroy(fec); return; }

    memset(syms, 0, sizeof(symbol_t) * FEC_TEST_N);

    /*
     * All symbols missing: valid_received = 0 < K.
     * fec_decode_block() checks valid_received < ctx->k first and returns
     * FEC_DECODE_ERR (-1) — before the holes > M check is reached.
     */
    rc = fec_decode_block(fec, syms, 0, FEC_TEST_N, out);
    check(r, rc == FEC_DECODE_ERR,
          "T09: returns FEC_DECODE_ERR when all symbols missing (valid < K)");

    /*
     * K-1 valid source symbols: valid_received = K-1 < K.
     * fec_decode_block() returns FEC_DECODE_ERR because the
     * insufficient-symbols guard fires before the holes > M check.
     */
    for (i = 0; i < FEC_TEST_K - 1; ++i) {
        syms[i].fec_id      = (uint32_t)i;
        syms[i].payload_len = (uint16_t)FEC_TEST_SYMBOL_SIZE;
    }
    memset(out, 0, (size_t)FEC_TEST_K * FEC_TEST_SYMBOL_SIZE);
    rc = fec_decode_block(fec, syms, FEC_TEST_K - 1, FEC_TEST_N, out);
    check(r, rc == FEC_DECODE_ERR,
          "T09: returns FEC_DECODE_ERR for K-1 symbols (valid < K)");

    free(out);
    fec_destroy(fec);
}

/* ========================================================================== */
/* T10 — Full N-symbol block: last push returns 1                            */
/* ========================================================================== */

static void t10_full_block(results_t *r)
{
    deinterleaver_t *dil;
    block_t          blk;
    int              rc;

    printf("\n--- T10: Full N-symbol block transitions correctly ---\n");

    dil = make_dil();
    if (!dil) { fail(r, "T10: create failed"); return; }

    rc = push_range(dil, 30U, 0, TEST_N, TEST_SYMBOL_SIZE, TEST_N);
    check(r, rc == 1,   "T10: last push returns 1 (READY)");
    check(r, deinterleaver_ready_count(dil) == 1, "T10: ready_count == 1");

    rc = deinterleaver_get_ready_block(dil, &blk);
    check(r, rc == 0,                    "T10: get_ready_block succeeded");
    check(r, blk.symbol_count == TEST_N, "T10: symbol_count == N");
    check(r, (uint32_t)blk.block_id == 30U, "T10: block_id == 30");

    rc = deinterleaver_mark_result(dil, (uint32_t)blk.block_id, 1);
    check(r, rc == 0, "T10: mark_result succeeded");
    check(r, deinterleaver_active_blocks(dil) == 0,
          "T10: active_blocks == 0 after mark_result");

    deinterleaver_destroy(dil);
}

/* ========================================================================== */
/* T11 — READY_TO_DECODE is evictable under slot pressure                    */
/*                                                                            */
/* Option B eviction priority: EMPTY → READY → FILLING → reject.            */
/* A READY_TO_DECODE slot IS evictable when no EMPTY slot exists.            */
/* Test: 1-slot pool, push block A (→ READY), then push block B.            */
/*       Block A's READY slot must be evicted to make room for B.            */
/*       mark_result(A) must then return -1 (slot is gone).                  */
/* ========================================================================== */

static void t11_ready_evictable_under_pressure(results_t *r)
{
    deinterleaver_t *dil;
    block_t          blk_a;
    dil_stats_t      stats_before, stats_after;
    int              rc;

    printf("\n--- T11: READY_TO_DECODE evictable under slot pressure ---\n");

    /* Single-slot pool, no timing paths */
    dil = deinterleaver_create(1, TEST_N, TEST_K,
                               TEST_SYMBOL_SIZE, 0.0, 0.0);
    if (!dil) { fail(r, "T11: create failed"); return; }

    /* Fill block A → READY */
    push_range(dil, 40U, 0, TEST_N, TEST_SYMBOL_SIZE, TEST_N);
    check(r, deinterleaver_ready_count(dil) == 1,
          "T11: block 40 READY");

    /* Retrieve block A (does not change state) */
    rc = deinterleaver_get_ready_block(dil, &blk_a);
    check(r, rc == 0, "T11: block 40 retrieved");
    check(r, (uint32_t)blk_a.block_id == 40U, "T11: block_id == 40");

    /* Record stats before pressure push */
    deinterleaver_get_stats(dil, &stats_before);

    /*
     * Push first symbol of block B.
     * The only slot is READY_TO_DECODE (priority 2 eviction candidate).
     * The implementation must evict it to allocate block B.
     */
    rc = push_range(dil, 41U, 0, 1, TEST_SYMBOL_SIZE, TEST_N);
    check(r, rc >= 0,
          "T11: block 41 first symbol accepted (READY slot evicted)");

    deinterleaver_get_stats(dil, &stats_after);
    check(r, stats_after.evicted_done_blocks > stats_before.evicted_done_blocks,
          "T11: evicted_done_blocks incremented (READY slot was recycled)");

    /*
     * Block A's slot has been evicted and recycled.
     * mark_result(40) must return -1 because block 40 no longer exists.
     */
    rc = deinterleaver_mark_result(dil, 40U, 1);
    check(r, rc == -1,
          "T11: mark_result(40) returns -1 (slot was evicted)");

    /* Clean up block B */
    deinterleaver_tick(dil, 0.0);
    if (deinterleaver_ready_count(dil) > 0) {
        block_t blk_b;
        deinterleaver_get_ready_block(dil, &blk_b);
        deinterleaver_mark_result(dil, (uint32_t)blk_b.block_id, 1);
    }

    deinterleaver_destroy(dil);
}

/* ========================================================================== */
/* T12 — mark_result() recycles READY → EMPTY; second call returns -1       */
/* ========================================================================== */

static void t12_mark_result_recycles(results_t *r)
{
    deinterleaver_t *dil;
    block_t          blk;
    int              rc;

    printf("\n--- T12: mark_result() recycles READY → EMPTY ---\n");

    dil = make_dil();
    if (!dil) { fail(r, "T12: create failed"); return; }

    push_range(dil, 50U, 0, TEST_N, TEST_SYMBOL_SIZE, TEST_N);
    deinterleaver_get_ready_block(dil, &blk);

    rc = deinterleaver_mark_result(dil, 50U, 1);
    check(r, rc == 0,  "T12: mark_result returned 0");
    check(r, deinterleaver_active_blocks(dil) == 0,
          "T12: active_blocks == 0 after mark_result");
    check(r, deinterleaver_ready_count(dil) == 0,
          "T12: ready_count == 0 after mark_result");

    /* Second call — slot is now EMPTY */
    rc = deinterleaver_mark_result(dil, 50U, 1);
    check(r, rc == -1,
          "T12: second mark_result for same block_id returns -1");

    deinterleaver_destroy(dil);
}

/* ========================================================================== */
/* T13 — Late arrival after freeze: mask unchanged                           */
/* ========================================================================== */

static void t13_late_arrival_mask(results_t *r)
{
    deinterleaver_t *dil;
    symbol_t         sym;
    block_t          blk1, blk2;

    printf("\n--- T13: Late arrival after freeze — mask unchanged ---\n");

    dil = make_dil();
    if (!dil) { fail(r, "T13: create failed"); return; }

    push_range(dil, 60U, 0, TEST_K, TEST_SYMBOL_SIZE, TEST_N);
    deinterleaver_tick(dil, 0.0);

    deinterleaver_get_ready_block(dil, &blk1);

    /* Late repair symbol (fec_id=K, unseen) */
    make_symbol(&sym, 60U, (uint32_t)TEST_K, TEST_SYMBOL_SIZE, TEST_N);
    deinterleaver_push_symbol(dil, &sym);

    deinterleaver_get_ready_block(dil, &blk2);

    check(r, blk1.symbol_count == blk2.symbol_count,
          "T13: symbol_count unchanged after late arrival");
    check(r, blk2.symbol_count == TEST_K,
          "T13: block has exactly K symbols");

    deinterleaver_mark_result(dil, 60U, 1);
    deinterleaver_destroy(dil);
}

/* ========================================================================== */
/* T14 — payload_len == 0 erasure sentinel                                   */
/* ========================================================================== */

static void t14_erasure_sentinel(results_t *r)
{
    deinterleaver_t *dil;
    symbol_t         sym;
    int              rc;
    dil_stats_t      stats;

    printf("\n--- T14: payload_len == 0 erasure sentinel rejected ---\n");

    dil = make_dil();
    if (!dil) { fail(r, "T14: create failed"); return; }

    make_symbol(&sym, 70U, 0U, TEST_SYMBOL_SIZE, TEST_N);
    sym.payload_len = 0;

    rc = deinterleaver_push_symbol(dil, &sym);
    check(r, rc == 0, "T14: erased symbol dropped (rc=0)");
    check(r, deinterleaver_active_blocks(dil) == 0,
          "T14: no slot allocated for erased symbol");

    deinterleaver_get_stats(dil, &stats);
    check(r, stats.dropped_symbols_erasure >= 1,
          "T14: dropped_symbols_erasure incremented");

    deinterleaver_destroy(dil);
}

/* ========================================================================== */
/* T15 — fec_id >= N rejected with -1                                        */
/* ========================================================================== */

static void t15_fec_id_range(results_t *r)
{
    deinterleaver_t *dil;
    symbol_t         sym;
    int              rc;

    printf("\n--- T15: fec_id >= N rejected with -1 ---\n");

    dil = make_dil();
    if (!dil) { fail(r, "T15: create failed"); return; }

    make_symbol(&sym, 80U, (uint32_t)TEST_N, TEST_SYMBOL_SIZE, TEST_N);
    rc = deinterleaver_push_symbol(dil, &sym);
    check(r, rc == -1, "T15: fec_id == N rejected with -1");

    make_symbol(&sym, 80U, 0xFFFFU, TEST_SYMBOL_SIZE, TEST_N);
    rc = deinterleaver_push_symbol(dil, &sym);
    check(r, rc == -1, "T15: fec_id >> N rejected with -1");

    deinterleaver_destroy(dil);
}

/* ========================================================================== */
/* T16 — get_ready_block() is idempotent (slot stays READY until mark)      */
/* ========================================================================== */

static void t16_idempotent_get(results_t *r)
{
    deinterleaver_t *dil;
    block_t          blk1, blk2;
    int              rc;

    printf("\n--- T16: get_ready_block() is idempotent ---\n");

    dil = make_dil();
    if (!dil) { fail(r, "T16: create failed"); return; }

    push_range(dil, 90U, 0, TEST_N, TEST_SYMBOL_SIZE, TEST_N);

    rc = deinterleaver_get_ready_block(dil, &blk1);
    check(r, rc == 0, "T16: first get_ready_block succeeded");
    check(r, deinterleaver_active_blocks(dil) == 1,
          "T16: slot still active (READY_TO_DECODE, not EMPTY)");

    /* Second call without mark_result — must return the same block */
    rc = deinterleaver_get_ready_block(dil, &blk2);
    check(r, rc == 0, "T16: second get_ready_block succeeded (idempotent)");
    check(r, blk2.block_id == blk1.block_id,
          "T16: same block_id returned on second call");

    /* success=0 — still recycles to EMPTY */
    rc = deinterleaver_mark_result(dil, (uint32_t)blk1.block_id, 0);
    check(r, rc == 0, "T16: mark_result(success=0) returned 0");
    check(r, deinterleaver_active_blocks(dil) == 0,
          "T16: slot recycled to EMPTY after mark_result");

    deinterleaver_destroy(dil);
}

/* ========================================================================== */
/* T17 — fec_decode_block() returns FEC_DECODE_OK with holes == M           */
/*                                                                            */
/* Uses FEC_TEST_K / FEC_TEST_SYMBOL_SIZE so Wirehair accepts the geometry.  */
/* wirehair_init() is called in main() before any FEC test.                  */
/* ========================================================================== */

static void t17_fec_boundary_ok(results_t *r)
{
    fec_handle_t    fec;
    symbol_t       *syms;
    unsigned char  *out;
    int             i, rc;

    printf("\n--- T17: fec_decode_block returns FEC_DECODE_OK for holes==M ---\n");

    fec = fec_create(FEC_TEST_K, FEC_TEST_SYMBOL_SIZE);
    if (!fec) { fail(r, "T17: fec_create failed"); return; }

    syms = (symbol_t *)calloc(FEC_TEST_N, sizeof(symbol_t));
    if (!syms) {
        fail(r, "T17: calloc(syms) failed");
        fec_destroy(fec);
        return;
    }

    out = (unsigned char *)calloc((size_t)FEC_TEST_K * FEC_TEST_SYMBOL_SIZE, 1);
    if (!out) {
        fail(r, "T17: calloc(out) failed");
        free(syms);
        fec_destroy(fec);
        return;
    }

    /*
     * Provide exactly K valid source symbols.
     * holes == FEC_TEST_M == M — the boundary recoverable case.
     * data is all-zero which is a valid (degenerate) source block for Wirehair.
     */
    for (i = 0; i < FEC_TEST_K; ++i) {
        syms[i].fec_id      = (uint32_t)i;
        syms[i].payload_len = (uint16_t)FEC_TEST_SYMBOL_SIZE;
        /* data already zeroed by calloc */
    }

    rc = fec_decode_block(fec, syms, FEC_TEST_K, FEC_TEST_N, out);
    check(r, rc == FEC_DECODE_OK,
          "T17: FEC_DECODE_OK for K source symbols (holes == M)");

    free(out);
    free(syms);
    fec_destroy(fec);
}

/* ========================================================================== */
/* T18 — dil_stats_t accumulation                                            */
/* ========================================================================== */

static void t18_stats(results_t *r)
{
    deinterleaver_t *dil;
    symbol_t         sym;
    block_t          blk;
    dil_stats_t      stats;

    printf("\n--- T18: dil_stats_t accumulation ---\n");

    dil = make_dil();
    if (!dil) { fail(r, "T18: create failed"); return; }

    /* Erasure */
    make_symbol(&sym, 100U, 0U, TEST_SYMBOL_SIZE, TEST_N);
    sym.payload_len = 0;
    deinterleaver_push_symbol(dil, &sym);

    /* Duplicate */
    make_symbol(&sym, 100U, 0U, TEST_SYMBOL_SIZE, TEST_N);
    deinterleaver_push_symbol(dil, &sym);
    deinterleaver_push_symbol(dil, &sym);   /* dup */

    /* Complete block 100 */
    {
        int i;
        for (i = 1; i < TEST_N; ++i) {
            make_symbol(&sym, 100U, (uint32_t)i, TEST_SYMBOL_SIZE, TEST_N);
            deinterleaver_push_symbol(dil, &sym);
        }
    }

    /* Frozen drop */
    make_symbol(&sym, 100U, 0U, TEST_SYMBOL_SIZE, TEST_N);
    deinterleaver_push_symbol(dil, &sym);

    deinterleaver_get_ready_block(dil, &blk);
    deinterleaver_mark_result(dil, (uint32_t)blk.block_id, 1);

    deinterleaver_get_stats(dil, &stats);

    check(r, stats.dropped_symbols_erasure   >= 1, "T18: erasure counter >= 1");
    check(r, stats.dropped_symbols_duplicate >= 1, "T18: duplicate counter >= 1");
    check(r, stats.dropped_symbols_frozen    >= 1, "T18: frozen counter >= 1");
    check(r, stats.blocks_ready             >= 1, "T18: blocks_ready >= 1");

    printf("  Stats: dup=%llu frozen=%llu erasure=%llu "
           "evict_fill=%llu evict_done=%llu "
           "ready=%llu fail_timeout=%llu fail_holes=%llu\n",
           (unsigned long long)stats.dropped_symbols_duplicate,
           (unsigned long long)stats.dropped_symbols_frozen,
           (unsigned long long)stats.dropped_symbols_erasure,
           (unsigned long long)stats.evicted_filling_blocks,
           (unsigned long long)stats.evicted_done_blocks,
           (unsigned long long)stats.blocks_ready,
           (unsigned long long)stats.blocks_failed_timeout,
           (unsigned long long)stats.blocks_failed_holes);

    deinterleaver_destroy(dil);
}

/* ========================================================================== */
/* T19 — Good symbol passes CRC verification                                  */
/* ========================================================================== */

static void t19_crc_good_symbol_passes(results_t *r)
{
    deinterleaver_t *dil;
    symbol_t         sym;
    int              rc;

    printf("\n--- T19: good symbol passes CRC ---\n");

    dil = make_dil();
    if (!dil) { fail(r, "T19: create failed"); return; }

    make_symbol(&sym, 200U, 0U, TEST_SYMBOL_SIZE, TEST_N);
    symbol_compute_crc(&sym);

    check(r, symbol_verify_crc(&sym) == 1,
          "T19: symbol_verify_crc returns 1 for correctly stamped symbol");

    rc = deinterleaver_push_symbol(dil, &sym);
    check(r, rc >= 0,
          "T19: CRC-stamped symbol accepted by deinterleaver");

    deinterleaver_destroy(dil);
}

/* ========================================================================== */
/* T20 — Corrupted symbol is rejected by CRC                                  */
/* ========================================================================== */

static void t20_crc_corrupted_symbol_rejected(results_t *r)
{
    symbol_t sym;
    symbol_t sym_meta;

    printf("\n--- T20: corrupted symbol rejected by CRC ---\n");

    /* (a) Payload byte flip */
    make_symbol(&sym, 201U, 0U, TEST_SYMBOL_SIZE, TEST_N);
    symbol_compute_crc(&sym);
    sym.data[0] ^= 0xFFU;

    check(r, symbol_verify_crc(&sym) == 0,
          "T20a: symbol_verify_crc returns 0 after payload corruption");

    /* (b) Metadata (packet_id) flip */
    make_symbol(&sym_meta, 202U, 1U, TEST_SYMBOL_SIZE, TEST_N);
    symbol_compute_crc(&sym_meta);
    sym_meta.packet_id ^= 0x1U;

    check(r, symbol_verify_crc(&sym_meta) == 0,
          "T20b: symbol_verify_crc returns 0 after metadata corruption");

    /* (c) CRC discard behaves as erasure */
    {
        deinterleaver_t *dil;
        block_t          blk;
        symbol_t         good;
        int              i;
        int              rc_ready;

        dil = make_dil();
        if (!dil) { fail(r, "T20c: create failed"); return; }

        make_symbol(&good, 203U, 0U, TEST_SYMBOL_SIZE, TEST_N);
        symbol_compute_crc(&good);
        deinterleaver_push_symbol(dil, &good);

        make_symbol(&sym, 203U, 1U, TEST_SYMBOL_SIZE, TEST_N);
        symbol_compute_crc(&sym);
        sym.data[5] ^= 0xA5U;
        check(r, symbol_verify_crc(&sym) == 0,
              "T20c: CRC detects corruption before push");

        for (i = 2; i < TEST_N; ++i) {
            make_symbol(&good, 203U, (uint32_t)i, TEST_SYMBOL_SIZE, TEST_N);
            symbol_compute_crc(&good);
            deinterleaver_push_symbol(dil, &good);
        }

        sleep_ms((long)(STAB_MS + 5.0));
        deinterleaver_tick(dil, -1.0);

        rc_ready = deinterleaver_get_ready_block(dil, &blk);
        check(r, rc_ready == 0,
              "T20c: block reaches READY despite one CRC-dropped symbol (erasure)");

        if (rc_ready == 0) {
            check(r, blk.symbol_count == TEST_N - 1,
                  "T20c: block has N-1 valid symbols (one erasure from CRC drop)");
            deinterleaver_mark_result(dil, (uint32_t)blk.block_id, 1);
        }

        deinterleaver_destroy(dil);
    }
}

/* ========================================================================== */
/* T21 — FEC recovery when corruption is converted to erasure via CRC        */
/* ========================================================================== */

static void t21_crc_erasure_fec_recovery(results_t *r)
{
    enum {
        T21_K  = FEC_TEST_K,
        T21_M  = FEC_TEST_M,
        T21_N  = FEC_TEST_N,
        T21_SZ = FEC_TEST_SYMBOL_SIZE
    };

    /* fec_set_current_decode_block_id is an internal function in
     * fec_wrapper.c not exposed via fec_wrapper.h; the same
     * extern-at-call-site pattern is used in sim_runner.c.            */
    extern void fec_set_current_decode_block_id(uint64_t block_id);

    fec_handle_t   fec        = NULL;
    unsigned char *source     = NULL;
    unsigned char *recon      = NULL;
    symbol_t      *repairs    = NULL;
    symbol_t       received[T21_N];
    int            i;
    int            ok = 0;

    printf("\n--- T21: CRC erasure → FEC recovery ---\n");

    fec = fec_create(T21_K, T21_SZ);
    if (!fec) { fail(r, "T21: fec_create failed"); return; }

    source  = (unsigned char *)calloc((size_t)T21_K * T21_SZ, 1U);
    recon   = (unsigned char *)calloc((size_t)T21_K * T21_SZ, 1U);
    repairs = (symbol_t *)calloc((size_t)T21_M, sizeof(symbol_t));
    if (!source || !recon || !repairs) {
        fail(r, "T21: alloc failed");
        goto cleanup;
    }

    for (i = 0; i < T21_K * T21_SZ; ++i) {
        source[i] = (unsigned char)((i * 17 + 5) & 0xFFU);
    }

    if (fec_encode_block(fec, source, repairs, T21_M) != FEC_DECODE_OK) {
        fail(r, "T21: fec_encode_block failed");
        goto cleanup;
    }

    memset(received, 0, sizeof(received));

    for (i = 0; i < T21_K; ++i) {
        symbol_t *s = &received[i];
        s->packet_id     = 0U;
        s->fec_id        = (uint32_t)i;
        s->symbol_index  = (uint16_t)i;
        s->total_symbols = (uint16_t)T21_N;
        s->payload_len   = (uint16_t)T21_SZ;
        memcpy(s->data, source + (size_t)i * T21_SZ, (size_t)T21_SZ);
        symbol_compute_crc(s);
    }

    for (i = 0; i < T21_M; ++i) {
        symbol_t *s = &repairs[i];
        s->packet_id     = 0U;
        s->total_symbols = (uint16_t)T21_N;
        symbol_compute_crc(s);
        received[T21_K + i] = *s;
    }

    received[0].data[0] ^= 0xFFU;

    check(r, symbol_verify_crc(&received[0]) == 0,
          "T21: corrupted source symbol fails CRC");

    memset(&received[0], 0, sizeof(symbol_t));

    ok = 1;
    for (i = 1; i < T21_K; ++i) {
        if (!symbol_verify_crc(&received[i])) { ok = 0; break; }
    }
    check(r, ok == 1, "T21: all non-corrupted source symbols pass CRC");

    ok = 1;
    for (i = 0; i < T21_M; ++i) {
        if (!symbol_verify_crc(&received[T21_K + i])) { ok = 0; break; }
    }
    check(r, ok == 1, "T21: all repair symbols pass CRC");

    fec_set_current_decode_block_id(21U);
    {
        int rc = fec_decode_block(fec,
                                  received,
                                  T21_N - 1,
                                  T21_N,
                                  recon);
        check(r, rc == FEC_DECODE_OK,
              "T21: FEC decodes successfully after CRC-erasure");

        if (rc == FEC_DECODE_OK) {
            check(r, memcmp(source, recon, (size_t)T21_K * T21_SZ) == 0,
                  "T21: reconstructed data is bit-exact");
        }
    }

cleanup:
    free(repairs);
    free(recon);
    free(source);
    fec_destroy(fec);
}

/* ========================================================================== */
/* Entry point                                                                 */
/* ========================================================================== */

int main(void)
{
    results_t r = { 0, 0 };

    log_init();

    if (wirehair_init() != Wirehair_Success) {
        fprintf(stderr, "[FATAL] wirehair_init() failed — aborting\n");
        return 1;
    }

    printf("============================================================\n");
    printf("  Receiver Robustness Test Suite (Option B — 3 states)\n");
    printf("  Deinterleaver: K=%d  M=%d  N=%d  SymSize=%d  Depth=%d\n",
           TEST_K, TEST_M, TEST_N, TEST_SYMBOL_SIZE, TEST_DEPTH);
    printf("  FEC tests:     K=%d  M=%d  N=%d  SymSize=%d\n",
           FEC_TEST_K, FEC_TEST_M, FEC_TEST_N, FEC_TEST_SYMBOL_SIZE);
    printf("  stab_ms=%.1f  max_age_ms=%.1f\n", STAB_MS, MAX_AGE_MS);
    printf("  FSM: EMPTY -> FILLING -> READY_TO_DECODE -> EMPTY\n");
    printf("============================================================\n");

    t01_stabilization(&r);
    t02_duplicate_rejection(&r);
    t03_frozen_same_fec(&r);
    t04_frozen_new_fec(&r);
    t05_holes_gt_m_recycled(&r);
    t06_slot_reuse(&r);
    t07_timeout_irrecoverable(&r);
    t08_timeout_ready(&r);
    t09_fec_too_many_holes(&r);
    t10_full_block(&r);
    t11_ready_evictable_under_pressure(&r);
    t12_mark_result_recycles(&r);
    t13_late_arrival_mask(&r);
    t14_erasure_sentinel(&r);
    t15_fec_id_range(&r);
    t16_idempotent_get(&r);
    t17_fec_boundary_ok(&r);
    t18_stats(&r);
    t19_crc_good_symbol_passes(&r);
    t20_crc_corrupted_symbol_rejected(&r);
    t21_crc_erasure_fec_recovery(&r);

    printf("\n============================================================\n");
    printf("  Results: %d passed, %d failed\n", r.passed, r.failed);
    printf("============================================================\n\n");

    return (r.failed == 0) ? 0 : 1;
}