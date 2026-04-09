/*
 * tools/rx_pipeline_test.c — RX pipeline validation tool for FSO Gateway Task 25.
 *
 * Runs in one of three modes:
 *   TX  — injects synthetic frames into NIC_LAN, runs TX pipeline,
 *          transmits FEC symbols out of NIC_FSO.
 *   RX  — receives symbols on NIC_FSO, runs RX pipeline,
 *          forwards reconstructed packets to NIC_LAN.
 *   OBS — observes NIC_LAN and counts recovered packets.
 *
 * Usage:
 *   rx_pipeline_test --lan-iface <i> --fso-iface <i> --mode tx|rx|obs
 *                    [--duration <sec>] [--k N] [--m N]
 *                    [--depth N] [--symbol-size N]
 *
 * Compile:
 *   gcc -std=c99 -Wall -Wextra -Iinclude \
 *       -o build/rx_pipeline_test \
 *       tools/rx_pipeline_test.c \
 *       src/rx_pipeline.c src/tx_pipeline.c src/packet_io.c \
 *       src/logging.c src/config.c src/block_builder.c \
 *       src/fec_wrapper.c src/interleaver.c src/deinterleaver.c \
 *       src/packet_fragmenter.c src/packet_reassembler.c \
 *       src/symbol.c src/stats.c \
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
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "logging.h"
#include "packet_io.h"
#include "rx_pipeline.h"
#include "tx_pipeline.h"

/* -------------------------------------------------------------------------- */
/* Constants                                                                   */
/* -------------------------------------------------------------------------- */

#define ERRBUF_SIZE         256
#define RX_BUF_SIZE         9200

/* Synthetic inject frame: 14-byte Eth hdr + 100-byte payload = 114 bytes */
#define INJECT_PAYLOAD_LEN  100
#define INJECT_FRAME_LEN    114
#define INJECT_INTERVAL_US  1000    /* 1 ms between injected frames           */

#define OBS_PROGRESS_EVERY  50      /* print progress every N recovered pkts  */
#define OBS_MIN_FRAME_LEN   114
#define OBS_ETHERTYPE_HI    0x08
#define OBS_ETHERTYPE_LO    0x00
#define OBS_PAYLOAD_BYTE    0xAB

/* Default CLI values */
#define DEFAULT_DURATION    15
#define DEFAULT_K           8
#define DEFAULT_M           4
#define DEFAULT_DEPTH       2
#define DEFAULT_SYMBOL_SIZE 1500

/* -------------------------------------------------------------------------- */
/* Signal handling                                                             */
/* -------------------------------------------------------------------------- */

static volatile sig_atomic_t running        = 1;
static volatile sig_atomic_t inject_running = 1;

static void sig_handler(int signum)
{
    (void)signum;
    running        = 0;
    inject_running = 0;
}

/* -------------------------------------------------------------------------- */
/* Injector thread                                                             */
/* -------------------------------------------------------------------------- */

typedef struct {
    packet_io_ctx_t *ctx_lan;
    unsigned long   *inject_count;
} injector_args_t;

