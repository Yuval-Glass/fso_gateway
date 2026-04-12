/* src/hw_stats.c */

#define _POSIX_C_SOURCE 200112L

#include "hw_stats.h"
#include "logging.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define HW_STATS_BURST_BUCKETS 257
#define HW_STATS_BURST_MAX     256

struct hw_stats {
    uint64_t blocks_attempted;
    uint64_t blocks_decode_success;
    uint64_t blocks_decode_failed;
    uint64_t blocks_too_many_holes;
    uint64_t blocks_timeout;

    uint64_t symbols_received;
    uint64_t symbols_crc_dropped;

    uint64_t packets_injected;
    uint64_t packets_recovered;
    uint64_t packets_failed;

    uint64_t burst_histogram[HW_STATS_BURST_BUCKETS];
    uint64_t running_burst;

    struct timespec start_mono;
    time_t          start_wall;

    pthread_mutex_t burst_mutex;
};

typedef struct {
    double   duration_s;
    uint64_t blocks_attempted;
    uint64_t blocks_decode_success;
    uint64_t blocks_decode_failed;
    uint64_t blocks_too_many_holes;
    uint64_t blocks_timeout;
    uint64_t symbols_received;
    uint64_t symbols_crc_dropped;
    uint64_t symbols_lost_total;
    uint64_t packets_injected;
    uint64_t packets_recovered;
    uint64_t packets_failed;
    double   recovery_rate;
    uint64_t total_burst_events;
    uint64_t longest_burst_seen;
    double   avg_burst_length;
} hw_stats_snapshot_t;

static uint64_t hw_stats_load_u64(const uint64_t *value)
{
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}

static void hw_stats_inc_u64(uint64_t *value)
{
    __atomic_fetch_add(value, 1U, __ATOMIC_RELAXED);
}

static void hw_stats_add_u64(uint64_t *value, uint64_t addend)
{
    __atomic_fetch_add(value, addend, __ATOMIC_RELAXED);
}

static void hw_stats_finalize_running_burst(hw_stats_t *s)
{
    uint64_t burst_len;
    uint64_t bucket_index;

    if (s == NULL) {
        LOG_ERROR("[hw_stats] finalize_running_burst: stats handle is NULL");
        return;
    }

    if (pthread_mutex_lock(&s->burst_mutex) != 0) {
        LOG_ERROR("[hw_stats] finalize_running_burst: pthread_mutex_lock failed");
        return;
    }

    burst_len = s->running_burst;
    if (burst_len > 0U) {
        bucket_index = burst_len;
        if (bucket_index > HW_STATS_BURST_MAX) {
            bucket_index = HW_STATS_BURST_MAX;
        }

        s->burst_histogram[bucket_index]++;
        s->running_burst = 0U;
    }

    if (pthread_mutex_unlock(&s->burst_mutex) != 0) {
        LOG_ERROR("[hw_stats] finalize_running_burst: pthread_mutex_unlock failed");
        return;
    }
}

static double hw_stats_elapsed_seconds(const hw_stats_t *s)
{
    struct timespec now;
    double          seconds;
    double          nanoseconds;

    if (s == NULL) {
        LOG_ERROR("[hw_stats] elapsed_seconds: stats handle is NULL");
        return 0.0;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        LOG_ERROR("[hw_stats] elapsed_seconds: clock_gettime failed: %s",
                  strerror(errno));
        return 0.0;
    }

    seconds     = (double)(now.tv_sec - s->start_mono.tv_sec);
    nanoseconds = (double)(now.tv_nsec - s->start_mono.tv_nsec);

    return seconds + (nanoseconds / 1000000000.0);
}

