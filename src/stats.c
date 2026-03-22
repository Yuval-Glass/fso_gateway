/**
 * @file stats.c
 * @brief Thread-safe runtime statistics for the FSO Gateway.
 */

#include "stats.h"
#include "logging.h"

#include <pthread.h>
#include <string.h>

/**
 * @brief Global statistics storage.
 *
 * This object is intentionally kept private to this translation unit so that
 * external code cannot modify counters directly without synchronization.
 */
static struct stats_container g_stats;

/**
 * @brief Mutex protecting all access to g_stats.
 *
 * Since packet processing will eventually happen across multiple threads,
 * every modification and report operation is guarded by this mutex.
 */
static pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Initialize the statistics subsystem.
 *
 * Resets all counters to zero. The mutex is statically initialized above,
 * so no explicit pthread_mutex_init() call is required here.
 */
void stats_init(void)
{
    pthread_mutex_lock(&g_stats_mutex);
    memset(&g_stats, 0, sizeof(g_stats));
    pthread_mutex_unlock(&g_stats_mutex);
}

/**
 * @brief Increment the RX packet counter in a thread-safe way.
 */
void stats_increment_rx(void)
{
    pthread_mutex_lock(&g_stats_mutex);
    g_stats.rx_packets++;
    pthread_mutex_unlock(&g_stats_mutex);
}

/**
 * @brief Increment the TX packet counter in a thread-safe way.
 */
void stats_increment_tx(void)
{
    pthread_mutex_lock(&g_stats_mutex);
    g_stats.tx_packets++;
    pthread_mutex_unlock(&g_stats_mutex);
}

/**
 * @brief Print a snapshot of the current counters.
 *
 * A local copy is taken while holding the mutex, then the lock is released
 * before logging. This keeps the critical section short and avoids holding
 * the mutex during formatted output.
 */
void stats_report(void)
{
    struct stats_container snapshot;

    pthread_mutex_lock(&g_stats_mutex);
    snapshot = g_stats;
    pthread_mutex_unlock(&g_stats_mutex);

    LOG_INFO("=== Statistics Report ===");
    LOG_INFO("RX packets            : %llu",
             (unsigned long long)snapshot.rx_packets);
    LOG_INFO("TX packets            : %llu",
             (unsigned long long)snapshot.tx_packets);
    LOG_INFO("Dropped packets       : %llu",
             (unsigned long long)snapshot.dropped_packets);
    LOG_INFO("FEC recovered packets : %llu",
             (unsigned long long)snapshot.fec_recovered_packets);
}