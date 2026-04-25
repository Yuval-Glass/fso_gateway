/*
 * src/control_server.c — UNIX-domain telemetry exporter.
 *
 * Implementation notes:
 *   - One worker pthread.
 *   - Uses poll() with a 200ms timeout so the running flag is checked promptly
 *     during shutdown without burning CPU.
 *   - Single concurrent client (the FastAPI bridge). Additional connect
 *     attempts queue in the listen backlog.
 *   - JSON is built with snprintf into a stack buffer — no heap, no deps.
 *
 * Additional data exposed (v1.1 schema additions):
 *   - dil_stats: live deinterleaver counters (evictions, drops by reason,
 *     blocks failed by timeout vs too-many-holes, active/ready slot counts).
 *   - block_events: per-block lifecycle events pushed from the deinterleaver
 *     callbacks into a ring buffer, drained on each snapshot.
 *   - arp_cache: IP↔MAC table dump (learned peers for proxy-ARP).
 */

#include "control_server.h"

#include "arp_cache.h"
#include "deinterleaver.h"
#include "logging.h"
#include "stats.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SOCKET_PATH "/tmp/fso_gw.sock"
#define DEFAULT_TICK_HZ     10u
#define LISTEN_POLL_MS      200
#define SNAPSHOT_BUF_SZ     32768      /* room for arp + events + stats */
#define ARP_DUMP_MAX        32
#define EVENT_RING_CAP      256

struct block_event {
    uint32_t    block_id;
    int64_t     ts_ms;
    uint8_t     reason;      /* deinterleaver_block_final_reason_t cast */
    uint8_t     is_eviction; /* 1 for eviction-class callbacks */
};

struct control_server {
    int                       listen_fd;
    char                      socket_path[108]; /* sun_path size */
    unsigned int              tick_hz;
    const struct config      *gateway_cfg;
    deinterleaver_t          *dil;          /* optional, may be NULL */
    arp_cache_t              *arp_cache;    /* optional, may be NULL */
    pthread_t                 worker;
    atomic_int                running;
    int                       worker_started;
    struct timespec           start_time;

    /* Block lifecycle event ring — callbacks may fire from any RX thread. */
    struct block_event        events[EVENT_RING_CAP];
    int                       events_head;   /* next write slot */
    int                       events_count;  /* valid entries (<= CAP) */
    pthread_mutex_t           events_lock;
};

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static double elapsed_seconds(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double sec = (double)(now.tv_sec - start->tv_sec);
    sec += (double)(now.tv_nsec - start->tv_nsec) / 1e9;
    return sec;
}

static void escape_iface(char *out, size_t out_sz, const char *in)
{
    if (in == NULL) {
        if (out_sz > 0) out[0] = '\0';
        return;
    }
    snprintf(out, out_sz, "%s", in);
    for (char *p = out; *p; ++p) {
        if (*p == '"' || *p == '\\') *p = '_';
    }
}

static const char *reason_str(uint8_t r)
{
    switch ((deinterleaver_block_final_reason_t)r) {
        case DIL_BLOCK_FINAL_NONE:                                 return "NONE";
        case DIL_BLOCK_FINAL_DECODE_SUCCESS:                       return "SUCCESS";
        case DIL_BLOCK_FINAL_DECODE_FAILED:                        return "DECODE_FAILED";
        case DIL_BLOCK_FINAL_DISCARDED_TIMEOUT_BEFORE_DECODE:      return "TIMEOUT";
        case DIL_BLOCK_FINAL_DISCARDED_TOO_MANY_HOLES_BEFORE_DECODE: return "TOO_MANY_HOLES";
        case DIL_BLOCK_FINAL_DISCARDED_EVICTED_BEFORE_DECODE:      return "EVICTED_FILLING";
        case DIL_BLOCK_FINAL_DISCARDED_READY_EVICTED_BEFORE_MARK:  return "EVICTED_READY";
        default:                                                    return "UNKNOWN";
    }
}

/* -------------------------------------------------------------------------- */
/* Block lifecycle event ring                                                 */
/* -------------------------------------------------------------------------- */