static void hw_stats_compute_snapshot(const hw_stats_t *s,
                                      hw_stats_snapshot_t *out)
{
    uint64_t total_burst_events;
    uint64_t symbols_lost_total;
    uint64_t longest_burst_seen;
    uint64_t bucket;
    uint64_t count;
    uint64_t running_burst;
    uint64_t running_bucket;

    if (s == NULL || out == NULL) {
        LOG_ERROR("[hw_stats] compute_snapshot: invalid argument");
        return;
    }

    memset(out, 0, sizeof(*out));

    out->duration_s             = hw_stats_elapsed_seconds(s);
    out->blocks_attempted       = hw_stats_load_u64(&s->blocks_attempted);
    out->blocks_decode_success  = hw_stats_load_u64(&s->blocks_decode_success);
    out->blocks_decode_failed   = hw_stats_load_u64(&s->blocks_decode_failed);
    out->blocks_too_many_holes  = hw_stats_load_u64(&s->blocks_too_many_holes);
    out->blocks_timeout         = hw_stats_load_u64(&s->blocks_timeout);
    out->symbols_received       = hw_stats_load_u64(&s->symbols_received);
    out->symbols_crc_dropped    = hw_stats_load_u64(&s->symbols_crc_dropped);
    out->packets_injected       = hw_stats_load_u64(&s->packets_injected);
    out->packets_recovered      = hw_stats_load_u64(&s->packets_recovered);
    out->packets_failed         = hw_stats_load_u64(&s->packets_failed);

    if (out->packets_injected > 0U) {
        out->recovery_rate =
            (100.0 * (double)out->packets_recovered) /
            (double)out->packets_injected;
    }

    total_burst_events = 0U;
    symbols_lost_total = 0U;
    longest_burst_seen = 0U;

    if (pthread_mutex_lock((pthread_mutex_t *)&s->burst_mutex) != 0) {
        LOG_ERROR("[hw_stats] compute_snapshot: pthread_mutex_lock failed");
        return;
    }

    for (bucket = 1U; bucket <= HW_STATS_BURST_MAX; ++bucket) {
        count = s->burst_histogram[bucket];
        if (count == 0U) {
            continue;
        }

        total_burst_events += count;

        if (bucket < HW_STATS_BURST_MAX) {
            symbols_lost_total += (bucket * count);
            longest_burst_seen = bucket;
        } else {
            symbols_lost_total += (HW_STATS_BURST_MAX * count);
            longest_burst_seen = HW_STATS_BURST_MAX;
        }
    }

    running_burst = s->running_burst;
    if (running_burst > 0U) {
        total_burst_events += 1U;

        running_bucket = running_burst;
        if (running_bucket > HW_STATS_BURST_MAX) {
            running_bucket = HW_STATS_BURST_MAX;
        }

        symbols_lost_total += running_bucket;
        if (running_bucket > longest_burst_seen) {
            longest_burst_seen = running_bucket;
        }
    }

    if (pthread_mutex_unlock((pthread_mutex_t *)&s->burst_mutex) != 0) {
        LOG_ERROR("[hw_stats] compute_snapshot: pthread_mutex_unlock failed");
        return;
    }

    out->total_burst_events = total_burst_events;
    out->symbols_lost_total = symbols_lost_total;
    out->longest_burst_seen = longest_burst_seen;

    if (total_burst_events > 0U) {
        out->avg_burst_length =
            (double)symbols_lost_total / (double)total_burst_events;
    }
}

static int hw_stats_make_timestamp(const hw_stats_t *s,
                                   char *buf,
                                   size_t buf_size)
{
    struct tm tm_local;

    if (s == NULL || buf == NULL || buf_size == 0U) {
        LOG_ERROR("[hw_stats] make_timestamp: invalid argument");
        return -1;
    }

    if (localtime_r(&s->start_wall, &tm_local) == NULL) {
        LOG_ERROR("[hw_stats] make_timestamp: localtime_r failed");
        return -1;
    }

    if (strftime(buf, buf_size, "%Y%m%d_%H%M%S", &tm_local) == 0U) {
        LOG_ERROR("[hw_stats] make_timestamp: strftime failed");
        return -1;
    }

    return 0;
}

hw_stats_t *hw_stats_create(void)
{
    hw_stats_t *s;
    int         rc;

    s = (hw_stats_t *)malloc(sizeof(hw_stats_t));
    if (s == NULL) {
        LOG_ERROR("[hw_stats] create: malloc failed");
        return NULL;
    }

    memset(s, 0, sizeof(*s));

    rc = pthread_mutex_init(&s->burst_mutex, NULL);
    if (rc != 0) {
        LOG_ERROR("[hw_stats] create: pthread_mutex_init failed (rc=%d)", rc);
        free(s);
        return NULL;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &s->start_mono) != 0) {
        LOG_ERROR("[hw_stats] create: clock_gettime failed: %s",
                  strerror(errno));
        pthread_mutex_destroy(&s->burst_mutex);
        free(s);
        return NULL;
    }

    s->start_wall = time(NULL);
    if (s->start_wall == (time_t)-1) {
        LOG_ERROR("[hw_stats] create: time() failed");
        pthread_mutex_destroy(&s->burst_mutex);
        free(s);
        return NULL;
    }

    return s;
}

void hw_stats_destroy(hw_stats_t *s)
{
    int rc;

    if (s == NULL) {
        return;
    }

    rc = pthread_mutex_destroy(&s->burst_mutex);
    if (rc != 0) {
        LOG_ERROR("[hw_stats] destroy: pthread_mutex_destroy failed (rc=%d)", rc);
    }

    free(s);
}

