/*
 * tools/gateway_test.c — Full-duplex gateway validation tool for Task 26.
 *
 * Runs the full gateway for a fixed duration while:
 *   - An injector thread sends synthetic frames into NIC_LAN
 *   - An observer thread counts recovered packets on NIC_LAN
 *   - A timer thread calls gateway_stop() after duration seconds
 *
 * Usage:
 *   gateway_test --lan-iface <i> --fso-iface <i>
 *                [--duration <sec>] [--k N] [--m N]
 *                [--depth N] [--symbol-size N]
 *
 * Compile:
 *   gcc -std=c99 -Wall -Wextra -Iinclude \
 *       -o build/gateway_test \
 *       tools/gateway_test.c \
 *       src/gateway.c src/rx_pipeline.c src/tx_pipeline.c \
 *       src/packet_io.c src/logging.c src/config.c \
 *       src/block_builder.c src/fec_wrapper.c src/interleaver.c \
 *       src/deinterleaver.c src/packet_fragmenter.c \
 *       src/packet_reassembler.c src/symbol.c src/stats.c \
 *       third_party/wirehair/*.cpp \
 *       -lpcap -lpthread -lm -lstdc++ \
 *       -Ithird_party/wirehair/include
 */

#define _POSIX_C_SOURCE 200112L

#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "gateway.h"
#include "logging.h"
#include "packet_io.h"

/* -------------------------------------------------------------------------- */
/* Constants                                                                   */
/* -------------------------------------------------------------------------- */

#define ERRBUF_SIZE         256
#define RX_BUF_SIZE         9200

#define INJECT_PAYLOAD_LEN  100
#define INJECT_FRAME_LEN    114
#define INJECT_INTERVAL_US  1000    /* 1 ms between injected frames           */

#define OBS_MIN_FRAME_LEN   114
#define OBS_ETHERTYPE_HI    0x08
#define OBS_ETHERTYPE_LO    0x00
#define OBS_PAYLOAD_BYTE    0xCC

#define DEFAULT_DURATION    20
#define DEFAULT_K           8
#define DEFAULT_M           4
#define DEFAULT_DEPTH       2
#define DEFAULT_SYMBOL_SIZE 1500

/* -------------------------------------------------------------------------- */
/* Permitted globals                                                           */
/* -------------------------------------------------------------------------- */

static gateway_t              *g_gw            = NULL;
static volatile sig_atomic_t   inject_running  = 1;
static volatile sig_atomic_t   observe_running = 1;
static volatile sig_atomic_t   timer_running   = 1;

/* -------------------------------------------------------------------------- */
/* Signal handling                                                             */
/* -------------------------------------------------------------------------- */

static void sig_handler(int signum)
{
    (void)signum;
    if (g_gw != NULL) {
        gateway_stop(g_gw);
    }
    inject_running  = 0;
    observe_running = 0;
    timer_running   = 0;
}

/* -------------------------------------------------------------------------- */
/* Injector thread                                                             */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char    *lan_iface;
    unsigned long *inject_count;
} injector_args_t;

static void *injector_thread(void *arg)
{
    injector_args_t *a = (injector_args_t *)arg;
    char             errbuf[ERRBUF_SIZE];
    packet_io_ctx_t *ctx;
    unsigned char    frame[INJECT_FRAME_LEN];

    ctx = packet_io_open(a->lan_iface, 0, errbuf, sizeof(errbuf));
    if (ctx == NULL) {
        LOG_ERROR("[gateway_test] injector: failed to open \"%s\": %s",
                  a->lan_iface, errbuf);
        return NULL;
    }

    memset(frame, 0, sizeof(frame));
    /* dst MAC: ff:ff:ff:ff:ff:ff */
    memset(frame, 0xff, 6);
    /* src MAC: 02:00:00:00:00:BB */
    frame[6]  = 0x02;
    frame[7]  = 0x00;
    frame[8]  = 0x00;
    frame[9]  = 0x00;
    frame[10] = 0x00;
    frame[11] = 0xBB;
    /* EtherType: 0x0800 */
    frame[12] = OBS_ETHERTYPE_HI;
    frame[13] = OBS_ETHERTYPE_LO;
    /* Payload: 100 bytes of 0xCC */
    memset(frame + 14, OBS_PAYLOAD_BYTE, INJECT_PAYLOAD_LEN);

    while (inject_running) {
        if (packet_io_send(ctx, frame, INJECT_FRAME_LEN) == 0) {
            (*a->inject_count)++;
        } else {
            LOG_WARN("[gateway_test] injector: packet_io_send failed");
        }
        usleep(INJECT_INTERVAL_US);
    }

    packet_io_close(ctx);
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Observer thread                                                             */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char    *lan_iface;
    unsigned long *recovered_count;
} observer_args_t;

