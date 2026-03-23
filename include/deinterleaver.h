/*
 * include/deinterleaver.h — Receiver-side block collector: Block Lifecycle FSM.
 *
 * =============================================================================
 * BLOCK LIFECYCLE STATE MACHINE  (Option B — 3 observable states)
 * =============================================================================
 *
 * Every slot is externally observable in exactly ONE of three states:
 *
 *   BLOCK_EMPTY            Slot is free.  Ready to accept a new block_id.
 *
 *   BLOCK_FILLING          At least one symbol has been stored.  The slot is
 *                          accumulating symbols and evaluating exit conditions.
 *
 *   BLOCK_READY_TO_DECODE  Frozen.  The block has been committed for FEC
 *                          decoding.  No further symbols are accepted.  The
 *                          caller MUST call get_ready_block() then
 *                          mark_result() to return the slot to EMPTY.
 *
 * =============================================================================
 * LEGAL EXTERNAL TRANSITIONS
 * =============================================================================
 *
 *   EMPTY → FILLING
 *       Trigger: first valid symbol for an unknown block_id arrives.
 *
 *   FILLING → READY_TO_DECODE
 *       Trigger (full):       all N symbols received.
 *       Trigger (stabilized): valid_symbols >= K  AND  holes <= M  AND
 *                             no new symbol for >= stabilization_ms.
 *       Trigger (timeout-OK): age >= block_max_age_ms  AND
 *                             valid_symbols >= K  AND  holes <= M.
 *
 *   FILLING → EMPTY  (direct — auto-recycled by tick)
 *       Trigger (timeout-ERR): age >= block_max_age_ms  AND
 *                              (valid_symbols < K  OR  holes > M).
 *       Slot is recycled to EMPTY immediately; it is never READY.
 *
 *   READY_TO_DECODE → EMPTY
 *       Trigger: deinterleaver_mark_result() called by the caller.
 *       get_ready_block() does NOT change state.
 *       mark_result() ALWAYS returns the slot to EMPTY, regardless of the
 *       success flag.
 *
 *   NO OTHER TRANSITIONS ARE LEGAL.
 *
 * =============================================================================
 * KEY INVARIANTS
 * =============================================================================
 *
 * Freeze Rule
 *   Once a slot leaves BLOCK_FILLING its received_mask is never written again.
 *   All arriving symbols for that block_id are silently dropped (return 0).
 *
 * holes > M enforcement
 *   M = N - K.  holes = N - valid_symbols.
 *   A block with holes > M is unrecoverable.  When detected at timeout it is
 *   recycled directly to EMPTY without ever becoming READY_TO_DECODE.
 *   Enforced inside maybe_freeze_slot(), not
 *   deferred to the FEC layer.
 *
 * get_ready_block() is a pure read
 *   It copies the block into the caller's buffer and returns.  Slot state is
 *   NOT changed.  Calling it multiple times for the same block is safe and
 *   idempotent.
 *
 * mark_result() always returns slot to EMPTY
 *   Regardless of the success flag, mark_result() recycles the slot back to
 *   BLOCK_EMPTY.  The success flag is informational only; it does not produce
 *   a distinct persistent state.
 *
 * Explicit acknowledgment (mandatory)
 *   mark_result() MUST be called after every successful get_ready_block().
 *   Without it the slot stays in READY_TO_DECODE indefinitely.
 *
 * No guessed parameters
 *   K, stabilization_ms, and block_max_age_ms are all passed explicitly to
 *   deinterleaver_create().  Nothing is derived or approximated.
 *
 * =============================================================================
 * EVICTION POLICY  (when no EMPTY slot is available for a new block_id)
 * =============================================================================
 *
 *   Priority 1: BLOCK_EMPTY slot              — free; no eviction cost.
 *   Priority 2: oldest BLOCK_READY_TO_DECODE slot — recycled silently.
 *   Priority 3: oldest BLOCK_FILLING slot     — data loss; LOG_WARN emitted.
 *   Priority 4: all slots READY_TO_DECODE     — caller is not draining;
 *               symbol dropped, -1 returned.
 *
 * =============================================================================
 * DIAGNOSTIC COUNTERS
 * =============================================================================
 *   dil_stats_t tracks dropped symbols, evictions, and state transitions.
 *   Retrieve with deinterleaver_get_stats().
 */

#ifndef FSO_DEINTERLEAVER_H
#define FSO_DEINTERLEAVER_H

#include <stddef.h>
#include <stdint.h>

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Block lifecycle states  (public FSM — 3 states)                           */
/* -------------------------------------------------------------------------- */

typedef enum {
    BLOCK_EMPTY           = 0,   /* slot free                                 */
    BLOCK_FILLING         = 1,   /* accumulating symbols                      */
    BLOCK_READY_TO_DECODE = 2    /* frozen; awaiting mark_result()            */
} block_state_t;

/* -------------------------------------------------------------------------- */
/* Diagnostic counters                                                         */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint64_t dropped_symbols_duplicate; /* bitmap dup detected — O(1)         */
    uint64_t dropped_symbols_frozen;    /* arrived after slot left FILLING     */
    uint64_t dropped_symbols_erasure;   /* payload_len == 0 sentinel           */
    uint64_t evicted_filling_blocks;    /* FILLING slot force-recycled (warn)  */
    uint64_t evicted_done_blocks;       /* acknowledged slot recycled silently */
    uint64_t blocks_ready;             /* transitions to READY_TO_DECODE      */
    uint64_t blocks_failed_timeout;    /* recycled directly: timeout + valid < K */
    uint64_t blocks_failed_holes;      /* recycled directly: holes > M        */
} dil_stats_t;

/* -------------------------------------------------------------------------- */
/* Opaque handle                                                               */
/* -------------------------------------------------------------------------- */

