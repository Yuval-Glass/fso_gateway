/**
 * @file stats.c
 * @brief Lock-free runtime statistics for the FSO Gateway.
 */

#define _POSIX_C_SOURCE 200112L

#include "stats.h"
#include "logging.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* -------------------------------------------------------------------------- */
/* Internal storage                                                            */
/* -------------------------------------------------------------------------- */

#define STATS_MAX_BURST_SAMPLES  4096U

typedef struct {
    atomic_uint_fast64_t ingress_packets;
    atomic_uint_fast64_t ingress_bytes;

    atomic_uint_fast64_t transmitted_packets;
    atomic_uint_fast64_t transmitted_bytes;

    atomic_uint_fast64_t recovered_packets;
    atomic_uint_fast64_t recovered_bytes;
    atomic_uint_fast64_t failed_packets;

    atomic_uint_fast64_t total_symbols;
    atomic_uint_fast64_t lost_symbols;

    /* CRC integrity counters */
    atomic_uint_fast64_t symbols_dropped_crc;
    atomic_uint_fast64_t packet_fail_crc_drop;

    atomic_uint_fast64_t blocks_attempted;
    atomic_uint_fast64_t blocks_recovered;
    atomic_uint_fast64_t blocks_failed;

    atomic_uint_fast64_t total_bursts;
    atomic_uint_fast64_t current_burst_length;
    atomic_uint_fast64_t max_burst_length;
    atomic_uint_fast64_t sum_burst_lengths;

    atomic_uint_fast64_t burst_len_1;
    atomic_uint_fast64_t burst_len_2_5;
    atomic_uint_fast64_t burst_len_6_10;
    atomic_uint_fast64_t burst_len_11_50;
    atomic_uint_fast64_t burst_len_51_100;
    atomic_uint_fast64_t burst_len_101_500;
    atomic_uint_fast64_t burst_len_501_plus;

    atomic_uint_fast64_t bursts_exceeding_fec_span;
    atomic_uint_fast64_t configured_fec_burst_span;
    atomic_uint_fast64_t recoverable_bursts;
    atomic_uint_fast64_t critical_bursts;

    atomic_uint_fast64_t burst_samples_recorded;
    atomic_uint_fast64_t burst_samples_dropped;

    atomic_uint_fast64_t blocks_with_loss;
    atomic_uint_fast64_t worst_holes_in_block;
    atomic_uint_fast64_t total_holes_in_blocks;
} stats_atomic_container_t;

static stats_atomic_container_t g_stats;
static struct timespec          g_start_time;
static uint64_t                 g_burst_samples[STATS_MAX_BURST_SAMPLES];

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

static uint64_t stats_atomic_load_u64(const atomic_uint_fast64_t *value)
{
    return (uint64_t)atomic_load_explicit(value, memory_order_relaxed);
}