static void *observer_thread(void *arg)
{
    observer_args_t *a = (observer_args_t *)arg;
    char             errbuf[ERRBUF_SIZE];
    packet_io_ctx_t *ctx;
    unsigned char    buf[RX_BUF_SIZE];
    size_t           pkt_len;
    int              rc;

    ctx = packet_io_open(a->lan_iface, 1, errbuf, sizeof(errbuf));
    if (ctx == NULL) {
        LOG_ERROR("[gateway_test] observer: failed to open \"%s\": %s",
                  a->lan_iface, errbuf);
        return NULL;
    }

    while (observe_running) {
        pkt_len = 0;
        rc = packet_io_receive(ctx, buf, sizeof(buf), &pkt_len);

        if (rc == 1) {
            if (pkt_len >= (size_t)OBS_MIN_FRAME_LEN &&
                buf[12] == OBS_ETHERTYPE_HI &&
                buf[13] == OBS_ETHERTYPE_LO &&
                buf[14] == OBS_PAYLOAD_BYTE) {
                (*a->recovered_count)++;
            }
        } else if (rc == -1) {
            LOG_WARN("[gateway_test] observer: packet_io_receive error: %s",
                     packet_io_last_error(ctx));
        }
    }

    packet_io_close(ctx);
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Timer thread                                                                */
/* -------------------------------------------------------------------------- */

typedef struct {
    int duration;
} timer_args_t;

static void *timer_thread(void *arg)
{
    timer_args_t *a = (timer_args_t *)arg;

    sleep((unsigned int)a->duration);

    if (timer_running) {
        if (g_gw != NULL) {
            gateway_stop(g_gw);
        }
        inject_running  = 0;
        observe_running = 0;
    }

    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Usage                                                                       */
/* -------------------------------------------------------------------------- */

static void print_usage(void)
{
    fprintf(stderr,
            "Usage: gateway_test --lan-iface <i> --fso-iface <i>"
            " [--duration <sec>] [--k N] [--m N]"
            " [--depth N] [--symbol-size N]\n");
}

/* -------------------------------------------------------------------------- */
/* main                                                                        */
/* -------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    static const struct option long_opts[] = {
        { "lan-iface",   required_argument, NULL, 'l' },
        { "fso-iface",   required_argument, NULL, 'f' },
        { "duration",    required_argument, NULL, 'd' },
        { "k",           required_argument, NULL, 'k' },
        { "m",           required_argument, NULL, 'm' },
        { "depth",       required_argument, NULL, 'e' },
        { "symbol-size", required_argument, NULL, 's' },
        { NULL,          0,                 NULL,  0  }
    };

    const char     *lan_iface   = NULL;
    const char     *fso_iface   = NULL;
    int             duration    = DEFAULT_DURATION;
    int             k           = DEFAULT_K;
    int             m           = DEFAULT_M;
    int             depth       = DEFAULT_DEPTH;
    int             symbol_size = DEFAULT_SYMBOL_SIZE;
    int             opt;

    struct config    cfg;
    struct sigaction sa;

    pthread_t        inj_tid;
    pthread_t        obs_tid;
    pthread_t        tmr_tid;
    injector_args_t  inj_args;
    observer_args_t  obs_args;
    timer_args_t     tmr_args;
    unsigned long    inject_count;
    unsigned long    recovered_count;
    int              gw_rc;

    log_init();

    if (wirehair_init() != Wirehair_Success) {
        fprintf(stderr, "wirehair_init() failed\n");
        return 1;
    }

    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'l': lan_iface   = optarg;       break;
        case 'f': fso_iface   = optarg;       break;
        case 'd': duration    = atoi(optarg); break;
        case 'k': k           = atoi(optarg); break;
        case 'm': m           = atoi(optarg); break;
        case 'e': depth       = atoi(optarg); break;
        case 's': symbol_size = atoi(optarg); break;
        default:
            print_usage();
            return 1;
        }
    }

    if (lan_iface == NULL || fso_iface == NULL) {
        print_usage();
        return 1;
    }

    /* Build config */
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.lan_iface, lan_iface, sizeof(cfg.lan_iface) - 1);
    strncpy(cfg.fso_iface, fso_iface, sizeof(cfg.fso_iface) - 1);
    cfg.k                           = k;
    cfg.m                           = m;
    cfg.depth                       = depth;
    cfg.symbol_size                 = symbol_size;
    cfg.internal_symbol_crc_enabled = 1;

    /* Install signal handlers */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Create gateway */
    g_gw = gateway_create(&cfg);
    if (g_gw == NULL) {
        LOG_ERROR("[gateway_test] gateway_create failed");
        return 1;
    }

    /* Initialise shared counters */
    inject_count    = 0;
    recovered_count = 0;

    /* Start injector thread */
    inj_args.lan_iface     = lan_iface;
    inj_args.inject_count  = &inject_count;
    if (pthread_create(&inj_tid, NULL, injector_thread, &inj_args) != 0) {
        LOG_ERROR("[gateway_test] pthread_create(injector) failed");
        gateway_destroy(g_gw);
        g_gw = NULL;
        return 1;
    }

    /* Start observer thread */
    obs_args.lan_iface       = lan_iface;
    obs_args.recovered_count = &recovered_count;
    if (pthread_create(&obs_tid, NULL, observer_thread, &obs_args) != 0) {
        LOG_ERROR("[gateway_test] pthread_create(observer) failed");
        inject_running = 0;
        pthread_join(inj_tid, NULL);
        gateway_destroy(g_gw);
        g_gw = NULL;
        return 1;
    }

    /* Start timer thread */
    tmr_args.duration = duration;
    if (pthread_create(&tmr_tid, NULL, timer_thread, &tmr_args) != 0) {
        LOG_ERROR("[gateway_test] pthread_create(timer) failed");
        inject_running  = 0;
        observe_running = 0;
        pthread_join(obs_tid, NULL);
        pthread_join(inj_tid, NULL);
        gateway_destroy(g_gw);
        g_gw = NULL;
        return 1;
    }

    LOG_INFO("[gateway_test] Running for %d seconds "
             "(k=%d m=%d depth=%d sym=%d)",
             duration, k, m, depth, symbol_size);

    /* Run gateway — blocks until gateway_stop() is called */
    gw_rc = gateway_run(g_gw);

    /* Stop helper threads */
    timer_running   = 0;
    inject_running  = 0;
    observe_running = 0;

    pthread_join(tmr_tid, NULL);
    pthread_join(inj_tid, NULL);
    pthread_join(obs_tid, NULL);

    /* Tear down gateway */
    gateway_destroy(g_gw);
    g_gw = NULL;

    /* Print final stats */
    printf("gateway_test: injected=%lu recovered=%lu duration=%ds\n",
           inject_count, recovered_count, duration);

    if (recovered_count > 0 && gw_rc == 0) {
        printf("FULL DUPLEX TEST PASSED\n");
        return 0;
    } else {
        printf("FULL DUPLEX TEST FAILED: no packets recovered\n");
        return 1;
    }
}
