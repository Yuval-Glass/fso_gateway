/*
 * tools/control_server_demo.c — standalone demo of the control_server.
 *
 * Spins up the AF_UNIX telemetry server with synthetic activity so the
 * FastAPI bridge (and the dashboard behind it) can be exercised end-to-end
 * without the actual fso_gw_runner (which needs root + NICs).
 *
 * Exercises every extension the server exposes:
 *   - stats_container counters (ingress/transmitted/blocks/bursts/etc.)
 *   - deinterleaver stats + block lifecycle callbacks (real deinterleaver
 *     driven with synthetic symbols so evictions and decode outcomes occur)
 *   - arp_cache (learns a few peers each tick so the Topology ARP panel
 *     has content in demo mode)
 *
 * Drop-in compatibility with fso_gw_runner CLI
 * --------------------------------------------
 * Accepts the same long options as fso_gw_runner so the bridge's
 * DaemonSupervisor can invoke this binary instead of the real runner
 * for local lifecycle testing. The interface flags are accepted but
 * ignored (no real NIC is opened); --k / --m / --depth / --symbol-size
 * are echoed in the config snapshot so the dashboard reflects them.
 *
 * Examples:
 *   ./build/bin/control_server_demo
 *   ./build/bin/control_server_demo --socket-path /tmp/x.sock
 *   ./build/bin/control_server_demo --lan-iface eth0 --fso-iface eth1 \
 *                                   --k 8 --m 4 --depth 2 --symbol-size 1500
 *
 * Stop with Ctrl+C.
 */

#include "arp_cache.h"
#include "config.h"
#include "control_server.h"
#include "deinterleaver.h"
#include "logging.h"
#include "stats.h"

