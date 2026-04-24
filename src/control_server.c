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
 */

#include "control_server.h"

#include "logging.h"
#include "stats.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
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
#define SNAPSHOT_BUF_SZ     8192

struct control_server {
    int                       listen_fd;
    char                      socket_path[108]; /* sun_path size */
    unsigned int              tick_hz;
    const struct config      *gateway_cfg;
    pthread_t                 worker;
    atomic_int                running;
    int                       worker_started;
    struct timespec           start_time;
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
    /* Interface names are alphanumeric + ':' + '.' + '-' in practice; just copy
     * with a defensive truncation. We are not parsing arbitrary user strings.
     */
    if (in == NULL) {
        if (out_sz > 0) out[0] = '\0';
        return;
    }
    snprintf(out, out_sz, "%s", in);
    /* Replace any double-quotes defensively to keep JSON well-formed. */
    for (char *p = out; *p; ++p) {
        if (*p == '"' || *p == '\\') *p = '_';
    }
}

/* -------------------------------------------------------------------------- */
/* Snapshot serialization                                                     */
/* -------------------------------------------------------------------------- */

static int build_snapshot_json(char *buf, size_t bufsz,
                               const struct control_server *cs)
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

    int n = snprintf(
        buf, bufsz,
        "{"
        "\"schema\":\"fso-gw-stats/1\","
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
        "}"
        "}\n",
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
    );

    if (n < 0 || (size_t)n >= bufsz) {
        return -1;
    }
    return n;
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
    char buf[SNAPSHOT_BUF_SZ];
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
        if (pr == 0) continue; /* timeout — re-check running flag */
        if (!(pfd.revents & POLLIN)) continue;

        int client_fd = accept(cs->listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (errno == EBADF || errno == EINVAL) break; /* listen fd closed */
            LOG_ERROR("control_server: accept failed: %s", strerror(errno));
            continue;
        }

        LOG_INFO("control_server: client connected");

        /* Per-tick send loop. Exit on client disconnect or shutdown. */
        struct timespec next;
        clock_gettime(CLOCK_MONOTONIC, &next);
        while (atomic_load(&cs->running)) {
            int n = build_snapshot_json(buf, sizeof(buf), cs);
            if (n < 0) {
                LOG_ERROR("control_server: snapshot too large for buffer");
                break;
            }
            if (write_all(client_fd, buf, (size_t)n) < 0) {
                LOG_INFO("control_server: client disconnected (%s)", strerror(errno));
                break;
            }

            /* Sleep until next tick; allow early wake for shutdown. */
            next.tv_nsec += tick_us * 1000L;
            while (next.tv_nsec >= 1000000000L) {
                next.tv_nsec -= 1000000000L;
                next.tv_sec += 1;
            }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        }

        close(client_fd);
    }

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
    snprintf(cs->socket_path, sizeof(cs->socket_path), "%s", path);
    clock_gettime(CLOCK_MONOTONIC, &cs->start_time);
    atomic_store(&cs->running, 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("control_server: socket() failed: %s", strerror(errno));
        free(cs);
        return NULL;
    }

    /* Remove stale socket from a previous run. unlink() may fail with ENOENT
     * which is fine; any other failure means we likely can't bind. */
    if (unlink(path) != 0 && errno != ENOENT) {
        LOG_ERROR("control_server: unlink(%s) failed: %s", path, strerror(errno));
        close(fd);
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
        free(cs);
        return NULL;
    }

    /* Permissive perms so the user-mode bridge can connect even when the
     * gateway runs as root. UNIX socket is filesystem-scoped, not network. */
    if (chmod(path, 0666) != 0) {
        LOG_ERROR("control_server: chmod(%s, 0666) failed: %s", path, strerror(errno));
        /* Non-fatal — listen anyway. */
    }

    if (listen(fd, 4) < 0) {
        LOG_ERROR("control_server: listen() failed: %s", strerror(errno));
        unlink(path);
        close(fd);
        free(cs);
        return NULL;
    }

    cs->listen_fd = fd;

    int rc = pthread_create(&cs->worker, NULL, worker_main, cs);
    if (rc != 0) {
        LOG_ERROR("control_server: pthread_create failed: %s", strerror(rc));
        atomic_store(&cs->running, 0);
        close(cs->listen_fd);
        unlink(path);
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

    /* Closing the listen fd also wakes accept() with EBADF on most kernels;
     * combined with the poll timeout, the worker exits within 200ms. */
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
    free(cs);
}