static void *injector_thread(void *arg)
{
    injector_args_t *a = (injector_args_t *)arg;
    unsigned char    frame[INJECT_FRAME_LEN];

    memset(frame, 0, sizeof(frame));

    /* dst MAC: ff:ff:ff:ff:ff:ff */
    memset(frame, 0xff, 6);
    /* src MAC: 02:00:00:00:00:AA */
    frame[6]  = 0x02;
    frame[7]  = 0x00;
    frame[8]  = 0x00;
    frame[9]  = 0x00;
    frame[10] = 0x00;
    frame[11] = 0xAA;
    /* EtherType: 0x0800 */
    frame[12] = OBS_ETHERTYPE_HI;
    frame[13] = OBS_ETHERTYPE_LO;
    /* Payload: 100 bytes of 0xAB */
    memset(frame + 14, OBS_PAYLOAD_BYTE, INJECT_PAYLOAD_LEN);

    while (inject_running) {
        if (packet_io_send(a->ctx_lan, frame, INJECT_FRAME_LEN) == 0) {
            (*a->inject_count)++;
        } else {
            LOG_WARN("[rx_pipeline_test] injector: packet_io_send failed");
        }
        usleep(INJECT_INTERVAL_US);
    }

    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Usage                                                                       */
/* -------------------------------------------------------------------------- */

static void print_usage(void)
{
    fprintf(stderr,
            "Usage: rx_pipeline_test --lan-iface <i> --fso-iface <i>"
            " --mode tx|rx|obs [--duration <sec>] [--k N] [--m N]"
            " [--depth N] [--symbol-size N]\n");
}

/* -------------------------------------------------------------------------- */
/* TX mode                                                                     */
/* -------------------------------------------------------------------------- */

static int run_tx(const char *lan_iface,
                  const char *fso_iface,
                  int         duration,
                  int         k,
                  int         m,
                  int         depth,
                  int         symbol_size)
{
    struct config    cfg;
    char             errbuf[ERRBUF_SIZE];
    packet_io_ctx_t *ctx_lan;
    packet_io_ctx_t *ctx_fso;
    tx_pipeline_t   *pl;
    pthread_t        inj_tid;
    injector_args_t  inj_args;
    unsigned long    inject_count;
    time_t           t_start;
    int              rc;

    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.lan_iface, lan_iface, sizeof(cfg.lan_iface) - 1);
    strncpy(cfg.fso_iface, fso_iface, sizeof(cfg.fso_iface) - 1);
    cfg.k                           = k;
    cfg.m                           = m;
    cfg.depth                       = depth;
    cfg.symbol_size                 = symbol_size;
    cfg.internal_symbol_crc_enabled = 1;

    ctx_lan = packet_io_open(lan_iface, 1, errbuf, sizeof(errbuf));
    if (ctx_lan == NULL) {
        LOG_ERROR("[rx_pipeline_test] TX: failed to open LAN \"%s\": %s",
                  lan_iface, errbuf);
        return 1;
    }

    ctx_fso = packet_io_open(fso_iface, 0, errbuf, sizeof(errbuf));
    if (ctx_fso == NULL) {
        LOG_ERROR("[rx_pipeline_test] TX: failed to open FSO \"%s\": %s",
                  fso_iface, errbuf);
        packet_io_close(ctx_lan);
        return 1;
    }

    pl = tx_pipeline_create(&cfg, ctx_lan, ctx_fso);
    if (pl == NULL) {
        LOG_ERROR("[rx_pipeline_test] TX: tx_pipeline_create failed");
        packet_io_close(ctx_fso);
        packet_io_close(ctx_lan);
        return 1;
    }

    inject_count        = 0;
    inj_args.ctx_lan    = ctx_lan;
    inj_args.inject_count = &inject_count;

    if (pthread_create(&inj_tid, NULL, injector_thread, &inj_args) != 0) {
        LOG_ERROR("[rx_pipeline_test] TX: pthread_create failed");
        tx_pipeline_destroy(pl);
        packet_io_close(ctx_fso);
        packet_io_close(ctx_lan);
        return 1;
    }

    LOG_INFO("[rx_pipeline_test] TX: running for %d seconds "
             "(k=%d m=%d depth=%d sym=%d)",
             duration, k, m, depth, symbol_size);

    t_start = time(NULL);
    while (running && (time(NULL) - t_start) < duration) {
        rc = tx_pipeline_run_once(pl);
        if (rc == -1) {
            LOG_ERROR("[rx_pipeline_test] TX: tx_pipeline_run_once fatal error");
            inject_running = 0;
            pthread_join(inj_tid, NULL);
            tx_pipeline_destroy(pl);
            packet_io_close(ctx_fso);
            packet_io_close(ctx_lan);
            return 1;
        }
    }

    inject_running = 0;
    pthread_join(inj_tid, NULL);

    printf("TX MODE DONE: ran for %d seconds\n", duration);
    printf("  packets injected: %lu\n", inject_count);

    tx_pipeline_destroy(pl);
    packet_io_close(ctx_fso);
    packet_io_close(ctx_lan);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* RX mode                                                                     */
/* -------------------------------------------------------------------------- */

static int run_rx(const char *lan_iface,
                  const char *fso_iface,
                  int         duration,
                  int         k,
                  int         m,
                  int         depth,
                  int         symbol_size)
{
    struct config    cfg;
    char             errbuf[ERRBUF_SIZE];
    packet_io_ctx_t *ctx_fso;
    packet_io_ctx_t *ctx_lan;
    rx_pipeline_t   *pl;
    time_t           t_start;
    int              rc;

    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.lan_iface, lan_iface, sizeof(cfg.lan_iface) - 1);
    strncpy(cfg.fso_iface, fso_iface, sizeof(cfg.fso_iface) - 1);
    cfg.k                           = k;
    cfg.m                           = m;
    cfg.depth                       = depth;
    cfg.symbol_size                 = symbol_size;
    cfg.internal_symbol_crc_enabled = 1;

    ctx_fso = packet_io_open(fso_iface, 1, errbuf, sizeof(errbuf));
    if (ctx_fso == NULL) {
        LOG_ERROR("[rx_pipeline_test] RX: failed to open FSO \"%s\": %s",
                  fso_iface, errbuf);
        return 1;
    }

    ctx_lan = packet_io_open(lan_iface, 0, errbuf, sizeof(errbuf));
    if (ctx_lan == NULL) {
        LOG_ERROR("[rx_pipeline_test] RX: failed to open LAN \"%s\": %s",
                  lan_iface, errbuf);
        packet_io_close(ctx_fso);
        return 1;
    }

    pl = rx_pipeline_create(&cfg, ctx_fso, ctx_lan);
    if (pl == NULL) {
        LOG_ERROR("[rx_pipeline_test] RX: rx_pipeline_create failed");
        packet_io_close(ctx_lan);
        packet_io_close(ctx_fso);
        return 1;
    }

    LOG_INFO("[rx_pipeline_test] RX: running for %d seconds "
             "(k=%d m=%d depth=%d sym=%d)",
             duration, k, m, depth, symbol_size);

    t_start = time(NULL);
    while (running && (time(NULL) - t_start) < duration) {
        rc = rx_pipeline_run_once(pl);
        if (rc == -1) {
            LOG_ERROR("[rx_pipeline_test] RX: rx_pipeline_run_once fatal error");
            rx_pipeline_destroy(pl);
            packet_io_close(ctx_lan);
            packet_io_close(ctx_fso);
            return 1;
        }
    }

    printf("RX MODE DONE: ran for %d seconds\n", duration);

    rx_pipeline_destroy(pl);
    packet_io_close(ctx_lan);
    packet_io_close(ctx_fso);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* OBS (observer) mode                                                         */
/* -------------------------------------------------------------------------- */

static int run_obs(const char *lan_iface, int duration)
{
    char             errbuf[ERRBUF_SIZE];
    packet_io_ctx_t *ctx;
    unsigned char    buf[RX_BUF_SIZE];
    size_t           pkt_len;
    unsigned long    recovered_count;
    time_t           t_start;
    int              rc;

    ctx = packet_io_open(lan_iface, 1, errbuf, sizeof(errbuf));
    if (ctx == NULL) {
        LOG_ERROR("[rx_pipeline_test] OBS: failed to open \"%s\": %s",
                  lan_iface, errbuf);
        return 1;
    }

    recovered_count = 0;
    t_start         = time(NULL);

    LOG_INFO("[rx_pipeline_test] OBS: observing \"%s\" for %d seconds",
             lan_iface, duration);

    while (running && (time(NULL) - t_start) < duration) {
        pkt_len = 0;
        rc = packet_io_receive(ctx, buf, sizeof(buf), &pkt_len);

        if (rc == 1) {
            if (pkt_len >= (size_t)OBS_MIN_FRAME_LEN &&
                buf[12] == OBS_ETHERTYPE_HI &&
                buf[13] == OBS_ETHERTYPE_LO &&
                buf[14] == OBS_PAYLOAD_BYTE) {

                recovered_count++;
                if (recovered_count % OBS_PROGRESS_EVERY == 0) {
                    printf("OBS: %lu recovered packets so far\n",
                           recovered_count);
                }
            }
        } else if (rc == -1) {
            LOG_WARN("[rx_pipeline_test] OBS: packet_io_receive error: %s",
                     packet_io_last_error(ctx));
        }
    }

    packet_io_close(ctx);

    if (recovered_count > 0) {
        printf("RX PIPELINE TEST PASSED: recovered %lu packets on %s\n",
               recovered_count, lan_iface);
        return 0;
    } else {
        printf("RX PIPELINE TEST FAILED: no packets recovered on %s\n",
               lan_iface);
        return 1;
    }
}

/* -------------------------------------------------------------------------- */
/* main                                                                        */
/* -------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    static const struct option long_opts[] = {
        { "lan-iface",   required_argument, NULL, 'l' },
        { "fso-iface",   required_argument, NULL, 'f' },
        { "mode",        required_argument, NULL, 'o' },
        { "duration",    required_argument, NULL, 'd' },
        { "k",           required_argument, NULL, 'k' },
        { "m",           required_argument, NULL, 'm' },
        { "depth",       required_argument, NULL, 'e' },
        { "symbol-size", required_argument, NULL, 's' },
        { NULL,          0,                 NULL,  0  }
    };

    const char     *lan_iface   = NULL;
    const char     *fso_iface   = NULL;
    const char     *mode        = NULL;
    int             duration    = DEFAULT_DURATION;
    int             k           = DEFAULT_K;
    int             m           = DEFAULT_M;
    int             depth       = DEFAULT_DEPTH;
    int             symbol_size = DEFAULT_SYMBOL_SIZE;
    int             opt;
    struct sigaction sa;

    log_init();

    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'l': lan_iface   = optarg;       break;
        case 'f': fso_iface   = optarg;       break;
        case 'o': mode        = optarg;       break;
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

    if (lan_iface == NULL || fso_iface == NULL || mode == NULL) {
        print_usage();
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (strcmp(mode, "tx") == 0) {
        return run_tx(lan_iface, fso_iface, duration,
                      k, m, depth, symbol_size);
    } else if (strcmp(mode, "rx") == 0) {
        return run_rx(lan_iface, fso_iface, duration,
                      k, m, depth, symbol_size);
    } else if (strcmp(mode, "obs") == 0) {
        return run_obs(lan_iface, duration);
    } else {
        fprintf(stderr,
                "rx_pipeline_test: unknown mode \"%s\" (use tx, rx, or obs)\n",
                mode);
        print_usage();
        return 1;
    }
}