void hw_stats_block_success(hw_stats_t *s)
{
    if (s == NULL) {
        LOG_ERROR("[hw_stats] block_success: stats handle is NULL");
        return;
    }

    hw_stats_inc_u64(&s->blocks_attempted);
    hw_stats_inc_u64(&s->blocks_decode_success);
}

void hw_stats_block_failed(hw_stats_t *s)
{
    if (s == NULL) {
        LOG_ERROR("[hw_stats] block_failed: stats handle is NULL");
        return;
    }

    hw_stats_inc_u64(&s->blocks_attempted);
    hw_stats_inc_u64(&s->blocks_decode_failed);
}

void hw_stats_block_too_many_holes(hw_stats_t *s)
{
    if (s == NULL) {
        LOG_ERROR("[hw_stats] block_too_many_holes: stats handle is NULL");
        return;
    }

    hw_stats_inc_u64(&s->blocks_too_many_holes);
}

void hw_stats_block_timeout(hw_stats_t *s)
{
    if (s == NULL) {
        LOG_ERROR("[hw_stats] block_timeout: stats handle is NULL");
        return;
    }

    hw_stats_inc_u64(&s->blocks_timeout);
}

void hw_stats_symbol_received(hw_stats_t *s)
{
    if (s == NULL) {
        LOG_ERROR("[hw_stats] symbol_received: stats handle is NULL");
        return;
    }

    hw_stats_inc_u64(&s->symbols_received);
}

void hw_stats_symbol_crc_dropped(hw_stats_t *s)
{
    if (s == NULL) {
        LOG_ERROR("[hw_stats] symbol_crc_dropped: stats handle is NULL");
        return;
    }

    hw_stats_inc_u64(&s->symbols_crc_dropped);
}

void hw_stats_packet_recovered(hw_stats_t *s)
{
    if (s == NULL) {
        LOG_ERROR("[hw_stats] packet_recovered: stats handle is NULL");
        return;
    }

    hw_stats_inc_u64(&s->packets_recovered);
}

void hw_stats_packet_failed(hw_stats_t *s)
{
    if (s == NULL) {
        LOG_ERROR("[hw_stats] packet_failed: stats handle is NULL");
        return;
    }

    hw_stats_inc_u64(&s->packets_failed);
}

void hw_stats_set_packets_injected(hw_stats_t *s, uint64_t n)
{
    if (s == NULL) {
        LOG_ERROR("[hw_stats] set_packets_injected: stats handle is NULL");
        return;
    }

    __atomic_store_n(&s->packets_injected, n, __ATOMIC_RELAXED);
}

void hw_stats_record_block_holes(hw_stats_t *s, int hole_count)
{
    uint64_t bucket_index;
    int      rc;

    if (s == NULL) {
        LOG_ERROR("[hw_stats] record_block_holes: stats handle is NULL");
        return;
    }

    if (hole_count < 0) {
        LOG_ERROR("[hw_stats] record_block_holes: invalid hole_count=%d",
                  hole_count);
        return;
    }

    rc = pthread_mutex_lock(&s->burst_mutex);
    if (rc != 0) {
        LOG_ERROR("[hw_stats] record_block_holes: pthread_mutex_lock failed "
                  "(rc=%d)", rc);
        return;
    }

    if (hole_count > 0) {
        s->running_burst += (uint64_t)hole_count;
    } else if (s->running_burst > 0U) {
        bucket_index = s->running_burst;
        if (bucket_index > HW_STATS_BURST_MAX) {
            bucket_index = HW_STATS_BURST_MAX;
        }

        s->burst_histogram[bucket_index]++;
        s->running_burst = 0U;
    }

    rc = pthread_mutex_unlock(&s->burst_mutex);
    if (rc != 0) {
        LOG_ERROR("[hw_stats] record_block_holes: pthread_mutex_unlock failed "
                  "(rc=%d)", rc);
        return;
    }
}