static void events_push(struct control_server *cs, uint32_t block_id,
                        uint8_t reason, uint8_t is_eviction)
{
    pthread_mutex_lock(&cs->events_lock);
    cs->events[cs->events_head].block_id    = block_id;
    cs->events[cs->events_head].ts_ms       = now_ms();
    cs->events[cs->events_head].reason      = reason;
    cs->events[cs->events_head].is_eviction = is_eviction;
    cs->events_head = (cs->events_head + 1) % EVENT_RING_CAP;
    if (cs->events_count < EVENT_RING_CAP) {
        cs->events_count++;
    }
    pthread_mutex_unlock(&cs->events_lock);
}

/* Drain up to max events into out[] in chronological (oldest-first) order.
 * Returns the count written. The ring is emptied. */
static int events_drain(struct control_server *cs, struct block_event *out, int max)
{
    int written = 0;
    pthread_mutex_lock(&cs->events_lock);
    int count = cs->events_count;
    if (count > max) count = max;
    int start = (cs->events_head - cs->events_count + EVENT_RING_CAP) % EVENT_RING_CAP;
    for (int i = 0; i < count; ++i) {
        out[written++] = cs->events[(start + i) % EVENT_RING_CAP];
    }
    cs->events_count = 0;
    pthread_mutex_unlock(&cs->events_lock);
    return written;
}

static void on_block_final_cb(uint32_t block_id,
                              deinterleaver_block_final_reason_t reason,
                              void *user)
{
    struct control_server *cs = (struct control_server *)user;
    events_push(cs, block_id, (uint8_t)reason, 0);
}

static void on_eviction_cb(uint32_t evicted_block_id,
                           deinterleaver_block_final_reason_t reason,
                           const dil_eviction_info_t *info,
                           void *user)
{
    (void)info;  /* snapshot details available for future deeper diagnostics */
    struct control_server *cs = (struct control_server *)user;
    events_push(cs, evicted_block_id, (uint8_t)reason, 1);
}

/* -------------------------------------------------------------------------- */
/* JSON fragment helpers                                                      */
/* -------------------------------------------------------------------------- */

/* Append to buf; returns number of bytes written, -1 on overflow. */
static int append_fmt(char *buf, size_t bufsz, size_t *pos, const char *fmt, ...)
{
    if (*pos >= bufsz) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, bufsz - *pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= bufsz - *pos) return -1;
    *pos += (size_t)n;
    return n;
}

/* -------------------------------------------------------------------------- */
/* Snapshot serialization                                                     */
/* -------------------------------------------------------------------------- */

static int build_snapshot_json(char *buf, size_t bufsz,
                               struct control_server *cs)
{
    struct stats_container s;
    memset(&s, 0, sizeof(s));
    stats_snapshot(&s);

    char lan[32] = "";
    char fso[32] = "";
    int  k = 0, m = 0, depth = 0, symbol_size = 0, crc = 0;
    if (cs->gateway_cfg != NULL) {
        escape_iface(lan, sizeof(lan), cs->gateway_cfg->lan_iface);
        escape_iface(fso, sizeof(fso), cs->gateway_cfg->fso_iface);
        k           = cs->gateway_cfg->k;
        m           = cs->gateway_cfg->m;
        depth       = cs->gateway_cfg->depth;
        symbol_size = cs->gateway_cfg->symbol_size;
        crc         = cs->gateway_cfg->internal_symbol_crc_enabled;
    }

    size_t pos = 0;

