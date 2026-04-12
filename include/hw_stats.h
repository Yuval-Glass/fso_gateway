/* include/hw_stats.h */

#ifndef HW_STATS_H
#define HW_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hw_stats hw_stats_t;

hw_stats_t *hw_stats_create(void);
/*
 * Allocates and zeroes all counters and histogram.
 * Records start time (clock_gettime CLOCK_MONOTONIC).
 * Returns non-NULL on success, NULL on allocation failure.
 */

void hw_stats_destroy(hw_stats_t *s);
/* Frees all resources. Safe to call with NULL. */

/* Increment functions — all thread-safe via __atomic builtins */
void hw_stats_block_success(hw_stats_t *s);
void hw_stats_block_failed(hw_stats_t *s);
void hw_stats_block_too_many_holes(hw_stats_t *s);
void hw_stats_block_timeout(hw_stats_t *s);
void hw_stats_symbol_received(hw_stats_t *s);
void hw_stats_symbol_crc_dropped(hw_stats_t *s);
void hw_stats_packet_recovered(hw_stats_t *s);
void hw_stats_packet_failed(hw_stats_t *s);

void hw_stats_set_packets_injected(hw_stats_t *s, uint64_t n);
/*
 * Sets the injected packet count (called once at end of run
 * from the injector thread's final count).
 */

void hw_stats_record_block_holes(hw_stats_t *s, int hole_count);
/*
 * Called after every block decode attempt with the number of
 * missing symbols (holes) in that block (0 = no loss).
 * Maintains internal running_burst counter:
 *   - if hole_count > 0: add hole_count to running_burst
 *   - if hole_count == 0 AND running_burst > 0:
 *       record running_burst in histogram, reset to 0
 * Thread-safe via mutex (burst tracking is stateful).
 */

void hw_stats_print_report(const hw_stats_t *s);
/*
 * Prints full report to stdout.
 */

int hw_stats_save_csv(const hw_stats_t *s, const char *dirpath);
/*
 * Saves two CSV files to dirpath:
 *
 * File 1: gateway_stats_<timestamp>.csv
 *   One row with all scalar counters:
 *   duration_s,blocks_attempted,blocks_success,blocks_failed,
 *   blocks_too_many_holes,blocks_timeout,symbols_received,
 *   symbols_crc_dropped,symbols_lost,packets_injected,
 *   packets_recovered,packets_failed,recovery_rate,
 *   total_burst_events,longest_burst,avg_burst_length
 *
 * File 2: burst_histogram_<timestamp>.csv
 *   Header: burst_len,count
 *   One row per non-zero histogram bucket (up to longest_burst_seen)
 *
 * Timestamp format: YYYYMMDD_HHMMSS (from localtime of run start)
 * Returns 0 on success, -1 on any file I/O error.
 */

void hw_stats_print_live(const hw_stats_t *s, int elapsed_s);
/*
 * Prints a single compact live-update line to stdout:
 * [STATS Xs] blocks=N recovered=N lost=N longest_burst=N
 * Used by the live stats thread in gateway_test.
 */

#ifdef __cplusplus
}
#endif

#endif /* HW_STATS_H */