void hw_stats_print_report(const hw_stats_t *s)
{
    hw_stats_snapshot_t snapshot;
    hw_stats_t         *mutable_stats;
    uint64_t            bucket;
    uint64_t            count;

    if (s == NULL) {
        LOG_ERROR("[hw_stats] print_report: stats handle is NULL");
        return;
    }

    mutable_stats = (hw_stats_t *)s;
    hw_stats_finalize_running_burst(mutable_stats);
    hw_stats_compute_snapshot(s, &snapshot);

    printf("=== GATEWAY RUN SUMMARY ===\n");
    printf("Duration:              %.2fs\n", snapshot.duration_s);
    printf("--- Block Statistics ---\n");
    printf("blocks_attempted:      %llu\n",
           (unsigned long long)snapshot.blocks_attempted);
    printf("blocks_decode_success: %llu\n",
           (unsigned long long)snapshot.blocks_decode_success);
    printf("blocks_decode_failed:  %llu\n",
           (unsigned long long)snapshot.blocks_decode_failed);
    printf("blocks_too_many_holes: %llu\n",
           (unsigned long long)snapshot.blocks_too_many_holes);
    printf("blocks_timeout:        %llu\n",
           (unsigned long long)snapshot.blocks_timeout);
    printf("--- Symbol Statistics ---\n");
    printf("symbols_received:      %llu\n",
           (unsigned long long)snapshot.symbols_received);
    printf("symbols_crc_dropped:   %llu\n",
           (unsigned long long)snapshot.symbols_crc_dropped);
    printf("symbols_lost_total:    %llu\n",
           (unsigned long long)snapshot.symbols_lost_total);
    printf("--- Packet Statistics ---\n");
    printf("packets_injected:      %llu\n",
           (unsigned long long)snapshot.packets_injected);
    printf("packets_recovered:     %llu\n",
           (unsigned long long)snapshot.packets_recovered);
    printf("packets_failed:        %llu\n",
           (unsigned long long)snapshot.packets_failed);
    printf("recovery_rate:         %.2f%%\n", snapshot.recovery_rate);
    printf("--- Burst Analysis ---\n");
    printf("total_burst_events:    %llu\n",
           (unsigned long long)snapshot.total_burst_events);
    printf("longest_burst_seen:    %llu symbols\n",
           (unsigned long long)snapshot.longest_burst_seen);
    printf("avg_burst_length:      %.2f symbols\n",
           snapshot.avg_burst_length);
    printf("--- Burst Histogram ---\n");
    printf("burst_len  count\n");

    if (pthread_mutex_lock((pthread_mutex_t *)&s->burst_mutex) != 0) {
        LOG_ERROR("[hw_stats] print_report: pthread_mutex_lock failed");
        return;
    }

    for (bucket = 1U; bucket <= snapshot.longest_burst_seen; ++bucket) {
        count = s->burst_histogram[bucket];
        if (count > 0U) {
            printf("%llu          %llu\n",
                   (unsigned long long)bucket,
                   (unsigned long long)count);
        }
    }

    if (pthread_mutex_unlock((pthread_mutex_t *)&s->burst_mutex) != 0) {
        LOG_ERROR("[hw_stats] print_report: pthread_mutex_unlock failed");
        return;
    }

    printf("===========================\n");
    fflush(stdout);
}