    if (append_fmt(buf, bufsz, &pos,
        "{"
        "\"schema\":\"fso-gw-stats/2\","
        "\"ts_ms\":%" PRId64 ","
        "\"uptime_sec\":%.3f,"
        "\"config\":{"
            "\"k\":%d,\"m\":%d,\"depth\":%d,\"symbol_size\":%d,"
            "\"internal_symbol_crc\":%d,"
            "\"lan_iface\":\"%s\",\"fso_iface\":\"%s\""
        "},"
        "\"stats\":{"
            "\"ingress_packets\":%" PRIu64 ","
            "\"ingress_bytes\":%" PRIu64 ","
            "\"transmitted_packets\":%" PRIu64 ","
            "\"transmitted_bytes\":%" PRIu64 ","
            "\"recovered_packets\":%" PRIu64 ","
            "\"recovered_bytes\":%" PRIu64 ","
            "\"failed_packets\":%" PRIu64 ","
            "\"total_symbols\":%" PRIu64 ","
            "\"lost_symbols\":%" PRIu64 ","
            "\"symbols_dropped_crc\":%" PRIu64 ","
            "\"packet_fail_crc_drop\":%" PRIu64 ","
            "\"blocks_attempted\":%" PRIu64 ","
            "\"blocks_recovered\":%" PRIu64 ","
            "\"blocks_failed\":%" PRIu64 ","
            "\"total_bursts\":%" PRIu64 ","
            "\"max_burst_length\":%" PRIu64 ","
            "\"sum_burst_lengths\":%" PRIu64 ","
            "\"burst_len_1\":%" PRIu64 ","
            "\"burst_len_2_5\":%" PRIu64 ","
            "\"burst_len_6_10\":%" PRIu64 ","
            "\"burst_len_11_50\":%" PRIu64 ","
            "\"burst_len_51_100\":%" PRIu64 ","
            "\"burst_len_101_500\":%" PRIu64 ","
            "\"burst_len_501_plus\":%" PRIu64 ","
            "\"bursts_exceeding_fec_span\":%" PRIu64 ","
            "\"configured_fec_burst_span\":%" PRIu64 ","
            "\"recoverable_bursts\":%" PRIu64 ","
            "\"critical_bursts\":%" PRIu64 ","
            "\"blocks_with_loss\":%" PRIu64 ","
            "\"worst_holes_in_block\":%" PRIu64 ","
            "\"total_holes_in_blocks\":%" PRIu64
        "}",
        now_ms(), elapsed_seconds(&cs->start_time),
        k, m, depth, symbol_size, crc, lan, fso,
        s.ingress_packets, s.ingress_bytes,
        s.transmitted_packets, s.transmitted_bytes,
        s.recovered_packets, s.recovered_bytes, s.failed_packets,
        s.total_symbols, s.lost_symbols,
        s.symbols_dropped_crc, s.packet_fail_crc_drop,
        s.blocks_attempted, s.blocks_recovered, s.blocks_failed,
        s.total_bursts, s.max_burst_length, s.sum_burst_lengths,
        s.burst_len_1, s.burst_len_2_5, s.burst_len_6_10,
        s.burst_len_11_50, s.burst_len_51_100,
        s.burst_len_101_500, s.burst_len_501_plus,
        s.bursts_exceeding_fec_span, s.configured_fec_burst_span,
        s.recoverable_bursts, s.critical_bursts,
        s.blocks_with_loss, s.worst_holes_in_block, s.total_holes_in_blocks
    ) < 0) return -1;

    /* ---- dil_stats (optional) ------------------------------------------ */
    if (cs->dil) {
        dil_stats_t ds;
        memset(&ds, 0, sizeof(ds));
        deinterleaver_get_stats(cs->dil, &ds);
        int active = deinterleaver_active_blocks(cs->dil);
        int ready  = deinterleaver_ready_count(cs->dil);

        if (append_fmt(buf, bufsz, &pos,
            ","
            "\"dil_stats\":{"
                "\"dropped_duplicate\":%" PRIu64 ","
                "\"dropped_frozen\":%" PRIu64 ","
                "\"dropped_erasure\":%" PRIu64 ","
                "\"dropped_crc_fail\":%" PRIu64 ","
                "\"evicted_filling\":%" PRIu64 ","
                "\"evicted_done\":%" PRIu64 ","
                "\"blocks_ready\":%" PRIu64 ","
                "\"blocks_failed_timeout\":%" PRIu64 ","
                "\"blocks_failed_holes\":%" PRIu64 ","
                "\"active_blocks\":%d,"
                "\"ready_count\":%d"
            "}",
            ds.dropped_symbols_duplicate, ds.dropped_symbols_frozen,
            ds.dropped_symbols_erasure, ds.dropped_symbols_crc_fail,
            ds.evicted_filling_blocks, ds.evicted_done_blocks,
            ds.blocks_ready, ds.blocks_failed_timeout, ds.blocks_failed_holes,
            active, ready
        ) < 0) return -1;
    }

