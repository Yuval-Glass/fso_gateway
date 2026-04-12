/*
 * tools/tx_pipeline_test.c — TX pipeline validation tool for FSO Gateway Task 24.
 *
 * Runs in one of two modes:
 *   TX  — injects synthetic Ethernet frames into NIC_LAN, runs the TX pipeline,
 *          and transmits interleaved FEC symbols out of NIC_FSO.
 *   RX  — observes NIC_FSO and counts received wire-format symbols.
 *
 * Usage:
 *   tx_pipeline_test --lan-iface <i> --fso-iface <i> --mode tx|rx
 *                    [--duration <sec>] [--k N] [--m N]
 *                    [--depth N] [--symbol-size N]
 *
 * Compile:
 *   gcc -std=c99 -Wall -Wextra \
 *       -Iinclude \
 *       -o build/tx_pipeline_test \
 *       tools/tx_pipeline_test.c \
 *       src/tx_pipeline.c src/packet_io.c src/logging.c src/config.c \
 *       src/block_builder.c src/fec_wrapper.c src/interleaver.c \
 *       src/packet_fragmenter.c src/symbol.c \
 *       third_party/wirehair/*.cpp \
 *       -lpcap -lpthread -lm -lstdc++ \
 *       -Ithird_party/wirehair/include
 */

#define _POSIX_C_SOURCE 200112L

#include <arpa/inet.h>
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
#include "tx_pipeline.h"
#include <wirehair/wirehair.h>

/* -------------------------------------------------------------------------- */
/* Constants                                                                   */
/* -------------------------------------------------------------------------- */

#define ERRBUF_SIZE         256
#define RX_BUF_SIZE         9200
#define WIRE_HDR_SIZE       18      /* bytes: FSO Gateway wire format header  */
#define INJECT_FRAME_LEN    1014    /* 14-byte Eth hdr + 1000-byte payload    */
#define INJECT_INTERVAL_US  1000    /* 1 ms between injected frames           */
#define RX_PROGRESS_EVERY   100     /* print progress every N symbols         */
#define MAX_SYMBOL_PAYLOAD  9000

/* Default CLI values */
#define DEFAULT_DURATION    10
#define DEFAULT_K           8
#define DEFAULT_M           4
#define DEFAULT_DEPTH       2
#define DEFAULT_SYMBOL_SIZE 1500

/* -------------------------------------------------------------------------- */
/* Signal handling                                                             */
/* -------------------------------------------------------------------------- */

static volatile sig_atomic_t running       = 1;
static volatile sig_atomic_t inject_running = 1;

static void sig_handler(int signum)
{
    (void)signum;
    running        = 0;
    inject_running = 0;
}

/* -------------------------------------------------------------------------- */
/* Injector thread state                                                       */
/* -------------------------------------------------------------------------- */

typedef struct {
    packet_io_ctx_t *ctx_lan;
    unsigned long   *inject_count;
    size_t           inject_frame_len;
    int              inject_k;
} injector_args_t;

static void *injector_thread(void *arg)
{
    injector_args_t *a = (injector_args_t *)arg;
    unsigned char    frame[9200];
    int              rc;

    size_t frame_len = a->inject_frame_len;
    memset(frame, 0, frame_len);

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
    frame[12] = 0x08;
    frame[13] = 0x00;
    /* Payload: 1000 bytes of 0xAB */
    memset(frame + 14, 0xAB, frame_len - 14);

    while (inject_running) {
        int i;
        for (i = 0; i < a->inject_k && inject_running; i++) {
            rc = packet_io_send(a->ctx_lan, frame, frame_len);
            if (rc == 0) {
                (*a->inject_count)++;
            } else {
                LOG_WARN("[tx_pipeline_test] injector: packet_io_send failed");
            }
        }
        usleep(INJECT_INTERVAL_US);
    }

    return NULL;
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

    /* Build config */
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.lan_iface,  lan_iface,  sizeof(cfg.lan_iface)  - 1);
    strncpy(cfg.fso_iface,  fso_iface,  sizeof(cfg.fso_iface)  - 1);
    cfg.k                        = k;
    cfg.m                        = m;
    cfg.depth                    = depth;
    cfg.symbol_size              = symbol_size;
    cfg.internal_symbol_crc_enabled = 1;

    /* Open interfaces */
    ctx_lan = packet_io_open(lan_iface, 1, errbuf, sizeof(errbuf));
    if (ctx_lan == NULL) {
        LOG_ERROR("[tx_pipeline_test] TX: failed to open LAN iface \"%s\": %s",
                  lan_iface, errbuf);
        return 1;
    }

    ctx_fso = packet_io_open(fso_iface, 0, errbuf, sizeof(errbuf));
    if (ctx_fso == NULL) {
        LOG_ERROR("[tx_pipeline_test] TX: failed to open FSO iface \"%s\": %s",
                  fso_iface, errbuf);
        packet_io_close(ctx_lan);
        return 1;
    }

    /* Create TX pipeline */
    pl = tx_pipeline_create(&cfg, ctx_lan, ctx_fso);
    if (pl == NULL) {
        LOG_ERROR("[tx_pipeline_test] TX: tx_pipeline_create failed");
        packet_io_close(ctx_fso);
        packet_io_close(ctx_lan);
        return 1;
    }

    inject_running = 1;
    running = 1;
    /* Start injector thread */
    inject_count       = 0;
    inj_args.ctx_lan   = ctx_fso;
    inj_args.inject_count = &inject_count;
    inj_args.inject_frame_len = (size_t)symbol_size + 14;
    inj_args.inject_k = k;

    if (pthread_create(&inj_tid, NULL, injector_thread, &inj_args) != 0) {
        LOG_ERROR("[tx_pipeline_test] TX: pthread_create failed");
        tx_pipeline_destroy(pl);
        packet_io_close(ctx_fso);
        packet_io_close(ctx_lan);
        return 1;
    }

    LOG_INFO("[tx_pipeline_test] TX: running for %d seconds "
             "(k=%d m=%d depth=%d sym=%d)",
             duration, k, m, depth, symbol_size);

    /* Main TX loop */
    t_start = time(NULL);
    while (running && (time(NULL) - t_start) < duration) {
        rc = tx_pipeline_run_once(pl);
        if (rc == -1) {
            LOG_ERROR("[tx_pipeline_test] TX: tx_pipeline_run_once fatal error");
            inject_running = 0;
            pthread_cancel(inj_tid);
    pthread_join(inj_tid, NULL);
            tx_pipeline_destroy(pl);
            packet_io_close(ctx_fso);
            packet_io_close(ctx_lan);
            return 1;
        }
    }

    /* Stop injector */
    inject_running = 0;
    pthread_cancel(inj_tid);
    pthread_join(inj_tid, NULL);

    printf("TX MODE DONE: ran for %d seconds\n", duration);
    printf("  packets injected: %lu\n", inject_count);

    tx_pipeline_destroy(pl);
    packet_io_close(ctx_fso);
    packet_io_close(ctx_lan);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* RX (observer) mode                                                          */
