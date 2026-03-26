/* tests/sim_runner.h — Reusable FSO pipeline engine for campaign testing. */

#ifndef SIM_RUNNER_H
#define SIM_RUNNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHANNEL_EVENT_ERASURE    = 0,
    CHANNEL_EVENT_CORRUPTION = 1
} channel_event_type_t;

typedef struct {
    channel_event_type_t type;
    int                  start_symbol;
    int                  length_symbols;
} channel_event_t;

typedef struct {
    int      k;
    int      m;
    int      depth;
    int      symbol_size;
    int      num_windows;
    uint32_t seed;
} sim_config_t;

#define SIM_MAX_EVENTS 32

typedef struct {
    channel_event_t events[SIM_MAX_EVENTS];
    int             num_events;
    uint32_t        seed_override;
} sim_run_request_t;

typedef struct {
    uint64_t total_tx_symbols;
    uint64_t lost_symbols;
    uint64_t corrupted_symbols;
} channel_apply_result_t;

typedef struct {
    uint64_t generated_packets;
    uint64_t transmitted_packets;
    uint64_t recovered_packets;
    uint64_t exact_match_packets;
    uint64_t corrupted_packets;
    uint64_t missing_packets;

    uint64_t total_tx_symbols;
    uint64_t lost_symbols;
    uint64_t corrupted_symbols;

    uint64_t blocks_attempted;
    uint64_t blocks_recovered;
    uint64_t blocks_failed;

    uint64_t lost_bytes;
    uint64_t lost_equiv_1500_pkts;
    double   fade_duration_us;
    double   recovery_rate;
    double   exact_match_rate;
} sim_result_t;

enum {
    SR_FAIL_NONE = 0,
    SR_FAIL_TOO_MANY_HOLES = 1,
    SR_FAIL_INSUFFICIENT_SYMBOLS = 2,
    SR_FAIL_PACKET_AFTER_BLOCK_DECODE = 3
};

typedef struct {
    uint64_t blocks_total_expected;

    uint64_t blocks_attempted_for_decode;
    uint64_t blocks_decode_success;
    uint64_t blocks_decode_failed;

    uint64_t blocks_discarded_before_decode;
    uint64_t blocks_discarded_timeout_before_decode;
    uint64_t blocks_discarded_too_many_holes_before_decode;
    uint64_t blocks_discarded_evicted_before_decode;
    uint64_t blocks_discarded_ready_evicted_before_mark;

    uint64_t packet_fail_missing;
    uint64_t packet_fail_corrupted;
    uint64_t packet_fail_due_to_missing_blocks;
    uint64_t packet_fail_after_successful_block_decode;

    uint64_t max_missing_symbols_in_block;
    double   avg_missing_symbols_in_failed_blocks;

    bool     oracle_valid_for_run;
    uint64_t oracle_blocks_theoretically_recoverable;
    uint64_t oracle_blocks_theoretically_unrecoverable;
    uint64_t oracle_recoverable_but_not_decoded_successfully;
    uint64_t oracle_unrecoverable_and_not_decoded_successfully;
    uint64_t oracle_recoverable_but_discarded_before_decode;
    uint64_t oracle_recoverable_but_decode_failed;

    uint64_t failed_missing_sum;
    uint64_t failed_missing_count;
} sr_run_report_t;

typedef struct {
    uint64_t block_id;

    uint64_t expected_symbols;
    uint64_t survived_symbols;
    uint64_t source_symbols_survived;
    uint64_t repair_symbols_survived;
    uint64_t missing_symbols;

    bool     oracle_theoretically_recoverable;
    int      final_reason;

    bool     suspicious_oracle_recoverable_but_discarded_before_decode;
    bool     discarded_before_decode;
    bool     discarded_timeout_before_decode;
    bool     discarded_too_many_holes_before_decode;
    bool     discarded_evicted_before_decode;
    bool     discarded_ready_evicted_before_mark;

    /* Eviction trace fields — populated when has_eviction_trace is true */
    bool     has_eviction_trace;
    uint64_t eviction_incoming_block_id;
    uint32_t eviction_slot_index;
    uint32_t eviction_valid_symbols;
    uint32_t eviction_holes;
    uint32_t eviction_expected_symbols;
    uint32_t eviction_active_blocks;
    uint32_t eviction_max_active_blocks;

    uint32_t eviction_snapshot_count;
    uint32_t eviction_snapshot_indices[16];
    char     eviction_snapshot_states[16];
    uint64_t eviction_snapshot_block_ids[16];
    uint32_t eviction_snapshot_valid_symbols[16];
} sr_oracle_block_diag_t;

int sim_runner_global_init(void);
int sim_run_campaign_case(const sim_config_t      *cfg,
                          const sim_run_request_t *req,
                          sim_result_t            *out_result,
                          channel_apply_result_t  *out_channel);

void sim_runner_get_last_run_report(sr_run_report_t *out);
void sim_runner_get_last_oracle_block_diags(const sr_oracle_block_diag_t **out_blocks,
                                            size_t                       *out_count);
const char *sim_runner_failure_reason_name(int reason);
const char *sim_runner_block_final_reason_name(int reason);

#ifdef __cplusplus
}
#endif

#endif /* SIM_RUNNER_H */