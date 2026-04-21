/*
 * include/rx_pipeline.h — RX pipeline for the FSO Gateway.
 *
 * Implements the unidirectional receive path:
 *   NIC_FSO receive → deinterleave → FEC decode → reassemble → NIC_LAN transmit
 *
 * Opaque handle: struct rx_pipeline is defined only in rx_pipeline.c.
 */

#ifndef RX_PIPELINE_H
#define RX_PIPELINE_H

#include "arp_cache.h"
#include "config.h"
#include "packet_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Opaque handle                                                               */
/* -------------------------------------------------------------------------- */

typedef struct rx_pipeline rx_pipeline_t;

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

/*
 * rx_pipeline_create() — allocate and initialise the RX pipeline.
 *
 *   cfg    : runtime configuration (copied internally).
 *   rx_ctx : packet_io handle to receive from (FSO NIC).
 *   tx_ctx : packet_io handle to transmit to  (LAN NIC).
 *
 * Internally allocates:
 *   - deinterleaver (max_active_blocks = cfg->depth * 4,
 *                    symbols_per_block = cfg->k + cfg->m,
 *                    k                 = cfg->k,
 *                    symbol_size       = cfg->symbol_size,
 *                    stabilization_ms  = 0.0,
 *                    block_max_age_ms  = 50.0)
 *   - fec_handle    (k = cfg->k, symbol_size = cfg->symbol_size)
 *   - recon_buf     (k * symbol_size bytes, heap-allocated)
 *
 * Returns non-NULL on success, NULL on any allocation failure.
 * Caller retains ownership of rx_ctx and tx_ctx — do NOT close them
 * inside rx_pipeline_destroy().
 */
rx_pipeline_t *rx_pipeline_create(const struct config *cfg,
                                  packet_io_ctx_t     *rx_ctx,
                                  packet_io_ctx_t     *tx_ctx,
                                  arp_cache_t         *arp_cache);

/*
 * rx_pipeline_run_once() — execute one iteration of the RX pipeline.
 *
 * Steps performed (in order):
 *   1. Non-blocking receive from the FSO NIC.
 *   2. Deserialize wire format → symbol_t; validate; CRC check.
 *   3. Push symbol into deinterleaver.
 *   4. Drain any ready blocks: FEC decode → reassemble → send to LAN NIC.
 *
 * Returns:
 *    0  — success (or benign no-op such as no packet available).
 *   -1  — fatal error (should not occur in normal operation).
 */
int rx_pipeline_run_once(rx_pipeline_t *pl);

/*
 * rx_pipeline_destroy() — free all resources owned by the pipeline.
 *
 * Does NOT close rx_ctx or tx_ctx — those are owned by the caller.
 * Safe to call with NULL.
 */
void rx_pipeline_destroy(rx_pipeline_t *pl);

#ifdef __cplusplus
}
#endif

#endif /* RX_PIPELINE_H */