/* -------------------------------------------------------------------------- */

static int run_rx(const char *fso_iface, int duration)
{
    char             errbuf[ERRBUF_SIZE];
    packet_io_ctx_t *ctx;
    unsigned char    buf[RX_BUF_SIZE];
    size_t           pkt_len;
    unsigned long    symbol_count;
    time_t           t_start;
    int              rc;
    uint16_t         payload_len_be;
    uint16_t         payload_len;

    ctx = packet_io_open(fso_iface, 1, errbuf, sizeof(errbuf));
    if (ctx == NULL) {
        LOG_ERROR("[tx_pipeline_test] RX: failed to open \"%s\": %s",
                  fso_iface, errbuf);
        return 1;
    }

    symbol_count = 0;
    t_start      = time(NULL);

    LOG_INFO("[tx_pipeline_test] RX: observing \"%s\" for %d seconds",
             fso_iface, duration);

    while (running && (time(NULL) - t_start) < duration) {
        pkt_len = 0;
        rc = packet_io_receive(ctx, buf, sizeof(buf), &pkt_len);

        if (rc == 1) {
            /* Heuristic: must be at least WIRE_HDR_SIZE bytes */
            if (pkt_len >= (size_t)WIRE_HDR_SIZE) {
                /* payload_len is at wire offset 12, big-endian uint16 */
                memcpy(&payload_len_be, buf + 12, 2);
                payload_len = ntohs(payload_len_be);
                if (payload_len > 0 && payload_len <= MAX_SYMBOL_PAYLOAD) {
                    symbol_count++;
                    if (symbol_count % RX_PROGRESS_EVERY == 0) {
                        printf("RX observer: %lu symbols captured so far\n",
                               symbol_count);
                    }
                }
            }
        } else if (rc == -1) {
            LOG_WARN("[tx_pipeline_test] RX: packet_io_receive error: %s",
                     packet_io_last_error(ctx));
        }
        /* rc == 0: no packet, keep polling */
    }

    packet_io_close(ctx);

    if (symbol_count > 0) {
        printf("TX PIPELINE TEST PASSED: captured %lu symbols on %s\n",
               symbol_count, fso_iface);
        return 0;
    } else {
        printf("TX PIPELINE TEST FAILED: no symbols captured on %s\n",
               fso_iface);
        return 1;
    }
}

/* -------------------------------------------------------------------------- */
/* Usage                                                                       */
/* -------------------------------------------------------------------------- */

static void print_usage(void)
{
    fprintf(stderr,
            "Usage: tx_pipeline_test --lan-iface <i> --fso-iface <i>"
            " --mode tx|rx [--duration <sec>] [--k N] [--m N]"
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

    if (wirehair_init() != Wirehair_Success) {
        fprintf(stderr, "wirehair_init() failed\n");
        return 1;
    }

    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'l': lan_iface   = optarg;        break;
        case 'f': fso_iface   = optarg;        break;
        case 'o': mode        = optarg;        break;
        case 'd': duration    = atoi(optarg);  break;
        case 'k': k           = atoi(optarg);  break;
        case 'm': m           = atoi(optarg);  break;
        case 'e': depth       = atoi(optarg);  break;
        case 's': symbol_size = atoi(optarg);  break;
        default:
            print_usage();
            return 1;
        }
    }

    if (lan_iface == NULL || fso_iface == NULL || mode == NULL) {
        print_usage();
        return 1;
    }

    /* Signal handlers */
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
        return run_rx(fso_iface, duration);
    } else {
        fprintf(stderr,
                "tx_pipeline_test: unknown mode \"%s\" (use tx or rx)\n",
                mode);
        print_usage();
        return 1;
    }
}
