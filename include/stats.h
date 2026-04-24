#ifndef STATS_H
#define STATS_H

/**
 * @file stats.h
 * @brief Lock-free statistics collection for the FSO Gateway.
 *
 * This module centralizes runtime counters for simulation and future real-time
 * pipeline stages. Counters are implemented with C11 atomics so the hot path
 * can update them without mutex contention.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct stats_container
 * @brief Plain non-atomic snapshot/report structure.
 *
 * This structure is used only for reporting and snapshot export. It is not the
 * live synchronized storage used internally by the statistics subsystem.
 */
struct stats_container {
    uint64_t ingress_packets;
    uint64_t ingress_bytes;

    uint64_t transmitted_packets;
    uint64_t transmitted_bytes;

    uint64_t recovered_packets;
    uint64_t recovered_bytes;
    uint64_t failed_packets;

    uint64_t total_symbols;
    uint64_t lost_symbols;

    /* CRC integrity counters (zero when internal CRC is disabled) */
    uint64_t symbols_dropped_crc;   /* symbols discarded due to CRC failure   */
    uint64_t packet_fail_crc_drop;  /* packets that failed due to CRC drops   */

    uint64_t blocks_attempted;
    uint64_t blocks_recovered;
    uint64_t blocks_failed;

    uint64_t total_bursts;
    uint64_t current_burst_length;
    uint64_t max_burst_length;
    uint64_t sum_burst_lengths;

    uint64_t burst_len_1;
    uint64_t burst_len_2_5;
    uint64_t burst_len_6_10;
    uint64_t burst_len_11_50;
    uint64_t burst_len_51_100;
    uint64_t burst_len_101_500;
    uint64_t burst_len_501_plus;

    uint64_t bursts_exceeding_fec_span;
    uint64_t configured_fec_burst_span;
    uint64_t recoverable_bursts;
    uint64_t critical_bursts;

    uint64_t burst_samples_recorded;
    uint64_t burst_samples_dropped;

    uint64_t blocks_with_loss;
    uint64_t worst_holes_in_block;
    uint64_t total_holes_in_blocks;
};

/**
 * @brief Initialize the statistics subsystem and reset all counters.
 */
void stats_init(void);

/**
 * @brief Reset all counters and restart timing measurements.
 */
void stats_reset(void);

/**
 * @brief Configure the burst-length threshold above which a burst is counted
 *        as exceeding the recoverable FEC/interleaver span.
 *
 * @param span Recoverable span in symbols. A burst is flagged if burst_len > span.
 */
void stats_set_burst_fec_span(uint64_t span);

/**
 * @brief Record one ingress packet and its byte count.
 *
 * @param bytes Packet size in bytes.
 */
void stats_inc_ingress(size_t bytes);

/**
 * @brief Record one transmitted packet and its byte count.
 *
 * @param bytes Packet size in bytes.
 */
void stats_inc_transmitted(size_t bytes);

/**
 * @brief Record one recovered packet and its byte count.
 *
 * @param bytes Packet size in bytes.
 */
void stats_inc_recovered(size_t bytes);

/**
 * @brief Increment the failed-packet counter.
 */
void stats_inc_failed_packet(void);

/**
 * @brief Add symbol totals directly.
 *
 * @param total Number of symbols processed.
 * @param lost  Number of lost/erased symbols.
 */
void stats_add_symbols(uint64_t total, uint64_t lost);

/**
 * @brief Record one symbol in the RX stream and update burst diagnostics.
 *
 * @param lost True if the symbol was erased/lost, false otherwise.
 */
void stats_record_symbol(bool lost);

/**
 * @brief Flush the currently open burst, if any.
 *
 * Call once at end-of-stream so a trailing burst is accounted for.
 */
void stats_finalize_burst(void);

/**
 * @brief Record block-level hole count for diagnostics.
 *
 * @param holes Number of missing symbols in this block.
 */
void stats_record_block(uint64_t holes);

/**
 * @brief Increment the block-attempt counter.
 */
void stats_inc_block_attempt(void);

/**
 * @brief Increment the successfully recovered block counter.
 */
void stats_inc_block_success(void);

/**
 * @brief Increment the failed block counter.
 */
void stats_inc_block_failure(void);

/**
 * @brief Increment the CRC-dropped symbol counter.
 *
 * Called by the RX pipeline each time a symbol is discarded due to CRC
 * failure (internal_symbol_crc_enabled must be non-zero).  The symbol is
 * treated as an erasure and must not be forwarded to the deinterleaver.
 */
void stats_inc_crc_drop_symbol(void);

/**
 * @brief Increment the packet-level CRC-drop failure counter.
 *
 * Called when a packet fails to reconstruct because one or more of its
 * symbols were dropped due to CRC failure (creating erasures beyond the
 * FEC recovery threshold).  This is an additional diagnostic dimension;
 * it does NOT replace packet_fail_missing or packet_fail_corrupted.
 */
void stats_inc_crc_drop_packet_fail(void);

/**
 * @brief Atomically snapshot all counters into a plain stats_container.
 *
 * Each field is read with relaxed atomic loads. The resulting struct is a
 * point-in-time picture suitable for serialization (e.g. by control_server)
 * or external reporting. No locking is required.
 *
 * @param out Destination struct. Must be non-NULL.
 */
void stats_snapshot(struct stats_container *out);

/**
 * @brief Print a structured statistics report through LOG_INFO.
 */
void stats_report(void);

#ifdef __cplusplus
}
#endif

#endif /* STATS_H */
