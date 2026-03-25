/* tests/sim_runner.h — Reusable FSO pipeline engine for campaign testing.
 *
 * Extracted from end_to_end_sim_test.c (Task 20).
 * Drives the full real TX→channel→RX pipeline and returns only real measured
 * metrics from the existing implementation:
 *
 *   packet → fragmentation → symbolization → block builder → FEC encode
 *          → interleaving → channel impairments → deinterleaving
 *          → FEC decode → packet reassembly
 */

#ifndef SIM_RUNNER_H
#define SIM_RUNNER_H


#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Channel event types
 * =========================================================================*/

typedef enum {
    CHANNEL_EVENT_ERASURE    = 0, /* zero out symbol: payload_len = 0         */
    CHANNEL_EVENT_CORRUPTION = 1  /* XOR payload bytes, keep payload_len > 0  */
} channel_event_type_t;

typedef struct {
    channel_event_type_t type;
    int                  start_symbol;    /* index into TX symbol stream       */
    int                  length_symbols;  /* number of symbols affected        */
} channel_event_t;

/* =========================================================================
 * Simulation configuration
 * =========================================================================*/

typedef struct {
    int      k;            /* source symbols per FEC block                  */
    int      m;            /* repair symbols per FEC block                  */
    int      depth;        /* interleaver depth (blocks per window)         */
    int      symbol_size;  /* bytes per symbol                              */
    int      num_windows;  /* number of complete interleave windows         */
    uint32_t seed;         /* deterministic packet-generation seed          */
} sim_config_t;

/* =========================================================================
 * Run request
 * =========================================================================*/

#define SIM_MAX_EVENTS 32

typedef struct {
    channel_event_t events[SIM_MAX_EVENTS];
    int             num_events;
    uint32_t        seed_override; /* 0 = use cfg->seed                      */
} sim_run_request_t;

/* =========================================================================
 * Channel apply result
 * =========================================================================*/

typedef struct {
    uint64_t total_tx_symbols;
    uint64_t lost_symbols;
    uint64_t corrupted_symbols;
} channel_apply_result_t;

/* =========================================================================
 * Simulation result — all values are produced by real pipeline execution
 * =========================================================================*/

typedef struct {
    /* Packet-level */
    uint64_t generated_packets;
    uint64_t transmitted_packets;
    uint64_t recovered_packets;
    uint64_t exact_match_packets;
    uint64_t corrupted_packets;
    uint64_t missing_packets;

    /* Symbol-level */
    uint64_t total_tx_symbols;
    uint64_t lost_symbols;
    uint64_t corrupted_symbols;

    /* Block-level */
    uint64_t blocks_attempted;
    uint64_t blocks_recovered;
    uint64_t blocks_failed;

    /* Derived */
    uint64_t lost_bytes;
    uint64_t lost_equiv_1500_pkts;
    double   fade_duration_us;   /* contiguous erasure duration at 10 Gbps   */
    double   recovery_rate;      /* recovered_packets   / transmitted_packets */
    double   exact_match_rate;   /* exact_match_packets / transmitted_packets */
} sim_result_t;

/* =========================================================================
 * Public API
 * =========================================================================*/

/*
 * sim_runner_global_init()
 *
 * Call once before any sim_run_campaign_case().
 * Initializes Wirehair and the stats subsystem.
 *
 * Returns 0 on success, -1 on failure.
 */
int sim_runner_global_init(void);

/*
 * sim_run_campaign_case()
 *
 * Runs one full real pipeline case using the existing implementation.
 *
 *   cfg         : geometry and default seed
 *   req         : channel events to apply on the real TX symbol stream
 *   out_result  : filled with real measured metrics on success
 *   out_channel : optional raw channel counts
 *
 * stats_reset() is called at entry.
 *
 * Returns 0 on success, -1 on pipeline error.
 */
int sim_run_campaign_case(const sim_config_t      *cfg,
                          const sim_run_request_t *req,
                          sim_result_t            *out_result,
                          channel_apply_result_t  *out_channel);

#ifdef __cplusplus
}
#endif

#endif /* SIM_RUNNER_H */