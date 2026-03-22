/*
 * include/interleaver.h — Symbol-level matrix interleaver for the FSO Gateway.
 *
 * Theory of operation
 * -------------------
 * Atmospheric FSO links suffer from burst losses: a turbulence event can
 * erase tens of consecutive symbols.  FEC alone cannot recover a burst that
 * is longer than M repair symbols within a single block.  Interleaving
 * spreads the symbols of every block across time so that a physical burst
 * erases at most one symbol per block, converting the burst into independent
 * per-block losses that FEC can handle.
 *
 * Matrix layout  [Depth rows × N columns]
 * ----------------------------------------
 *   Row    → one complete FEC block  (block_id mod Depth)
 *   Column → one symbol position across all blocks  (fec_id 0..N-1)
 *
 *   Write order (enqueue):  row-major  — fill each row left-to-right as
 *                            FEC blocks arrive.
 *   Read  order (dequeue):  column-major — drain column 0 top-to-bottom,
 *                            then column 1, etc.
 *
 * This transforms a burst of B consecutive transmitted symbols into B losses
 * spread across B different blocks, each losing at most one symbol — well
 * within FEC's recovery budget when B ≤ Depth.
 *
 * Memory model
 * ------------
 * The entire Depth×N matrix is one contiguous allocation:
 *
 *   matrix[row * N + col]   (row = block slot, col = fec_id)
 *
 * Column-major traversal therefore strides by N symbols — one cache line
 * per symbol at symbol_size ≥ 64 bytes, which is always true in this
 * project (symbol_size = 1500).  The hot path touches exactly one
 * symbol_t per dequeue call, keeping L1 pressure low.
 *
 * Window management
 * -----------------
 * The interleaver maintains a sliding window of Depth block slots.
 * A slot is "occupied" once its first symbol arrives and "complete" once
 * all N symbols for that slot have been enqueued.  Dequeue is only
 * permitted after every slot in the current window is complete — i.e.
 * the matrix is full.  This ensures the receiver always sees a uniform
 * interleave depth.
 */

#ifndef FSO_INTERLEAVER_H
#define FSO_INTERLEAVER_H

#include <stdint.h>

#include "types.h"   /* symbol_t */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Opaque handle                                                               */
/* -------------------------------------------------------------------------- */

typedef struct interleaver interleaver_t;

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

/*
 * interleaver_create() — allocate and initialise a matrix interleaver.
 *
 *   depth       : number of FEC blocks in the interleave window  (>= 2)
 *   k_plus_m    : total symbols per block  (K + M, >= 1)
 *   symbol_size : payload bytes per symbol  (> 0)
 *
 * The matrix requires  depth * k_plus_m * sizeof(symbol_t)  bytes.
 * For the reference geometry (depth=8, K+M=96, symbol_size=1500) that
 * is 8 × 96 × ~1530 bytes ≈ 1.18 MiB — comfortably fits in L2/L3.
 *
 * Returns a valid pointer on success, NULL on failure.
 * Must be released with interleaver_destroy().
 */
interleaver_t *interleaver_create(int depth, int k_plus_m, int symbol_size);

/*
 * interleaver_destroy() — release all resources.  Safe to call with NULL.
 */
void interleaver_destroy(interleaver_t *il);

/* -------------------------------------------------------------------------- */
/* Enqueue (write path — row-major)                                           */
/* -------------------------------------------------------------------------- */

/*
 * interleaver_push_symbol() — place one symbol into the matrix.
 *
 *   il  : interleaver handle
 *   sym : symbol to enqueue; the following fields must be populated:
 *           sym->fec_id     — column index  (0 .. K+M-1)
 *           sym->packet_id  — used to derive the row (block slot)
 *
 * The row is computed as:  sym->packet_id % depth
 * (callers that track block_id explicitly should store it in packet_id
 * or pass it via a wrapper — see implementation notes in interleaver.c).
 *
 * Returns:
 *    0   symbol accepted; matrix not yet full.
 *    1   symbol accepted; this write completed the matrix — the window
 *        is now ready and interleaver_pop_ready_symbol() will return
 *        symbols.
 *   -1   error (NULL pointer, out-of-range fec_id, slot already full).
 */
int interleaver_push_symbol(interleaver_t *il, const symbol_t *sym);

/* -------------------------------------------------------------------------- */
/* Dequeue (read path — column-major)                                         */
/* -------------------------------------------------------------------------- */

/*
 * interleaver_pop_ready_symbol() — retrieve the next symbol in column-major
 * transmission order.
 *
 *   il      : interleaver handle
 *   out_sym : caller-allocated symbol_t to receive the popped symbol
 *
 * The traversal order is:
 *   (row=0,col=0), (row=1,col=0) … (row=D-1,col=0),
 *   (row=0,col=1), (row=1,col=1) … (row=D-1,col=N-1)
 *
 * After the last symbol (row=D-1, col=N-1) is popped the matrix is reset
 * and the interleaver is ready to accept the next window of Depth blocks.
 *
 * Returns:
 *    0   symbol written to *out_sym; more symbols remain.
 *    1   symbol written to *out_sym; this was the last symbol in the
 *        window — the matrix has been reset.
 *   -1   not ready (matrix not yet full) or error.
 */
int interleaver_pop_ready_symbol(interleaver_t *il, symbol_t *out_sym);

/* -------------------------------------------------------------------------- */
/* Introspection                                                               */
/* -------------------------------------------------------------------------- */

/*
 * interleaver_is_ready() — returns 1 if the matrix is full and pop calls
 * will succeed, 0 otherwise.  Useful for polling in a select/poll loop.
 */
int interleaver_is_ready(const interleaver_t *il);

/*
 * interleaver_symbols_pending() — number of symbols remaining to be popped
 * from the current window.  Returns 0 if the matrix is not yet full.
 */
int interleaver_symbols_pending(const interleaver_t *il);

#ifdef __cplusplus
}
#endif

#endif /* FSO_INTERLEAVER_H */