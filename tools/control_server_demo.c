/*
 * tools/control_server_demo.c — standalone demo of the control_server.
 *
 * Spins up the AF_UNIX telemetry server with synthetic stats activity, so
 * the FastAPI bridge (and the dashboard behind it) can be exercised
 * end-to-end on a developer machine without the actual fso_gw_runner
 * (which needs root + NICs).
 *
 * Run:
 *   ./build/bin/control_server_demo            # default /tmp/fso_gw.sock
 *   ./build/bin/control_server_demo /tmp/x.sock
 *
 * Stop with Ctrl+C.
 */

#include "config.h"
#include "control_server.h"
#include "logging.h"
#include "stats.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* Drive a believable counter trajectory so the dashboard has something to
 * display while we test. Calls the public stats inc/record API the same way
 * the real pipelines do — no internal poking. */
static void simulate_one_tick(void)
{
    /* Per-tick (~100ms) targets, very rough, scaled for visibility. */
    for (int i = 0; i < 5000; ++i) {
        size_t pkt_len = 200 + (rand() % 1300);
        stats_inc_ingress(pkt_len);
        stats_inc_transmitted(pkt_len);
    }
    for (int i = 0; i < 70000; ++i) {
        bool lost = (rand() % 10000) < 12;
        stats_record_symbol(lost);
    }
    for (int i = 0; i < 5000; ++i) {
        stats_inc_block_attempt();
        if ((rand() % 1000) < 998) {
            stats_inc_block_success();
        } else {
            stats_inc_block_failure();
            stats_record_block(2 + (rand() % 4));
        }
        if ((rand() % 100) < 8) {
            stats_inc_recovered(1200);
        }
    }
    if ((rand() % 50) == 0) stats_inc_crc_drop_symbol();
    if ((rand() % 200) == 0) stats_inc_failed_packet();
}

int main(int argc, char *argv[])
{
    const char *path = (argc > 1) ? argv[1] : NULL;

    log_set_level(INFO);
    LOG_INFO("control_server_demo: starting");

    stats_init();
    stats_set_burst_fec_span(8);

    /* Echo a representative config so the dashboard shows non-zero K/M/etc. */
    struct config cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.lan_iface, sizeof(cfg.lan_iface), "demo-lan");
    snprintf(cfg.fso_iface, sizeof(cfg.fso_iface), "demo-fso");
    cfg.k = 8;
    cfg.m = 4;
    cfg.depth = 16;
    cfg.symbol_size = 800;
    cfg.internal_symbol_crc_enabled = 1;

    struct control_server_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.socket_path = path;
    opts.tick_hz = 10;
    opts.gateway_cfg = &cfg;

    control_server_t *cs = control_server_start(&opts);
    if (!cs) {
        LOG_ERROR("control_server_demo: failed to start server");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    srand((unsigned)time(NULL));
    LOG_INFO("control_server_demo: simulating activity (Ctrl+C to stop)");

    while (!g_stop) {
        simulate_one_tick();
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    LOG_INFO("control_server_demo: shutting down");
    control_server_stop(cs);
    stats_finalize_burst();
    return 0;
}