static void stats_update_max(atomic_uint_fast64_t *target, uint64_t candidate)
{
    uint64_t current = (uint64_t)atomic_load_explicit(target, memory_order_relaxed);

    while (candidate > current) {
        if (atomic_compare_exchange_weak_explicit(target,
                                                  &current,
                                                  candidate,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
            break;
        }
    }
}

static void stats_bucket_burst_length(uint64_t burst_len)
{
    if (burst_len == 1U) {
        atomic_fetch_add_explicit(&g_stats.burst_len_1, 1U, memory_order_relaxed);
    } else if (burst_len <= 5U) {
        atomic_fetch_add_explicit(&g_stats.burst_len_2_5, 1U, memory_order_relaxed);
    } else if (burst_len <= 10U) {
        atomic_fetch_add_explicit(&g_stats.burst_len_6_10, 1U, memory_order_relaxed);
    } else if (burst_len <= 50U) {
        atomic_fetch_add_explicit(&g_stats.burst_len_11_50, 1U, memory_order_relaxed);
    } else if (burst_len <= 100U) {
        atomic_fetch_add_explicit(&g_stats.burst_len_51_100, 1U, memory_order_relaxed);
    } else if (burst_len <= 500U) {
        atomic_fetch_add_explicit(&g_stats.burst_len_101_500, 1U, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&g_stats.burst_len_501_plus, 1U, memory_order_relaxed);
    }
}

static void stats_store_burst_sample(uint64_t burst_len)
{
    uint64_t slot = stats_atomic_load_u64(&g_stats.burst_samples_recorded);

    while (slot < STATS_MAX_BURST_SAMPLES) {
        uint64_t desired = slot + 1U;
        if (atomic_compare_exchange_weak_explicit(&g_stats.burst_samples_recorded,
                                                  &slot,
                                                  desired,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
            g_burst_samples[slot] = burst_len;
            return;
        }
    }

    atomic_fetch_add_explicit(&g_stats.burst_samples_dropped, 1U, memory_order_relaxed);
}

static void stats_close_current_burst(void)
{
    uint64_t burst_len;
    uint64_t fec_span;

    burst_len = (uint64_t)atomic_exchange_explicit(&g_stats.current_burst_length,
                                                   0U,
                                                   memory_order_relaxed);
    if (burst_len == 0U) {
        return;
    }

    atomic_fetch_add_explicit(&g_stats.total_bursts, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_stats.sum_burst_lengths, burst_len, memory_order_relaxed);
    stats_update_max(&g_stats.max_burst_length, burst_len);
    stats_bucket_burst_length(burst_len);
    stats_store_burst_sample(burst_len);

    fec_span = stats_atomic_load_u64(&g_stats.configured_fec_burst_span);
    if (fec_span > 0U && burst_len > fec_span) {
        atomic_fetch_add_explicit(&g_stats.critical_bursts, 1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_stats.bursts_exceeding_fec_span,
                                  1U,
                                  memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&g_stats.recoverable_bursts, 1U, memory_order_relaxed);
    }
}

static void stats_snapshot(struct stats_container *out)
{
    if (out == NULL) {
        return;
    }

    out->ingress_packets           = stats_atomic_load_u64(&g_stats.ingress_packets);
    out->ingress_bytes             = stats_atomic_load_u64(&g_stats.ingress_bytes);
    out->transmitted_packets       = stats_atomic_load_u64(&g_stats.transmitted_packets);
    out->transmitted_bytes         = stats_atomic_load_u64(&g_stats.transmitted_bytes);
    out->recovered_packets         = stats_atomic_load_u64(&g_stats.recovered_packets);
    out->recovered_bytes           = stats_atomic_load_u64(&g_stats.recovered_bytes);
    out->failed_packets            = stats_atomic_load_u64(&g_stats.failed_packets);
    out->total_symbols             = stats_atomic_load_u64(&g_stats.total_symbols);
    out->lost_symbols              = stats_atomic_load_u64(&g_stats.lost_symbols);
    out->symbols_dropped_crc       = stats_atomic_load_u64(&g_stats.symbols_dropped_crc);
    out->packet_fail_crc_drop      = stats_atomic_load_u64(&g_stats.packet_fail_crc_drop);
    out->blocks_attempted          = stats_atomic_load_u64(&g_stats.blocks_attempted);
    out->blocks_recovered          = stats_atomic_load_u64(&g_stats.blocks_recovered);
    out->blocks_failed             = stats_atomic_load_u64(&g_stats.blocks_failed);

    out->total_bursts              = stats_atomic_load_u64(&g_stats.total_bursts);
    out->current_burst_length      = stats_atomic_load_u64(&g_stats.current_burst_length);
    out->max_burst_length          = stats_atomic_load_u64(&g_stats.max_burst_length);
    out->sum_burst_lengths         = stats_atomic_load_u64(&g_stats.sum_burst_lengths);

    out->burst_len_1               = stats_atomic_load_u64(&g_stats.burst_len_1);
    out->burst_len_2_5             = stats_atomic_load_u64(&g_stats.burst_len_2_5);
    out->burst_len_6_10            = stats_atomic_load_u64(&g_stats.burst_len_6_10);
    out->burst_len_11_50           = stats_atomic_load_u64(&g_stats.burst_len_11_50);
    out->burst_len_51_100          = stats_atomic_load_u64(&g_stats.burst_len_51_100);
    out->burst_len_101_500         = stats_atomic_load_u64(&g_stats.burst_len_101_500);
    out->burst_len_501_plus        = stats_atomic_load_u64(&g_stats.burst_len_501_plus);

    out->bursts_exceeding_fec_span = stats_atomic_load_u64(&g_stats.bursts_exceeding_fec_span);
    out->configured_fec_burst_span = stats_atomic_load_u64(&g_stats.configured_fec_burst_span);
    out->recoverable_bursts        = stats_atomic_load_u64(&g_stats.recoverable_bursts);
    out->critical_bursts           = stats_atomic_load_u64(&g_stats.critical_bursts);

    out->burst_samples_recorded    = stats_atomic_load_u64(&g_stats.burst_samples_recorded);
    out->burst_samples_dropped     = stats_atomic_load_u64(&g_stats.burst_samples_dropped);

    out->blocks_with_loss          = stats_atomic_load_u64(&g_stats.blocks_with_loss);
    out->worst_holes_in_block      = stats_atomic_load_u64(&g_stats.worst_holes_in_block);
    out->total_holes_in_blocks     = stats_atomic_load_u64(&g_stats.total_holes_in_blocks);
}

static void stats_safe_rate(char *out, size_t out_size,
                            uint64_t numerator, uint64_t denominator)
{
    double pct;

    if (out == NULL || out_size == 0U) {
        return;
    }

    if (denominator == 0U) {
        snprintf(out, out_size, "N/A");
        return;
    }

    pct = (100.0 * (double)numerator) / (double)denominator;
    snprintf(out, out_size, "%.2f%%", pct);
}

static void stats_safe_residual(char *out, size_t out_size,
                                uint64_t recovered, uint64_t transmitted)
{
    double pct;

    if (out == NULL || out_size == 0U) {
        return;
    }

    if (transmitted == 0U) {
        snprintf(out, out_size, "N/A");
        return;
    }

    pct = 100.0 * (1.0 - ((double)recovered / (double)transmitted));
    if (pct < 0.0) {
        pct = 0.0;
    }

    snprintf(out, out_size, "%.2f%%", pct);
}

static void stats_safe_percent_string(char *out, size_t out_size,
                                      uint64_t numerator, uint64_t denominator)
{
    double pct;

    if (out == NULL || out_size == 0U) {
        return;
    }

    if (denominator == 0U) {
        snprintf(out, out_size, "N/A");
        return;
    }

    pct = (100.0 * (double)numerator) / (double)denominator;
    snprintf(out, out_size, "%.2f%%", pct);
}

static double stats_elapsed_seconds(void)
{
    struct timespec now;
    double seconds;
    double nanoseconds;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0.0;
    }

    seconds     = (double)(now.tv_sec - g_start_time.tv_sec);
    nanoseconds = (double)(now.tv_nsec - g_start_time.tv_nsec);

    return seconds + (nanoseconds / 1000000000.0);
}

static int stats_compare_u64(const void *a, const void *b)
{
    const uint64_t va = *(const uint64_t *)a;
    const uint64_t vb = *(const uint64_t *)b;

    if (va < vb) {
        return -1;
    }
    if (va > vb) {
        return 1;
    }
    return 0;
}

static int stats_percentile_value(uint64_t *out_value,
                                  uint64_t numerator,
                                  uint64_t denominator)
{
    uint64_t count;
    uint64_t work[STATS_MAX_BURST_SAMPLES];
    uint64_t rank;
    uint64_t index;

    if (out_value == NULL || denominator == 0U) {
        return -1;
    }

    count = stats_atomic_load_u64(&g_stats.burst_samples_recorded);
    if (count == 0U) {
        return -1;
    }
    if (count > STATS_MAX_BURST_SAMPLES) {
        count = STATS_MAX_BURST_SAMPLES;
    }

    for (index = 0U; index < count; ++index) {
        work[index] = g_burst_samples[index];
    }

    qsort(work, (size_t)count, sizeof(work[0]), stats_compare_u64);

    rank = (numerator * count + denominator - 1U) / denominator;
    if (rank == 0U) {
        rank = 1U;
    }
    if (rank > count) {
        rank = count;
    }

    *out_value = work[rank - 1U];
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

void stats_reset(void)
{
    atomic_store_explicit(&g_stats.ingress_packets,           0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.ingress_bytes,             0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.transmitted_packets,       0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.transmitted_bytes,         0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.recovered_packets,         0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.recovered_bytes,           0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.failed_packets,            0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.total_symbols,             0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.lost_symbols,              0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.symbols_dropped_crc,       0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.packet_fail_crc_drop,      0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.blocks_attempted,          0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.blocks_recovered,          0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.blocks_failed,             0U, memory_order_relaxed);

    atomic_store_explicit(&g_stats.total_bursts,              0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.current_burst_length,      0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.max_burst_length,          0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.sum_burst_lengths,         0U, memory_order_relaxed);

    atomic_store_explicit(&g_stats.burst_len_1,               0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.burst_len_2_5,             0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.burst_len_6_10,            0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.burst_len_11_50,           0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.burst_len_51_100,          0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.burst_len_101_500,         0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.burst_len_501_plus,        0U, memory_order_relaxed);

    atomic_store_explicit(&g_stats.bursts_exceeding_fec_span, 0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.configured_fec_burst_span, 0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.recoverable_bursts,        0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.critical_bursts,           0U, memory_order_relaxed);

    atomic_store_explicit(&g_stats.burst_samples_recorded,    0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.burst_samples_dropped,     0U, memory_order_relaxed);

    atomic_store_explicit(&g_stats.blocks_with_loss,          0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.worst_holes_in_block,      0U, memory_order_relaxed);
    atomic_store_explicit(&g_stats.total_holes_in_blocks,     0U, memory_order_relaxed);

    clock_gettime(CLOCK_MONOTONIC, &g_start_time);
}

void stats_init(void)
{
    stats_reset();
}

void stats_set_burst_fec_span(uint64_t span)
{
    atomic_store_explicit(&g_stats.configured_fec_burst_span,
                          span,
                          memory_order_relaxed);
}

void stats_inc_ingress(size_t bytes)
{
    atomic_fetch_add_explicit(&g_stats.ingress_packets, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_stats.ingress_bytes,
                              (uint64_t)bytes,
                              memory_order_relaxed);
}

void stats_inc_transmitted(size_t bytes)
{
    atomic_fetch_add_explicit(&g_stats.transmitted_packets, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_stats.transmitted_bytes,
                              (uint64_t)bytes,
                              memory_order_relaxed);
}

void stats_inc_recovered(size_t bytes)
{
    atomic_fetch_add_explicit(&g_stats.recovered_packets, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_stats.recovered_bytes,
                              (uint64_t)bytes,
                              memory_order_relaxed);
}

void stats_inc_failed_packet(void)
{
    atomic_fetch_add_explicit(&g_stats.failed_packets, 1U, memory_order_relaxed);
}

void stats_add_symbols(uint64_t total, uint64_t lost)
{
    if (total > 0U) {
        atomic_fetch_add_explicit(&g_stats.total_symbols, total, memory_order_relaxed);
    }

    if (lost > 0U) {
        atomic_fetch_add_explicit(&g_stats.lost_symbols, lost, memory_order_relaxed);
    }
}

void stats_record_symbol(bool lost)
{
    atomic_fetch_add_explicit(&g_stats.total_symbols, 1U, memory_order_relaxed);

    if (lost) {
        atomic_fetch_add_explicit(&g_stats.lost_symbols, 1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_stats.current_burst_length, 1U, memory_order_relaxed);
        return;
    }

    stats_close_current_burst();
}

void stats_finalize_burst(void)
{
    stats_close_current_burst();
}

void stats_record_block(uint64_t holes)
{
    if (holes == 0U) {
        return;
    }

    atomic_fetch_add_explicit(&g_stats.blocks_with_loss, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_stats.total_holes_in_blocks, holes, memory_order_relaxed);
    stats_update_max(&g_stats.worst_holes_in_block, holes);
}

void stats_inc_block_attempt(void)
{
    atomic_fetch_add_explicit(&g_stats.blocks_attempted, 1U, memory_order_relaxed);
}

void stats_inc_block_success(void)
{
    atomic_fetch_add_explicit(&g_stats.blocks_recovered, 1U, memory_order_relaxed);
}

void stats_inc_block_failure(void)
{
    atomic_fetch_add_explicit(&g_stats.blocks_failed, 1U, memory_order_relaxed);
}

void stats_inc_crc_drop_symbol(void)
{
    atomic_fetch_add_explicit(&g_stats.symbols_dropped_crc, 1U, memory_order_relaxed);
}

void stats_inc_crc_drop_packet_fail(void)
{
    atomic_fetch_add_explicit(&g_stats.packet_fail_crc_drop, 1U, memory_order_relaxed);
}

void stats_report(void)
{
    struct stats_container snapshot;
    char recovery_rate[32];
    char residual_loss[32];
    char fec_efficiency[32];
    char recoverable_pct[32];
    char critical_pct[32];
    char p50_str[32];
    char p90_str[32];
    char p99_str[32];
    double elapsed_s;
    double ingress_mib;
    double transmitted_mib;
    double recovered_mib;
    double avg_burst_length;
    double avg_holes_per_affected_block;
    uint64_t p50_value;
    uint64_t p90_value;
    uint64_t p99_value;

    stats_finalize_burst();
    stats_snapshot(&snapshot);

    stats_safe_rate(recovery_rate,
                    sizeof(recovery_rate),
                    snapshot.recovered_packets,
                    snapshot.ingress_packets);

    stats_safe_residual(residual_loss,
                        sizeof(residual_loss),
                        snapshot.recovered_packets,
                        snapshot.transmitted_packets);

    stats_safe_rate(fec_efficiency,
                    sizeof(fec_efficiency),
                    snapshot.blocks_recovered,
                    snapshot.blocks_attempted);

    stats_safe_percent_string(recoverable_pct,
                              sizeof(recoverable_pct),
                              snapshot.recoverable_bursts,
                              snapshot.total_bursts);

    stats_safe_percent_string(critical_pct,
                              sizeof(critical_pct),
                              snapshot.critical_bursts,
                              snapshot.total_bursts);

    if (stats_percentile_value(&p50_value, 50U, 100U) == 0) {
        snprintf(p50_str, sizeof(p50_str), "%" PRIu64, p50_value);
    } else {
        snprintf(p50_str, sizeof(p50_str), "N/A");
    }

    if (stats_percentile_value(&p90_value, 90U, 100U) == 0) {
        snprintf(p90_str, sizeof(p90_str), "%" PRIu64, p90_value);
    } else {
        snprintf(p90_str, sizeof(p90_str), "N/A");
    }

    if (stats_percentile_value(&p99_value, 99U, 100U) == 0) {
        snprintf(p99_str, sizeof(p99_str), "%" PRIu64, p99_value);
    } else {
        snprintf(p99_str, sizeof(p99_str), "N/A");
    }

    elapsed_s       = stats_elapsed_seconds();
    ingress_mib     = (double)snapshot.ingress_bytes / (1024.0 * 1024.0);
    transmitted_mib = (double)snapshot.transmitted_bytes / (1024.0 * 1024.0);
    recovered_mib   = (double)snapshot.recovered_bytes / (1024.0 * 1024.0);

    avg_burst_length = (snapshot.total_bursts > 0U)
                     ? ((double)snapshot.sum_burst_lengths / (double)snapshot.total_bursts)
                     : 0.0;

    avg_holes_per_affected_block = (snapshot.blocks_with_loss > 0U)
                                 ? ((double)snapshot.total_holes_in_blocks /
                                    (double)snapshot.blocks_with_loss)
                                 : 0.0;

    LOG_INFO("===== FSO GATEWAY STATS =====");

    LOG_INFO("Packets:");
    LOG_INFO("  Ingress:              %" PRIu64, snapshot.ingress_packets);
    LOG_INFO("  Transmitted:          %" PRIu64, snapshot.transmitted_packets);
    LOG_INFO("  Recovered:            %" PRIu64, snapshot.recovered_packets);
    LOG_INFO("  Failed:               %" PRIu64, snapshot.failed_packets);

    LOG_INFO("Bytes:");
    LOG_INFO("  Ingress:              %" PRIu64 " (%.3f MiB)",
             snapshot.ingress_bytes, ingress_mib);
    LOG_INFO("  Transmitted:          %" PRIu64 " (%.3f MiB)",
             snapshot.transmitted_bytes, transmitted_mib);
    LOG_INFO("  Recovered:            %" PRIu64 " (%.3f MiB)",
             snapshot.recovered_bytes, recovered_mib);

    LOG_INFO("Symbols:");
    LOG_INFO("  Total:                %" PRIu64, snapshot.total_symbols);
    LOG_INFO("  Lost:                 %" PRIu64, snapshot.lost_symbols);
    LOG_INFO("  CRC-Dropped:          %" PRIu64, snapshot.symbols_dropped_crc);

    LOG_INFO("CRC Integrity:");
    LOG_INFO("  Symbols dropped (CRC):%" PRIu64, snapshot.symbols_dropped_crc);
    LOG_INFO("  Packet fails (CRC):   %" PRIu64, snapshot.packet_fail_crc_drop);

    LOG_INFO("FEC:");
    LOG_INFO("  Blocks Attempted:     %" PRIu64, snapshot.blocks_attempted);
    LOG_INFO("  Blocks Recovered:     %" PRIu64, snapshot.blocks_recovered);
    LOG_INFO("  Blocks Failed:        %" PRIu64, snapshot.blocks_failed);

    LOG_INFO("Performance:");
    LOG_INFO("  Recovery Rate:        %s", recovery_rate);
    LOG_INFO("  Residual Loss:        %s", residual_loss);
    LOG_INFO("  FEC Efficiency:       %s", fec_efficiency);

    if (elapsed_s > 0.0) {
        LOG_INFO("  Runtime:              %.3f s", elapsed_s);
        LOG_INFO("  Ingress Throughput:   %.3f MiB/s", ingress_mib / elapsed_s);
        LOG_INFO("  Transmit Throughput:  %.3f MiB/s", transmitted_mib / elapsed_s);
        LOG_INFO("  Recover Throughput:   %.3f MiB/s", recovered_mib / elapsed_s);
    } else {
        LOG_INFO("  Runtime:              N/A");
    }

    LOG_INFO("===== CHANNEL DIAGNOSTICS =====");

    LOG_INFO("Burst Statistics:");
    LOG_INFO("  Total Bursts:         %" PRIu64, snapshot.total_bursts);
    LOG_INFO("  Max Burst Length:     %" PRIu64, snapshot.max_burst_length);
    LOG_INFO("  Avg Burst Length:     %.2f", avg_burst_length);

    LOG_INFO("Burst Percentiles:");
    LOG_INFO("  P50:                  %s", p50_str);
    LOG_INFO("  P90:                  %s", p90_str);
    LOG_INFO("  P99:                  %s", p99_str);

    LOG_INFO("Burst Histogram:");
    LOG_INFO("  1:                    %" PRIu64, snapshot.burst_len_1);
    LOG_INFO("  2-5:                  %" PRIu64, snapshot.burst_len_2_5);
    LOG_INFO("  6-10:                 %" PRIu64, snapshot.burst_len_6_10);
    LOG_INFO("  11-50:                %" PRIu64, snapshot.burst_len_11_50);
    LOG_INFO("  51-100:               %" PRIu64, snapshot.burst_len_51_100);
    LOG_INFO("  101-500:              %" PRIu64, snapshot.burst_len_101_500);
    LOG_INFO("  501+:                 %" PRIu64, snapshot.burst_len_501_plus);

    LOG_INFO("Recoverability:");
    LOG_INFO("  Configured FEC Span:  %" PRIu64, snapshot.configured_fec_burst_span);
    LOG_INFO("  Recoverable Bursts:   %" PRIu64 " (%s)",
             snapshot.recoverable_bursts, recoverable_pct);
    LOG_INFO("  Critical Bursts:      %" PRIu64 " (%s)",
             snapshot.critical_bursts, critical_pct);

    LOG_INFO("Block Stress:");
    LOG_INFO("  Blocks with loss:     %" PRIu64, snapshot.blocks_with_loss);
    LOG_INFO("  Worst holes/block:    %" PRIu64, snapshot.worst_holes_in_block);
    LOG_INFO("  Avg holes/block:      %.2f", avg_holes_per_affected_block);

    if (snapshot.burst_samples_dropped > 0U) {
        LOG_INFO("  Burst samples dropped:%" PRIu64, snapshot.burst_samples_dropped);
    }

    LOG_INFO("================================");
}