#include <arpa/inet.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEMO_K            8
#define DEMO_M            4
#define DEMO_DEPTH        16
#define DEMO_SYMBOL_SIZE  800
#define DEMO_N            (DEMO_K + DEMO_M)

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void simulate_stats_tick(void)
{
    /* Per-tick (~100 ms) synthetic activity. */
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

static uint64_t g_block_id = 0;

/* Push a block's worth of symbols into the deinterleaver with random losses
 * so its FSM actually moves through EMPTY → FILLING → READY_TO_DECODE and the
 * block_final_callback fires. */
static void drive_deinterleaver(deinterleaver_t *dil, int n, int m, int symbol_size)
{
    symbol_t sym;
    g_block_id++;

    /* Lose 0..min(2,m) symbols per block (stay within recovery budget). */
    int max_loss = m < 2 ? m : 2;
    int losses = max_loss > 0 ? rand() % (max_loss + 1) : 0;
    int lost_bitmap[64] = {0};
    if (n > 64) n = 64;
    for (int i = 0; i < losses; ++i) {
        lost_bitmap[rand() % n] = 1;
    }

    for (int fec_id = 0; fec_id < n; ++fec_id) {
        if (lost_bitmap[fec_id]) continue;
        memset(&sym, 0, sizeof(sym));
        sym.packet_id     = (uint32_t)g_block_id;
        sym.fec_id        = (uint32_t)fec_id;
        sym.symbol_index  = (uint16_t)fec_id;
        sym.total_symbols = (uint16_t)n;
        sym.payload_len   = (uint16_t)symbol_size;
        deinterleaver_push_symbol(dil, &sym);
    }

    /* Drain any ready blocks — this triggers the final_callback. */
    block_t blk;
    while (deinterleaver_get_ready_block(dil, &blk) == 0) {
        deinterleaver_mark_result(dil, (uint32_t)blk.block_id, 1);
    }
    deinterleaver_tick(dil, -1.0);
}

/* Teach the ARP cache about the Phase 8 peers so the Topology panel has
 * content in demo mode. */
static void seed_arp_cache(arp_cache_t *arp)
{
    /* Win-1 @ 192.168.50.1 → 90:2e:16:d6:96:ba */
    uint32_t ip1 = inet_addr("192.168.50.1");
    uint8_t  mac1[6] = { 0x90, 0x2e, 0x16, 0xd6, 0x96, 0xba };
    arp_cache_learn(arp, ip1, mac1);

    /* Win-2 @ 192.168.50.2 → c4:ef:bb:5f:cd:5c */
    uint32_t ip2 = inet_addr("192.168.50.2");
    uint8_t  mac2[6] = { 0xc4, 0xef, 0xbb, 0x5f, 0xcd, 0x5c };
    arp_cache_learn(arp, ip2, mac2);
}

int main(int argc, char *argv[])
{
    const char *path = NULL;

    /* Defaults — overridden by CLI args. */
    int k = DEMO_K;
    int m = DEMO_M;
    int depth = DEMO_DEPTH;
    int symbol_size = DEMO_SYMBOL_SIZE;
    char lan_iface[32] = "demo-lan";
    char fso_iface[32] = "demo-fso";

    static const struct option long_opts[] = {
        { "lan-iface",            required_argument, NULL, 'l' },
        { "fso-iface",            required_argument, NULL, 'f' },
        { "k",                    required_argument, NULL, 'k' },
        { "m",                    required_argument, NULL, 'm' },
        { "depth",                required_argument, NULL, 'e' },
        { "symbol-size",          required_argument, NULL, 's' },
        { "socket-path",          required_argument, NULL, 'p' },
        { "internal-symbol-crc",  required_argument, NULL, 'c' },
        { "duration",             required_argument, NULL, 'd' },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    /* getopt_long allows us to be tolerant of flag variations the runner uses. */
    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'l': snprintf(lan_iface, sizeof(lan_iface), "%s", optarg); break;
            case 'f': snprintf(fso_iface, sizeof(fso_iface), "%s", optarg); break;
            case 'k': k           = atoi(optarg); break;
            case 'm': m           = atoi(optarg); break;
            case 'e': depth       = atoi(optarg); break;
            case 's': symbol_size = atoi(optarg); break;
            case 'p': path        = optarg;       break;
            case 'c': /* accepted but always-on in the demo */         break;
            case 'd': /* accepted but ignored in the demo */            break;
            case '?': /* unknown flag — keep going for forward compat */ break;
            default: break;
        }
    }
    if (path == NULL && optind < argc && argv[optind][0] == '/') {
        path = argv[optind];
    }

    if (k <= 0) k = DEMO_K;
    if (m <  0) m = DEMO_M;
    if (depth <= 0) depth = DEMO_DEPTH;
    if (symbol_size <= 0) symbol_size = DEMO_SYMBOL_SIZE;

    log_set_level(INFO);
    LOG_INFO("control_server_demo: starting (k=%d m=%d depth=%d sym=%d lan=%s fso=%s)",
             k, m, depth, symbol_size, lan_iface, fso_iface);

    stats_init();
    stats_set_burst_fec_span((uint64_t)m * (uint64_t)depth);

    struct config cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.lan_iface, sizeof(cfg.lan_iface), "%s", lan_iface);
    snprintf(cfg.fso_iface, sizeof(cfg.fso_iface), "%s", fso_iface);
    cfg.k = k;
    cfg.m = m;
    cfg.depth = depth;
    cfg.symbol_size = symbol_size;
    cfg.internal_symbol_crc_enabled = 1;

    /* Real deinterleaver so dil_stats + block_events are live in demo mode. */
    deinterleaver_t *dil = deinterleaver_create(
        depth * 4, k + m, k, symbol_size, 0.0, 50.0);
    if (!dil) {
        LOG_ERROR("control_server_demo: deinterleaver_create failed");
        return 1;
    }

    arp_cache_t *arp = arp_cache_create();
    if (!arp) {
        LOG_ERROR("control_server_demo: arp_cache_create failed");
        deinterleaver_destroy(dil);
        return 1;
    }
    seed_arp_cache(arp);

    struct control_server_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.socket_path = path;
    opts.tick_hz = 10;
    opts.gateway_cfg = &cfg;
    opts.dil = dil;
    opts.arp_cache = arp;

    control_server_t *cs = control_server_start(&opts);
    if (!cs) {
        LOG_ERROR("control_server_demo: failed to start server");
        arp_cache_destroy(arp);
        deinterleaver_destroy(dil);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    srand((unsigned)time(NULL));
    LOG_INFO("control_server_demo: simulating activity (Ctrl+C to stop)");

    int ticks = 0;
    while (!g_stop) {
        simulate_stats_tick();
        /* Drive ~50 blocks through the deinterleaver per tick. */
        for (int i = 0; i < 50; ++i) {
            drive_deinterleaver(dil, k + m, m, symbol_size);
        }
        /* Refresh ARP entries periodically so they don't time out (TTL=5min). */
        if ((ticks++ % 50) == 0) {
            seed_arp_cache(arp);
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    LOG_INFO("control_server_demo: shutting down");
    control_server_stop(cs);
    stats_finalize_burst();
    arp_cache_destroy(arp);
    deinterleaver_destroy(dil);
    return 0;
}
