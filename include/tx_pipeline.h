/*
 * include/tx_pipeline.h — TX pipeline for the FSO Gateway.
 *
 * Implements the unidirectional transmit path:
 *   NIC_LAN receive → fragment → block build → FEC encode →
 *   interleave → NIC_FSO transmit
 *
 * Opaque handle: struct tx_pipeline is defined only in tx_pipeline.c.
 */

#ifndef TX_PIPELINE_H
#define TX_PIPELINE_H

#include "config.h"
#include "packet_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Opaque handle                                                               */
/* -------------------------------------------------------------------------- */

typedef struct tx_pipeline tx_pipeline_t;

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

/*
 * tx_pipeline_create() — allocate and initialise the TX pipeline.
 *
 *   cfg    : runtime configuration (copied internally).
 *   rx_ctx : packet_io handle to receive from (LAN NIC).
 *   tx_ctx : packet_io handle to transmit to  (FSO NIC).
 *
 * Internally allocates:
 *   - block_builder  (k = cfg->k)
 *   - fec_handle     (k = cfg->k, symbol_size = cfg->symbol_size)
 *   - interleaver    (depth = cfg->depth, k_plus_m = cfg->k + cfg->m,
 *                     symbol_size = cfg->symbol_size)
 *
 * Returns non-NULL on success, NULL on any allocation failure.
 * Caller retains ownership of rx_ctx and tx_ctx — do NOT close them
 * inside tx_pipeline_destroy().
 */
tx_pipeline_t *tx_pipeline_create(const struct config *cfg,
                                  packet_io_ctx_t     *rx_ctx,
                                  packet_io_ctx_t     *tx_ctx);

/*
 * tx_pipeline_run_once() — execute one iteration of the TX pipeline.
 *
 * Steps performed (in order):
 *   1. Non-blocking receive from the LAN NIC.
 *   2. Fragment the received packet into source symbols.
 *   3. Feed source symbols into the block builder; on block completion
 *      encode (FEC), push all K+M symbols to the interleaver, drain.
 *   4. On idle (no packet): check block-timeout and flush if needed.
 *
 * Returns:
 *    0  — success (or benign no-op such as no packet available).
 *   -1  — fatal error.
 */
int tx_pipeline_run_once(tx_pipeline_t *pl);

/*
 * tx_pipeline_destroy() — free all resources owned by the pipeline.
 *
 * Does NOT close rx_ctx or tx_ctx — those are owned by the caller.
 * Safe to call with NULL.
 */
void tx_pipeline_destroy(tx_pipeline_t *pl);

#ifdef __cplusplus
}
#endif

#endif /* TX_PIPELINE_H */
