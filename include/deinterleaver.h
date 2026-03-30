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
 *       Trigger (full):          all N symbols received.
 *       Trigger (early-promote): stabilization_ms == 0.0  AND
 *                                valid_symbols >= K  AND  holes <= M.
 *                                Promotes immediately; no quiet-period wait.
 *       Trigger (stabilized):    stabilization_ms > 0.0  AND
 *                                valid_symbols >= K  AND  holes <= M  AND
 *                                no new symbol for >= stabilization_ms.
 *       Trigger (timeout-OK):    age >= block_max_age_ms  AND
 *                                valid_symbols >= K  AND  holes <= M.
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
 * CRC REJECTION
 * =============================================================================
 *
 * Internal per-symbol CRC rejection is a pre-filter that operates BEFORE
 * push_symbol() is called.  CRC-failed symbols are counted in
 * dil_stats_t::dropped_symbols_crc_fail and are never presented to the
 * deinterleaver.  From the deinterleaver's perspective, they are identical
 * to symbols that were never transmitted — i.e., they become erasures.
 *
 * The CRC pre-filter is exercised in the sim_runner RX pipeline; the
 * deinterleaver itself has no knowledge of CRC processing.
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
 *   Enforced inside maybe_freeze_slot(), not deferred to the FEC layer.
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
 *   The dropped_symbols_crc_fail counter is incremented by callers (e.g.
 *   sim_runner) before calling push_symbol(); the deinterleaver itself never
 *   touches it.  Retrieve the full struct with deinterleaver_get_stats().
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
    BLOCK_EMPTY           = 0,
    BLOCK_FILLING         = 1,
    BLOCK_READY_TO_DECODE = 2
} block_state_t;

/* -------------------------------------------------------------------------- */
/* Source-of-truth final reason reporting                                     */
/* -------------------------------------------------------------------------- */

typedef enum {
    DIL_BLOCK_FINAL_NONE = 0,
    DIL_BLOCK_FINAL_DECODE_SUCCESS,
    DIL_BLOCK_FINAL_DECODE_FAILED,
    DIL_BLOCK_FINAL_DISCARDED_TIMEOUT_BEFORE_DECODE,
    DIL_BLOCK_FINAL_DISCARDED_TOO_MANY_HOLES_BEFORE_DECODE,
    DIL_BLOCK_FINAL_DISCARDED_EVICTED_BEFORE_DECODE,
    DIL_BLOCK_FINAL_DISCARDED_READY_EVICTED_BEFORE_MARK
} deinterleaver_block_final_reason_t;

typedef void (*deinterleaver_block_final_cb_t)(
    uint32_t                                block_id,
    deinterleaver_block_final_reason_t      reason,
    void                                   *user);

/* -------------------------------------------------------------------------- */
/* Eviction trace info  (populated only for eviction-type final reasons)      */
/* -------------------------------------------------------------------------- */

#define DIL_EVICTION_SNAPSHOT_MAX 16

typedef struct {
    uint64_t incoming_block_id;      /* block_id of the incoming symbol that  */
                                     /* triggered the eviction                 */
    uint32_t slot_index;             /* index of the evicted slot              */
    uint32_t valid_symbols;          /* valid_symbols in the evicted slot      */
    uint32_t holes;                  /* N - valid_symbols at eviction time     */
    uint32_t expected_symbols;       /* symbols_per_block (N)                  */
    uint32_t active_blocks;          /* number of non-EMPTY slots at eviction  */
    uint32_t max_active_blocks;      /* self->max_active_blocks                */

    uint32_t snapshot_count;
    uint32_t snapshot_indices[DIL_EVICTION_SNAPSHOT_MAX];
    char     snapshot_states[DIL_EVICTION_SNAPSHOT_MAX];   /* 'E','F','R'     */
    uint64_t snapshot_block_ids[DIL_EVICTION_SNAPSHOT_MAX];
    uint32_t snapshot_valid_symbols[DIL_EVICTION_SNAPSHOT_MAX];
} dil_eviction_info_t;

typedef void (*deinterleaver_eviction_cb_t)(
    uint32_t                    evicted_block_id,
    deinterleaver_block_final_reason_t reason,
    const dil_eviction_info_t  *info,
    void                       *user);

/* -------------------------------------------------------------------------- */
/* Diagnostic counters                                                         */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint64_t dropped_symbols_duplicate;
    uint64_t dropped_symbols_frozen;
    uint64_t dropped_symbols_erasure;
    uint64_t dropped_symbols_crc_fail;   /* CRC-rejected before push_symbol() */
    uint64_t evicted_filling_blocks;
    uint64_t evicted_done_blocks;
    uint64_t blocks_ready;
    uint64_t blocks_failed_timeout;
    uint64_t blocks_failed_holes;
} dil_stats_t;

/* -------------------------------------------------------------------------- */
/* Opaque handle                                                               */
/* -------------------------------------------------------------------------- */

typedef struct deinterleaver deinterleaver_t;

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

deinterleaver_t *deinterleaver_create(int    max_active_blocks,
                                      int    symbols_per_block,
                                      int    k,
                                      size_t symbol_size,
                                      double stabilization_ms,
                                      double block_max_age_ms);

void deinterleaver_destroy(deinterleaver_t *self);

/* -------------------------------------------------------------------------- */
/* Source-of-truth reporting hook                                              */
/* -------------------------------------------------------------------------- */

int deinterleaver_set_block_final_callback(deinterleaver_t                 *self,
                                           deinterleaver_block_final_cb_t   cb,
                                           void                            *user);

int deinterleaver_set_eviction_callback(deinterleaver_t              *self,
                                        deinterleaver_eviction_cb_t   cb,
                                        void                         *user);

/* -------------------------------------------------------------------------- */
/* Ingestion                                                                   */
/* -------------------------------------------------------------------------- */

int deinterleaver_push_symbol(deinterleaver_t *self, const symbol_t *sym);

/* -------------------------------------------------------------------------- */
/* Retrieval                                                                   */
/* -------------------------------------------------------------------------- */

int deinterleaver_get_ready_block(deinterleaver_t *self, block_t *out_block);

/* -------------------------------------------------------------------------- */
/* Explicit acknowledgment  (MANDATORY after every get_ready_block)          */
/* -------------------------------------------------------------------------- */

int deinterleaver_mark_result(deinterleaver_t *self,
                              uint32_t         block_id,
                              int              success);

/* -------------------------------------------------------------------------- */
/* Timeout / stabilization tick                                               */
/* -------------------------------------------------------------------------- */

int deinterleaver_tick(deinterleaver_t *self, double override_timeout_ms);

/* -------------------------------------------------------------------------- */
/* Introspection                                                               */
/* -------------------------------------------------------------------------- */

int deinterleaver_active_blocks(const deinterleaver_t *self);
int deinterleaver_ready_count(const deinterleaver_t *self);
int deinterleaver_get_stats(const deinterleaver_t *self, dil_stats_t *out);

/*
 * deinterleaver_inc_crc_drop() — increment the CRC-fail drop counter.
 *
 * Called by the RX pipeline (e.g. sim_runner) when a symbol is discarded
 * due to CRC failure before push_symbol() is invoked.  This keeps the
 * counter co-located with other per-deinterleaver stats so all drop
 * diagnostics are retrievable via deinterleaver_get_stats().
 */
void deinterleaver_inc_crc_drop(deinterleaver_t *self);

#ifdef __cplusplus
}
#endif

#endif /* FSO_DEINTERLEAVER_H */
