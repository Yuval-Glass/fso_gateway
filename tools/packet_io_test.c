/*
 * tools/packet_io_test.c — Minimal test tool for the packet_io module.
 *
 * Proves that packet_io_open, packet_io_receive, and packet_io_send work
 * correctly on a real NIC.
 *
 * Usage:
 *   packet_io_test --iface <iface> --mode rx|tx --count <N>
 *
 * Compile:
 *   gcc -std=c99 -Wall -Wextra \
 *       -Iinclude \
 *       -o build/packet_io_test \
 *       tools/packet_io_test.c src/packet_io.c src/logging.c \
 *       -lpcap
 */

#define _POSIX_C_SOURCE 200112L

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "logging.h"
#include "packet_io.h"

/* -------------------------------------------------------------------------- */
/* Constants                                                                   */
/* -------------------------------------------------------------------------- */

#define RX_BUF_SIZE      9200
#define RX_TIMEOUT_SEC   30
#define RX_POLL_US       1000
#define TX_INTER_US      10000

/* -------------------------------------------------------------------------- */
/* Signal handling                                                             */
/* -------------------------------------------------------------------------- */

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int signum)
{
    (void)signum;
    g_running = 0;
}

/* -------------------------------------------------------------------------- */
/* Usage                                                                       */
/* -------------------------------------------------------------------------- */

static void print_usage(void)
{
    fprintf(stderr,
            "Usage: packet_io_test --iface <iface> --mode rx|tx --count <N>\n");
}

/* -------------------------------------------------------------------------- */
/* RX mode                                                                     */
/* -------------------------------------------------------------------------- */

static int run_rx(const char *iface, int count)
{
    char             errbuf[256];
    packet_io_ctx_t *ctx;
    unsigned char    buf[RX_BUF_SIZE];
    size_t           out_len;
    int              received;
    int              rc;
    time_t           start;
    time_t           now;

    ctx = packet_io_open(iface, 1, errbuf, sizeof(errbuf));
    if (ctx == NULL) {
        LOG_ERROR("[packet_io_test] RX open failed: %s", errbuf);
        return 1;
    }

    LOG_INFO("[packet_io_test] RX mode: waiting for %d packets on %s",
             count, iface);

    received = 0;
    start    = time(NULL);

    while (g_running && received < count) {
        now = time(NULL);
        if ((now - start) >= RX_TIMEOUT_SEC) {
            printf("RX TEST TIMEOUT: received %d/%d packets on %s\n",
                   received, count, iface);
            packet_io_close(ctx);
            return 1;
        }

        out_len = 0;
        rc = packet_io_receive(ctx, buf, sizeof(buf), &out_len);

        if (rc == 1) {
            received++;
            printf("RX [%d/%d] len=%zu bytes\n", received, count, out_len);
        } else if (rc == 0) {
            usleep(RX_POLL_US);
        } else {
            LOG_ERROR("[packet_io_test] RX receive error: %s",
                      packet_io_last_error(ctx));
            packet_io_close(ctx);
            return 1;
        }
    }

    if (!g_running) {
        printf("INTERRUPTED: rx %d packets before signal\n", received);
        packet_io_close(ctx);
        return 1;
    }

    printf("RX TEST PASSED: received %d packets on %s\n", received, iface);
    packet_io_close(ctx);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* TX mode                                                                     */
/* -------------------------------------------------------------------------- */

static int run_tx(const char *iface, int count)
{
    /*
     * Minimal raw Ethernet frame:
     *   Bytes  0– 5:  dst MAC  ff:ff:ff:ff:ff:ff
     *   Bytes  6–11:  src MAC  02:00:00:00:00:01
     *   Bytes 12–13:  EtherType 0x1234 (big-endian)
     *   Bytes 14–24:  payload  "FSO-GW-TEST" (11 bytes, no null terminator)
     *   Total: 25 bytes
     */
    static const unsigned char frame[25] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,   /* dst MAC */
        0x02, 0x00, 0x00, 0x00, 0x00, 0x01,   /* src MAC */
        0x12, 0x34,                             /* EtherType */
        'F', 'S', 'O', '-', 'G', 'W', '-', 'T', 'E', 'S', 'T'  /* payload */
    };

    char             errbuf[256];
    packet_io_ctx_t *ctx;
    int              sent;
    int              rc;

    ctx = packet_io_open(iface, 0, errbuf, sizeof(errbuf));
    if (ctx == NULL) {
        LOG_ERROR("[packet_io_test] TX open failed: %s", errbuf);
        return 1;
    }

    LOG_INFO("[packet_io_test] TX mode: sending %d packets on %s",
             count, iface);

    sent = 0;

    while (g_running && sent < count) {
        rc = packet_io_send(ctx, frame, sizeof(frame));
        if (rc != 0) {
            printf("TX send error: %s\n", packet_io_last_error(ctx));
            packet_io_close(ctx);
            return 1;
        }

        sent++;
        printf("TX [%d/%d] sent %zu bytes\n", sent, count, sizeof(frame));

        if (sent < count) {
            usleep(TX_INTER_US);
        }
    }

    if (!g_running) {
        printf("INTERRUPTED: tx %d packets before signal\n", sent);
        packet_io_close(ctx);
        return 1;
    }

    printf("TX TEST PASSED: sent %d packets on %s\n", sent, iface);
    packet_io_close(ctx);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* main                                                                        */
/* -------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    static const struct option long_opts[] = {
        { "iface", required_argument, NULL, 'i' },
        { "mode",  required_argument, NULL, 'm' },
        { "count", required_argument, NULL, 'c' },
        { NULL,    0,                 NULL,  0  }
    };

    struct sigaction sa;
    char  iface[64];
    char  mode[8];
    int   count;
    int   opt;
    int   iface_set;
    int   mode_set;
    int   count_set;

    iface[0]  = '\0';
    mode[0]   = '\0';
    count     = 0;
    iface_set = 0;
    mode_set  = 0;
    count_set = 0;

    /* Install signal handlers */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Parse arguments */
    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'i':
            strncpy(iface, optarg, sizeof(iface) - 1);
            iface[sizeof(iface) - 1] = '\0';
            iface_set = 1;
            break;
        case 'm':
            strncpy(mode, optarg, sizeof(mode) - 1);
            mode[sizeof(mode) - 1] = '\0';
            mode_set = 1;
            break;
        case 'c':
            count = atoi(optarg);
            count_set = 1;
            break;
        default:
            print_usage();
            return 1;
        }
    }

    if (!iface_set || !mode_set || !count_set) {
        print_usage();
        return 1;
    }

    if (count <= 0) {
        fprintf(stderr, "Error: --count must be a positive integer\n");
        print_usage();
        return 1;
    }

    if (strcmp(mode, "rx") != 0 && strcmp(mode, "tx") != 0) {
        fprintf(stderr, "Error: --mode must be rx or tx\n");
        print_usage();
        return 1;
    }

    /* Initialize logging */
    if (log_init() != 0) {
        fprintf(stderr, "Error: failed to initialize logging\n");
        return 1;
    }

    LOG_INFO("[packet_io_test] Starting: iface=%s mode=%s count=%d",
             iface, mode, count);

    /* Dispatch */
    if (strcmp(mode, "rx") == 0) {
        return run_rx(iface, count);
    } else {
        return run_tx(iface, count);
    }
}