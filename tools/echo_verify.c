/*
 * tools/echo_verify.c — Echo test verification tool for FSO Gateway Task 23.
 *
 * Runs as either a sender (TX) or receiver (RX) process to verify that
 * tools/echo_test correctly forwards raw Ethernet frames between two NICs.
 *
 * Usage:
 *   echo_verify --iface <iface> --mode tx|rx --count <N>
 *
 * Compile:
 *   gcc -std=c99 -Wall -Wextra \
 *       -Iinclude \
 *       -o build/echo_verify \
 *       tools/echo_verify.c src/packet_io.c src/logging.c \
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

#define ERRBUF_SIZE     256
#define RX_BUF_SIZE     9200
#define RX_TIMEOUT_SEC  30

/* Test frame layout */
#define FRAME_LEN       25
#define ETHERTYPE_HI    0x5A
#define ETHERTYPE_LO    0x47
#define PAYLOAD_STR     "FSO-GW-T23"
#define PAYLOAD_LEN     10

/* -------------------------------------------------------------------------- */
/* Signal handling                                                             */
/* -------------------------------------------------------------------------- */

static volatile sig_atomic_t running = 1;

static void sig_handler(int signum)
{
    (void)signum;
    running = 0;
}

/* -------------------------------------------------------------------------- */
/* Usage                                                                       */
/* -------------------------------------------------------------------------- */

static void print_usage(void)
{
    fprintf(stderr,
            "Usage: echo_verify --iface <iface> --mode tx|rx --count <N>\n");
}

/* -------------------------------------------------------------------------- */
/* Build test frame                                                            */
/* -------------------------------------------------------------------------- */

static void build_test_frame(unsigned char frame[FRAME_LEN])
{
    /* dst MAC: ff:ff:ff:ff:ff:ff */
    memset(frame, 0xff, 6);
    /* src MAC: 02:00:00:00:00:99 */
    frame[6]  = 0x02;
    frame[7]  = 0x00;
    frame[8]  = 0x00;
    frame[9]  = 0x00;
    frame[10] = 0x00;
    frame[11] = 0x99;
    /* EtherType: 0x5A47 */
    frame[12] = ETHERTYPE_HI;
    frame[13] = ETHERTYPE_LO;
    /* Payload: "FSO-GW-T23" (10 bytes, no null terminator) */
    memcpy(frame + 14, PAYLOAD_STR, PAYLOAD_LEN);
}

/* -------------------------------------------------------------------------- */
/* TX mode                                                                     */
/* -------------------------------------------------------------------------- */

static int run_tx(const char *iface, int count)
{
    char             errbuf[ERRBUF_SIZE];
    packet_io_ctx_t *ctx;
    unsigned char    frame[FRAME_LEN];
    int              i;
    int              rc;

    ctx = packet_io_open(iface, 0, errbuf, sizeof(errbuf));
    if (ctx == NULL) {
        LOG_ERROR("[echo_verify] TX: failed to open \"%s\": %s", iface, errbuf);
        return 1;
    }

    build_test_frame(frame);

    for (i = 1; i <= count && running; ++i) {
        rc = packet_io_send(ctx, frame, FRAME_LEN);
        if (rc == -1) {
            LOG_ERROR("[echo_verify] TX: send error on \"%s\": %s",
                      iface, packet_io_last_error(ctx));
            packet_io_close(ctx);
            return 1;
        }
        printf("TX [%d/%d] sent\n", i, count);
        usleep(5000);
    }

    printf("TX DONE: sent %d frames on %s\n", count, iface);
    packet_io_close(ctx);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* RX mode                                                                     */
/* -------------------------------------------------------------------------- */

static int frame_matches(const unsigned char *buf, size_t len)
{
    if (len < FRAME_LEN) {
        return 0;
    }
    if (buf[12] != ETHERTYPE_HI || buf[13] != ETHERTYPE_LO) {
        return 0;
    }
    if (memcmp(buf + 14, PAYLOAD_STR, PAYLOAD_LEN) != 0) {
        return 0;
    }
    return 1;
}

static int run_rx(const char *iface, int count)
{
    char             errbuf[ERRBUF_SIZE];
    packet_io_ctx_t *ctx;
    unsigned char    buf[RX_BUF_SIZE];
    size_t           pkt_len;
    int              matched;
    time_t           t_start;
    int              rc;

    ctx = packet_io_open(iface, 1, errbuf, sizeof(errbuf));
    if (ctx == NULL) {
        LOG_ERROR("[echo_verify] RX: failed to open \"%s\": %s", iface, errbuf);
        return 1;
    }

    matched = 0;
    t_start = time(NULL);

    while (running) {
        if ((time(NULL) - t_start) >= RX_TIMEOUT_SEC) {
            printf("RX TIMEOUT: matched %d/%d frames on %s\n",
                   matched, count, iface);
            packet_io_close(ctx);
            return 1;
        }

        pkt_len = 0;
        rc = packet_io_receive(ctx, buf, sizeof(buf), &pkt_len);

        if (rc == 1) {
            if (frame_matches(buf, pkt_len)) {
                ++matched;
                printf("RX [%d/%d] matched\n", matched, count);
                if (matched == count) {
                    printf("RX TEST PASSED: matched %d/%d frames on %s\n",
                           matched, count, iface);
                    packet_io_close(ctx);
                    return 0;
                }
            }
            /* non-matching frames ignored silently */
        } else if (rc == 0) {
            usleep(1000);
        } else {
            LOG_ERROR("[echo_verify] RX: receive error on \"%s\": %s",
                      iface, packet_io_last_error(ctx));
        }
    }

    /* running == 0: signal received */
    printf("INTERRUPTED: %d frames before signal\n", matched);
    packet_io_close(ctx);
    return 1;
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

    const char *iface = NULL;
    const char *mode  = NULL;
    int         count = 0;
    int         opt;

    struct sigaction sa;

    log_init();

    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'i':
            iface = optarg;
            break;
        case 'm':
            mode = optarg;
            break;
        case 'c':
            count = atoi(optarg);
            break;
        default:
            print_usage();
            return 1;
        }
    }

    if (iface == NULL || mode == NULL || count <= 0) {
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
        return run_tx(iface, count);
    } else if (strcmp(mode, "rx") == 0) {
        return run_rx(iface, count);
    } else {
        fprintf(stderr, "echo_verify: unknown mode \"%s\" (use tx or rx)\n",
                mode);
        print_usage();
        return 1;
    }
}
