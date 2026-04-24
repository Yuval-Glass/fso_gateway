#ifndef CONTROL_SERVER_H
#define CONTROL_SERVER_H

/**
 * @file control_server.h
 * @brief Lightweight UNIX-domain control/telemetry server.
 *
 * Spawns a single background pthread that listens on an AF_UNIX SOCK_STREAM
 * socket. When a client connects, it streams newline-delimited JSON snapshots
 * of the statistics container at a configurable rate. One client at a time;
 * additional connect attempts wait in the listen backlog.
 *
 * Designed to be linked by the gateway daemon (or by a standalone tool such
 * as control_server_demo) without imposing any synchronization on the
 * gateway hot path — all reads go through stats_snapshot() which uses C11
 * atomic loads.
 *
 * Wire format (one JSON object per line, UTF-8):
 * {
 *   "schema": "fso-gw-stats/1",
 *   "ts_ms": <epoch_ms>,
 *   "uptime_sec": <double>,
 *   "config": { "k": ..., "m": ..., "depth": ..., "symbol_size": ...,
 *               "lan_iface": "...", "fso_iface": "..." },
 *   "stats":  { ... full stats_container fields ... }
 * }
 */

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct control_server control_server_t;

struct control_server_options {
    /** Filesystem path for the listen socket. NULL → "/tmp/fso_gw.sock". */
    const char *socket_path;
    /** Snapshot broadcast rate when a client is connected. 0 → 10 Hz. */
    unsigned int tick_hz;
    /** Optional pointer to live config (echoed in every snapshot). May be NULL. */
    const struct config *gateway_cfg;
};

/**
 * @brief Start the control server thread.
 *
 * Creates the listen socket (replacing any stale file at the same path),
 * sets permissions to 0666 so non-root clients can connect, and launches
 * the worker pthread. Returns immediately.
 *
 * @return Opaque handle on success, NULL on failure (errno set; LOG_ERROR emitted).
 */
control_server_t *control_server_start(const struct control_server_options *opts);

/**
 * @brief Signal the worker to stop and join it.
 *
 * Closes the listen socket, drops any active client, and unlinks the socket
 * path. Safe to call with NULL. Blocks until the worker has exited.
 */
void control_server_stop(control_server_t *cs);

#ifdef __cplusplus
}
#endif

#endif /* CONTROL_SERVER_H */