    /* ---- arp_cache dump (optional) ------------------------------------- */
    if (cs->arp_cache) {
        struct arp_entry arp[ARP_DUMP_MAX];
        int n_arp = arp_cache_dump(cs->arp_cache, arp, ARP_DUMP_MAX);

        if (append_fmt(buf, bufsz, &pos, ",\"arp\":[") < 0) return -1;
        for (int i = 0; i < n_arp; ++i) {
            uint32_t ip = arp[i].ip_nbo;
            if (append_fmt(buf, bufsz, &pos,
                "%s{\"ip\":\"%u.%u.%u.%u\","
                "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
                "\"last_seen_ms\":%" PRId64 "}",
                i == 0 ? "" : ",",
                (ip >> 0) & 0xff, (ip >> 8) & 0xff,
                (ip >> 16) & 0xff, (ip >> 24) & 0xff,
                arp[i].mac[0], arp[i].mac[1], arp[i].mac[2],
                arp[i].mac[3], arp[i].mac[4], arp[i].mac[5],
                arp[i].last_seen_ms
            ) < 0) return -1;
        }
        if (append_fmt(buf, bufsz, &pos, "]") < 0) return -1;
    }

    /* ---- block events drained from ring --------------------------------- */
    {
        struct block_event evs[EVENT_RING_CAP];
        int n_ev = events_drain(cs, evs, EVENT_RING_CAP);

        if (append_fmt(buf, bufsz, &pos, ",\"block_events\":[") < 0) return -1;
        for (int i = 0; i < n_ev; ++i) {
            if (append_fmt(buf, bufsz, &pos,
                "%s{\"block_id\":%u,\"ts_ms\":%" PRId64 ","
                "\"reason\":\"%s\",\"evicted\":%d}",
                i == 0 ? "" : ",",
                evs[i].block_id, evs[i].ts_ms,
                reason_str(evs[i].reason), evs[i].is_eviction
            ) < 0) return -1;
        }
        if (append_fmt(buf, bufsz, &pos, "]") < 0) return -1;
    }

    if (append_fmt(buf, bufsz, &pos, "}\n") < 0) return -1;

    return (int)pos;
}

/* -------------------------------------------------------------------------- */
/* I/O helpers                                                                */
/* -------------------------------------------------------------------------- */

static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Worker thread                                                              */
/* -------------------------------------------------------------------------- */

