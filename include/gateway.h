/*
 * include/gateway.h — Full-duplex FSO Gateway.
 *
 * Wires the TX and RX pipelines together into a single process,
 * running each direction in its own pthread.
 *
 * Opaque handle: struct gateway is defined only in gateway.c.
 *
 * Typical lifecycle:
 *   gw = gateway_create(&cfg);
 *   // install signal handler that calls gateway_stop(gw)
 *   rc = gateway_run(gw);   // blocks until stopped
 *   gateway_destroy(gw);
 */

#ifndef GATEWAY_H
#define GATEWAY_H

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Opaque handle                                                               */
/* -------------------------------------------------------------------------- */

typedef struct gateway gateway_t;

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

/*
 * gateway_create() — open all four NIC handles and create both pipelines.
 *
 * Opens:
 *   ctx_lan_rx : cfg->lan_iface, promiscuous=1  (TX pipeline source)
 *   ctx_fso_tx : cfg->fso_iface, promiscuous=0  (TX pipeline sink)
 *   ctx_fso_rx : cfg->fso_iface, promiscuous=1  (RX pipeline source)
 *   ctx_lan_tx : cfg->lan_iface, promiscuous=0  (RX pipeline sink)
 *
 * Creates:
 *   tx_pipeline (cfg, ctx_lan_rx, ctx_fso_tx)
 *   rx_pipeline (cfg, ctx_fso_rx, ctx_lan_tx)
 *
 * On any failure: closes all already-opened handles, frees all memory,
 * logs via LOG_ERROR, and returns NULL.
 *
 * Returns non-NULL on success.
 */
gateway_t *gateway_create(const struct config *cfg);

/*
 * gateway_run() — start both pipeline threads and block until they stop.
 *
 * Starts:
 *   TX thread: tight loop over tx_pipeline_run_once()
 *   RX thread: tight loop over rx_pipeline_run_once()
 *
 * Each thread exits if gateway_stop() is called or if its pipeline
 * returns a fatal error (-1).
 *
 * Blocks until both threads have been joined.
 *
 * Returns:
 *    0  — clean stop (gateway_stop() was called, no fatal errors).
 *   -1  — at least one pipeline thread encountered a fatal error.
 */
int gateway_run(gateway_t *gw);

/*
 * gateway_stop() — signal both threads to stop.
 *
 * Thread-safe. Returns immediately without waiting.
 * gateway_run() will return once both threads have joined.
 */
void gateway_stop(gateway_t *gw);

/*
 * gateway_destroy() — tear down pipelines, close handles, free memory.
 *
 * Must be called after gateway_run() has returned.
 * Safe to call with NULL.
 */
void gateway_destroy(gateway_t *gw);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_H */
