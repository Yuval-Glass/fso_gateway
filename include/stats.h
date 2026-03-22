#ifndef STATS_H
#define STATS_H

/**
 * @file stats.h
 * @brief Thread-safe statistics collection for the FSO Gateway.
 *
 * This module provides a minimal but robust statistics layer for tracking
 * packet flow and future FEC-related recovery information.
 *
 * The counters are stored internally in a global static container in stats.c,
 * and access is protected with a mutex because the gateway is expected to
 * become multi-threaded in future development stages.
 */

#include <stdint.h>

/**
 * @struct stats_container
 * @brief Holds all runtime counters for the gateway.
 *
 * All counters use uint64_t so they can safely count very large numbers of
 * packets during long runs without overflowing quickly.
 */
struct stats_container {
    uint64_t rx_packets;             /**< Number of received packets */
    uint64_t tx_packets;             /**< Number of transmitted packets */
    uint64_t dropped_packets;        /**< Number of dropped packets */
    uint64_t fec_recovered_packets;  /**< Number of packets recovered by FEC */
};

/**
 * @brief Initialize the statistics module.
 *
 * Resets all counters to zero and prepares the internal mutex.
 *
 * This function must be called once during program startup before any
 * increment or report function is used.
 */
void stats_init(void);

/**
 * @brief Increment the received-packet counter.
 *
 * Thread-safe.
 */
void stats_increment_rx(void);

/**
 * @brief Increment the transmitted-packet counter.
 *
 * Thread-safe.
 */
void stats_increment_tx(void);

/**
 * @brief Print the current statistics counters using the logging system.
 *
 * The report is emitted using LOG_INFO so it integrates with the project's
 * centralized logging output.
 *
 * Thread-safe.
 */
void stats_report(void);

#endif /* STATS_H */