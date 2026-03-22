/*
 * include/deinterleaver.h — Receiver-side block collector for the FSO Gateway.
 *
 * Theory of operation
 * -------------------
 * The transmitter interleaves symbols column-major across D FEC blocks,
 * so the receiver sees symbols from D different blocks interleaved in the
 * incoming stream.  The deinterleaver collects symbols by block_id and
 * reassembles each block independently.
 *
 * Because interleaving spreads symbols across time, up to D blocks will
 * be partially filled simultaneously — one slot per interleave depth
 * level.  Each slot is a "live block" keyed by block_id.
 *
 * A block becomes ready for FEC decoding when either:
 *   (a) all symbols_per_block symbols have arrived  (hard complete), or
 *   (b) a configurable timeout has elapsed since the first symbol arrived
 *       and at least K symbols are present  (timeout-triggered flush).
 *
 * The caller drains ready blocks via deinterleaver_get_ready_block() and
 * passes each one to fec_decode_block().
 *
 * Out-of-order and duplicate handling
 * ------------------------------------
 * Symbols may arrive out of order due to interleaving.  Each slot tracks
 * a per-position occupancy bitmap so duplicate symbols (retransmissions,
 * network echoes) are silently dropped without corrupting the block.
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
/* Opaque handle                                                               */
/* -------------------------------------------------------------------------- */

typedef struct deinterleaver deinterleaver_t;

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

/*
 * deinterleaver_create() — allocate and initialise a deinterleaver.
 *
 *   max_active_blocks : maximum number of blocks being assembled at once;
 *                       should equal the interleaver depth D  (>= 1)
 *   symbols_per_block : N = K + M  (>= 1)
 *   symbol_size       : informational — used for validation  (> 0)
 *
 * Returns a valid pointer on success, NULL on failure.
 * Must be released with deinterleaver_destroy().
 */
deinterleaver_t *deinterleaver_create(int    max_active_blocks,
                                      int    symbols_per_block,
                                      size_t symbol_size);

/*
 * deinterleaver_destroy() — release all resources.  Safe to call with NULL.
 */
void deinterleaver_destroy(deinterleaver_t *self);

/* -------------------------------------------------------------------------- */
/* Ingestion                                                                   */
/* -------------------------------------------------------------------------- */

/*
 * deinterleaver_push_symbol() — ingest one received symbol.
 *
 * The symbol is placed into the block slot identified by sym->packet_id
 * (which carries the block_id) at position sym->fec_id.
 *
 * If the block_id is not yet tracked a new slot is allocated.  If all
 * slots are occupied and none matches this block_id the oldest slot is
 * evicted (it is marked timed-out and becomes retrievable via
 * deinterleaver_get_ready_block).
 *
 * Duplicate symbols (same block_id + fec_id already received) are
 * silently dropped.
 *
 * Returns:
 *    0   symbol accepted; block not yet complete.
 *    1   symbol accepted; this was the final symbol — block is now complete
 *        and ready for deinterleaver_get_ready_block().
 *   -1   error (NULL pointer, fec_id out of range).
 */
int deinterleaver_push_symbol(deinterleaver_t *self, const symbol_t *sym);

/* -------------------------------------------------------------------------- */
/* Retrieval                                                                   */
/* -------------------------------------------------------------------------- */

/*
 * deinterleaver_get_ready_block() — retrieve one completed or timed-out block.
 *
 *   self      : deinterleaver handle
 *   out_block : caller-allocated block_t to receive the block
 *
 * A block is "ready" when:
 *   - all symbols_per_block symbols have been received, OR
 *   - deinterleaver_mark_timeout() has been called for its slot.
 *
 * The slot is freed after this call so it can be reused.
 *
 * Returns:
 *    0   block written to *out_block; more ready blocks may remain —
 *        keep calling until this returns -1.
 *   -1   no ready block available.
 */
int deinterleaver_get_ready_block(deinterleaver_t *self, block_t *out_block);

/* -------------------------------------------------------------------------- */
/* Timeout management                                                          */
/* -------------------------------------------------------------------------- */

/*
 * deinterleaver_tick() — advance the internal clock and mark any slots
 * that have exceeded timeout_ms as ready-for-flush.
 *
 *   timeout_ms : slots whose first-symbol wall-clock age exceeds this
 *                value are flushed regardless of completeness.
 *
 * Call this periodically from the main poll loop (e.g. on every
 * POLL_TIMEOUT_MS expiry) to ensure partially-filled blocks are not
 * held forever when the stream goes quiet or symbols are unrecoverable.
 *
 * Returns the number of slots newly marked as timed-out (>= 0).
 */
int deinterleaver_tick(deinterleaver_t *self, double timeout_ms);

/* -------------------------------------------------------------------------- */
/* Introspection                                                               */
/* -------------------------------------------------------------------------- */

/*
 * deinterleaver_active_blocks() — number of block slots currently occupied.
 */
int deinterleaver_active_blocks(const deinterleaver_t *self);

/*
 * deinterleaver_ready_count() — number of blocks ready to be retrieved.
 */
int deinterleaver_ready_count(const deinterleaver_t *self);

#ifdef __cplusplus
}
#endif

#endif /* FSO_DEINTERLEAVER_H */