int hw_stats_save_csv(const hw_stats_t *s, const char *dirpath)
{
    hw_stats_snapshot_t snapshot;
    hw_stats_t         *mutable_stats;
    char                timestamp[32];
    char                stats_path[512];
    char                hist_path[512];
    FILE               *stats_fp;
    FILE               *hist_fp;
    int                 written;
    uint64_t            bucket;
    uint64_t            count;

    if (s == NULL || dirpath == NULL) {
        LOG_ERROR("[hw_stats] save_csv: invalid argument");
        return -1;
    }

    mutable_stats = (hw_stats_t *)s;
    hw_stats_finalize_running_burst(mutable_stats);
    hw_stats_compute_snapshot(s, &snapshot);

    if (hw_stats_make_timestamp(s, timestamp, sizeof(timestamp)) != 0) {
        LOG_ERROR("[hw_stats] save_csv: failed to build timestamp");
        return -1;
    }

    written = snprintf(stats_path,
                       sizeof(stats_path),
                       "%s/gateway_stats_%s.csv",
                       dirpath,
                       timestamp);
    if (written < 0 || (size_t)written >= sizeof(stats_path)) {
        LOG_ERROR("[hw_stats] save_csv: stats path truncated");
        return -1;
    }

    written = snprintf(hist_path,
                       sizeof(hist_path),
                       "%s/burst_histogram_%s.csv",
                       dirpath,
                       timestamp);
    if (written < 0 || (size_t)written >= sizeof(hist_path)) {
        LOG_ERROR("[hw_stats] save_csv: histogram path truncated");
        return -1;
    }

    stats_fp = fopen(stats_path, "w");
    if (stats_fp == NULL) {
        LOG_ERROR("[hw_stats] save_csv: fopen(\"%s\") failed: %s",
                  stats_path, strerror(errno));
        return -1;
    }

    if (fprintf(stats_fp,
                "duration_s,blocks_attempted,blocks_success,blocks_failed,"
                "blocks_too_many_holes,blocks_timeout,symbols_received,"
                "symbols_crc_dropped,symbols_lost,packets_injected,"
                "packets_recovered,packets_failed,recovery_rate,"
                "total_burst_events,longest_burst,avg_burst_length\n") < 0) {
        LOG_ERROR("[hw_stats] save_csv: failed to write header to \"%s\"",
                  stats_path);
        fclose(stats_fp);
        return -1;
    }

    if (fprintf(stats_fp,
                "%.2f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.2f,%llu,%llu,%.2f\n",
                snapshot.duration_s,
                (unsigned long long)snapshot.blocks_attempted,
                (unsigned long long)snapshot.blocks_decode_success,
                (unsigned long long)snapshot.blocks_decode_failed,
                (unsigned long long)snapshot.blocks_too_many_holes,
                (unsigned long long)snapshot.blocks_timeout,
                (unsigned long long)snapshot.symbols_received,
                (unsigned long long)snapshot.symbols_crc_dropped,
                (unsigned long long)snapshot.symbols_lost_total,
                (unsigned long long)snapshot.packets_injected,
                (unsigned long long)snapshot.packets_recovered,
                (unsigned long long)snapshot.packets_failed,
                snapshot.recovery_rate,
                (unsigned long long)snapshot.total_burst_events,
                (unsigned long long)snapshot.longest_burst_seen,
                snapshot.avg_burst_length) < 0) {
        LOG_ERROR("[hw_stats] save_csv: failed to write row to \"%s\"",
                  stats_path);
        fclose(stats_fp);
        return -1;
    }

    if (fclose(stats_fp) != 0) {
        LOG_ERROR("[hw_stats] save_csv: fclose(\"%s\") failed", stats_path);
        return -1;
    }

    hist_fp = fopen(hist_path, "w");
    if (hist_fp == NULL) {
        LOG_ERROR("[hw_stats] save_csv: fopen(\"%s\") failed: %s",
                  hist_path, strerror(errno));
        return -1;
    }

    if (fprintf(hist_fp, "burst_len,count\n") < 0) {
        LOG_ERROR("[hw_stats] save_csv: failed to write header to \"%s\"",
                  hist_path);
        fclose(hist_fp);
        return -1;
    }

    if (pthread_mutex_lock((pthread_mutex_t *)&s->burst_mutex) != 0) {
        LOG_ERROR("[hw_stats] save_csv: pthread_mutex_lock failed");
        fclose(hist_fp);
        return -1;
    }

    for (bucket = 1U; bucket <= snapshot.longest_burst_seen; ++bucket) {
        count = s->burst_histogram[bucket];
        if (count == 0U) {
            continue;
        }

        if (fprintf(hist_fp, "%llu,%llu\n",
                    (unsigned long long)bucket,
                    (unsigned long long)count) < 0) {
            if (pthread_mutex_unlock((pthread_mutex_t *)&s->burst_mutex) != 0) {
                LOG_ERROR("[hw_stats] save_csv: pthread_mutex_unlock failed");
            }
            LOG_ERROR("[hw_stats] save_csv: failed to write histogram row "
                      "to \"%s\"", hist_path);
            fclose(hist_fp);
            return -1;
        }
    }

    if (pthread_mutex_unlock((pthread_mutex_t *)&s->burst_mutex) != 0) {
        LOG_ERROR("[hw_stats] save_csv: pthread_mutex_unlock failed");
        fclose(hist_fp);
        return -1;
    }

    if (fclose(hist_fp) != 0) {
        LOG_ERROR("[hw_stats] save_csv: fclose(\"%s\") failed", hist_path);
        return -1;
    }

    return 0;
}

void hw_stats_print_live(const hw_stats_t *s, int elapsed_s)
{
    hw_stats_snapshot_t snapshot;

    if (s == NULL) {
        LOG_ERROR("[hw_stats] print_live: stats handle is NULL");
        return;
    }

    hw_stats_compute_snapshot(s, &snapshot);

    printf("[STATS %ds] blocks=%llu recovered=%llu lost=%llu longest_burst=%llu\n",
           elapsed_s,
           (unsigned long long)snapshot.blocks_attempted,
           (unsigned long long)snapshot.packets_recovered,
           (unsigned long long)snapshot.symbols_lost_total,
           (unsigned long long)snapshot.longest_burst_seen);
    fflush(stdout);
}