typedef struct deinterleaver deinterleaver_t;

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

/*
 * deinterleaver_create()
 *
 *   max_active_blocks : maximum concurrent live slots  (>= 1).
 *   symbols_per_block : N = K + M  (1 <= N <= MAX_SYMBOLS_PER_BLOCK).
 *   k                 : source symbols per block — the REAL K from the FEC
 *                       configuration.  1 <= k < N.  Never derived or guessed.
 *   symbol_size       : payload bytes per symbol  (> 0). Validation only.
 *   stabilization_ms  : quiet-period in ms after valid_symbols >= K before
 *                       freezing to READY_TO_DECODE.  0.0 disables this path.
 *   block_max_age_ms  : hard deadline in ms from first symbol arrival.
 *                       0.0 disables automatic age-based transitions
 *                       (tick-driven only).
 *
 * Returns a non-NULL pointer on success, NULL on any failure.
 * Must be released with deinterleaver_destroy().
 */
deinterleaver_t *deinterleaver_create(int    max_active_blocks,
                                      int    symbols_per_block,
                                      int    k,
                                      size_t symbol_size,
                                      double stabilization_ms,
                                      double block_max_age_ms);

/*
 * deinterleaver_destroy() — free all resources.  Safe to call with NULL.
 */
void deinterleaver_destroy(deinterleaver_t *self);

/* -------------------------------------------------------------------------- */
/* Ingestion                                                                   */
/* -------------------------------------------------------------------------- */

/*
 * deinterleaver_push_symbol() — ingest one received symbol.
 *
 * Enforcement (in order):
 *   1. payload_len == 0  → erasure sentinel; silently dropped; return 0.
 *   2. payload_len > symbol_size → hard error; return -1.
 *   3. fec_id >= N → hard error; return -1.
 *   4. total_symbols mismatch → LOG_WARN; non-fatal.
 *   5. slot not in FILLING → silently dropped (Freeze Rule); return 0.
 *   6. duplicate (block_id, fec_id) → silently dropped; return 0.
 *
 * Returns:
 *    0   symbol accepted or silently dropped; block NOT yet READY.
 *    1   symbol accepted; block transitioned to READY_TO_DECODE.
 *   -1   hard error (NULL arg, fec_id >= N, payload_len > symbol_size).
 */
int deinterleaver_push_symbol(deinterleaver_t *self, const symbol_t *sym);

/* -------------------------------------------------------------------------- */
/* Retrieval                                                                   */
/* -------------------------------------------------------------------------- */

/*
 * deinterleaver_get_ready_block() — copy one READY_TO_DECODE block.
 *
 * PURE READ — does NOT change slot state.
 * Slot stays READY_TO_DECODE until mark_result() is called.
 * Calling this multiple times for the same block is safe and idempotent.
 *
 * Returns:
 *    0   block written to *out_block.
 *   -1   no READY_TO_DECODE block available.
 */
int deinterleaver_get_ready_block(deinterleaver_t *self, block_t *out_block);

/* -------------------------------------------------------------------------- */
/* Explicit acknowledgment  (MANDATORY after every get_ready_block)          */
/* -------------------------------------------------------------------------- */

/*
 * deinterleaver_mark_result() — acknowledge FEC outcome and recycle slot.
 *
 *   block_id : block.block_id as returned by get_ready_block().
 *   success  : 1 = FEC decode succeeded; 0 = FEC decode failed.
 *              Either way the slot is returned to BLOCK_EMPTY.
 *              The success flag does NOT produce a distinct persistent state.
 *
 * Transition: READY_TO_DECODE → EMPTY (always, regardless of success flag).
 *
 * Returns:
 *    0   slot found in READY_TO_DECODE, acknowledged, recycled to EMPTY.
 *   -1   no slot with matching block_id in READY_TO_DECODE state.
 */
int deinterleaver_mark_result(deinterleaver_t *self,
                              uint32_t         block_id,
                              int              success);

/* -------------------------------------------------------------------------- */
/* Timeout / stabilization tick                                               */
/* -------------------------------------------------------------------------- */

/*
 * deinterleaver_tick() — drive time-based state transitions.
 *
 * override_timeout_ms semantics:
 *   > 0.0   Use as the hard-age threshold (overrides block_max_age_ms).
 *   == 0.0  Immediate flush: all FILLING slots transition regardless of age
 *           (end-of-stream sentinel used by burst_sim_test).
 *   < 0.0   Use block_max_age_ms stored at create() time.
 *
 * FILLING → READY_TO_DECODE : age >= threshold AND valid_symbols >= K
 *                             AND holes <= M.
 * FILLING → EMPTY           : age >= threshold AND (valid_symbols < K OR
 *                             holes > M).  Slot is recycled immediately; it
 *                             is never READY and cannot be retrieved.
 *
 * Returns count of slots that transitioned this call (>= 0).
 */
int deinterleaver_tick(deinterleaver_t *self, double override_timeout_ms);

/* -------------------------------------------------------------------------- */
/* Introspection                                                               */
/* -------------------------------------------------------------------------- */

/*
 * deinterleaver_active_blocks() — count of slots NOT in BLOCK_EMPTY.
 */
int deinterleaver_active_blocks(const deinterleaver_t *self);

/*
 * deinterleaver_ready_count() — count of slots in BLOCK_READY_TO_DECODE.
 */
int deinterleaver_ready_count(const deinterleaver_t *self);

/*
 * deinterleaver_get_stats() — copy diagnostic counters into *out.
 * Returns 0 on success, -1 if either pointer is NULL.
 */
int deinterleaver_get_stats(const deinterleaver_t *self, dil_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* FSO_DEINTERLEAVER_H */