static void *worker_main(void *arg)
{
    struct control_server *cs = (struct control_server *)arg;
    char *buf = (char *)malloc(SNAPSHOT_BUF_SZ);
    if (!buf) {
        LOG_ERROR("control_server: worker malloc failed");
        return NULL;
    }
    const long tick_us = 1000000L / (long)cs->tick_hz;

    LOG_INFO("control_server: listening on %s (tick=%uHz)",
             cs->socket_path, cs->tick_hz);

    while (atomic_load(&cs->running)) {
        struct pollfd pfd = { .fd = cs->listen_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, LISTEN_POLL_MS);
        if (pr < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("control_server: poll(listen) failed: %s", strerror(errno));
            break;
        }
        if (pr == 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        int client_fd = accept(cs->listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (errno == EBADF || errno == EINVAL) break;
            LOG_ERROR("control_server: accept failed: %s", strerror(errno));
            continue;
        }

        LOG_INFO("control_server: client connected");

        struct timespec next;
        clock_gettime(CLOCK_MONOTONIC, &next);
        while (atomic_load(&cs->running)) {
            int n = build_snapshot_json(buf, SNAPSHOT_BUF_SZ, cs);
            if (n < 0) {
                LOG_ERROR("control_server: snapshot too large for buffer");
                break;
            }
            if (write_all(client_fd, buf, (size_t)n) < 0) {
                LOG_INFO("control_server: client disconnected (%s)", strerror(errno));
                break;
            }

            next.tv_nsec += tick_us * 1000L;
            while (next.tv_nsec >= 1000000000L) {
                next.tv_nsec -= 1000000000L;
                next.tv_sec += 1;
            }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        }

        close(client_fd);
    }

    free(buf);
    LOG_INFO("control_server: worker exiting");
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

control_server_t *control_server_start(const struct control_server_options *opts)
{
    const char *path = (opts && opts->socket_path) ? opts->socket_path : DEFAULT_SOCKET_PATH;
    unsigned int hz   = (opts && opts->tick_hz)     ? opts->tick_hz     : DEFAULT_TICK_HZ;

    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        LOG_ERROR("control_server: socket path too long: %s", path);
        errno = ENAMETOOLONG;
        return NULL;
    }

    control_server_t *cs = (control_server_t *)calloc(1, sizeof(*cs));
    if (!cs) {
        LOG_ERROR("control_server: out of memory");
        return NULL;
    }
    cs->listen_fd = -1;
    cs->tick_hz = hz;
    cs->gateway_cfg = opts ? opts->gateway_cfg : NULL;
    cs->dil         = opts ? opts->dil         : NULL;
    cs->arp_cache   = opts ? opts->arp_cache   : NULL;
    snprintf(cs->socket_path, sizeof(cs->socket_path), "%s", path);
    clock_gettime(CLOCK_MONOTONIC, &cs->start_time);
    pthread_mutex_init(&cs->events_lock, NULL);
    atomic_store(&cs->running, 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("control_server: socket() failed: %s", strerror(errno));
        pthread_mutex_destroy(&cs->events_lock);
        free(cs);
        return NULL;
    }

    if (unlink(path) != 0 && errno != ENOENT) {
        LOG_ERROR("control_server: unlink(%s) failed: %s", path, strerror(errno));
        close(fd);
        pthread_mutex_destroy(&cs->events_lock);
        free(cs);
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("control_server: bind(%s) failed: %s", path, strerror(errno));
        close(fd);
        pthread_mutex_destroy(&cs->events_lock);
        free(cs);
        return NULL;
    }

    if (chmod(path, 0666) != 0) {
        LOG_ERROR("control_server: chmod(%s, 0666) failed: %s", path, strerror(errno));
    }

    if (listen(fd, 4) < 0) {
        LOG_ERROR("control_server: listen() failed: %s", strerror(errno));
        unlink(path);
        close(fd);
        pthread_mutex_destroy(&cs->events_lock);
        free(cs);
        return NULL;
    }

    cs->listen_fd = fd;

    /* Register deinterleaver callbacks now that the context is stable. */
    if (cs->dil) {
        deinterleaver_set_block_final_callback(cs->dil, on_block_final_cb, cs);
        deinterleaver_set_eviction_callback   (cs->dil, on_eviction_cb,    cs);
    }

    int rc = pthread_create(&cs->worker, NULL, worker_main, cs);
    if (rc != 0) {
        LOG_ERROR("control_server: pthread_create failed: %s", strerror(rc));
        atomic_store(&cs->running, 0);
        close(cs->listen_fd);
        unlink(path);
        pthread_mutex_destroy(&cs->events_lock);
        free(cs);
        errno = rc;
        return NULL;
    }
    cs->worker_started = 1;
    return cs;
}

void control_server_stop(control_server_t *cs)
{
    if (!cs) return;

    atomic_store(&cs->running, 0);

    /* Unregister deinterleaver callbacks before we start tearing down so the
     * RX thread cannot call into freed memory. */
    if (cs->dil) {
        deinterleaver_set_block_final_callback(cs->dil, NULL, NULL);
        deinterleaver_set_eviction_callback   (cs->dil, NULL, NULL);
    }

    if (cs->listen_fd >= 0) {
        shutdown(cs->listen_fd, SHUT_RDWR);
        close(cs->listen_fd);
        cs->listen_fd = -1;
    }

    if (cs->worker_started) {
        pthread_join(cs->worker, NULL);
    }

    if (cs->socket_path[0]) {
        unlink(cs->socket_path);
    }
    pthread_mutex_destroy(&cs->events_lock);
    free(cs);
}
