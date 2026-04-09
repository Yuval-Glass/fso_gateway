/*
 * tools/echo_test.c — Raw Ethernet frame echo/forwarding test tool.
 *
 * Proves basic packet forwarding between two real NICs with no FEC,
 * no fragmentation, and no interleaving.  Frames received on the LAN
 * interface are forwarded to the FSO interface and vice-versa.
 *
 * Usage:
 *   echo_test --lan-iface <iface> --fso-iface <iface> [--duration <sec>]
 *
 * Compile:
 *   gcc -std=c99 -Wall -Wextra \
 *       -Iinclude \
 *       -o build/echo_test \
 *       tools/echo_test.c src/packet_io.c src/logging.c \
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

/* Receive buffer: matches packet_io snaplen (9200) with headroom */
#define ECHO_BUF_SIZE   9200UL

/* Print a progress line every this many forwarded packets */
#define PROGRESS_INTERVAL 1000UL

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
            "Usage: echo_test --lan-iface <iface> --fso-iface <iface>"
            " [--duration <sec>]\n");
}

/* -------------------------------------------------------------------------- */
/* main                                                                        */
/* -------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    /* --- option parsing -------------------------------------------------- */
    static const struct option long_opts[] = {
        { "lan-iface", required_argument, NULL, 'l' },
        { "fso-iface", required_argument, NULL, 'f' },
        { "duration",  required_argument, NULL, 'd' },
        { NULL,        0,                 NULL,  0  }
    };

    const char *lan_iface  = NULL;
    const char *fso_iface  = NULL;
    long        duration   = 0;
    int         opt;

    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'l':
            lan_iface = optarg;
            break;
        case 'f':
            fso_iface = optarg;
            break;
        case 'd':
            duration = atol(optarg);
            break;
        default:
            print_usage();
            return 1;
        }
    }

    if (lan_iface == NULL || fso_iface == NULL) {
        print_usage();
        return 1;
    }

    /* --- logging init ---------------------------------------------------- */
    log_init();

    /* --- signal handlers ------------------------------------------------- */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sig_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT,  &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }

    /* --- open interfaces ------------------------------------------------- */
    char errbuf[256];
    packet_io_ctx_t *ctx_lan;
    packet_io_ctx_t *ctx_fso;

    ctx_lan = packet_io_open(lan_iface, 1, errbuf, sizeof(errbuf));
    if (ctx_lan == NULL) {
        LOG_ERROR("[echo_test] Failed to open LAN interface \"%s\": %s",
                  lan_iface, errbuf);
        return 1;
    }

    ctx_fso = packet_io_open(fso_iface, 1, errbuf, sizeof(errbuf));
    if (ctx_fso == NULL) {
        LOG_ERROR("[echo_test] Failed to open FSO interface \"%s\": %s",
                  fso_iface, errbuf);
        packet_io_close(ctx_lan);
        return 1;
    }

    /* --- state ----------------------------------------------------------- */
    unsigned char  buf[ECHO_BUF_SIZE];
    size_t         pkt_len    = 0;
    unsigned long  lan_to_fso = 0;
    unsigned long  fso_to_lan = 0;
    unsigned long  total      = 0;
    unsigned long  last_print = 0;
    time_t         t_start    = time(NULL);
    int            rc;

    LOG_INFO("[echo_test] Starting: lan=%s fso=%s duration=%lds",
             lan_iface, fso_iface, duration);

    /* --- main forwarding loop -------------------------------------------- */
    while (running) {
        int got_packet = 0;

        /* Check duration */
        if (duration > 0 && (time(NULL) - t_start) >= duration) {
            break;
        }

        /* LAN → FSO */
        rc = packet_io_receive(ctx_lan, buf, sizeof(buf), &pkt_len);
        if (rc == 1) {
            got_packet = 1;
            if (packet_io_send(ctx_fso, buf, pkt_len) == -1) {
                LOG_WARN("[echo_test] send to FSO failed: %s",
                         packet_io_last_error(ctx_fso));
            } else {
                ++lan_to_fso;
                ++total;
            }
        } else if (rc == -1) {
            LOG_WARN("[echo_test] receive from LAN error: %s",
                     packet_io_last_error(ctx_lan));
        }

        /* FSO → LAN */
        rc = packet_io_receive(ctx_fso, buf, sizeof(buf), &pkt_len);
        if (rc == 1) {
            got_packet = 1;
            if (packet_io_send(ctx_lan, buf, pkt_len) == -1) {
                LOG_WARN("[echo_test] send to LAN failed: %s",
                         packet_io_last_error(ctx_lan));
            } else {
                ++fso_to_lan;
                ++total;
            }
        } else if (rc == -1) {
            LOG_WARN("[echo_test] receive from FSO error: %s",
                     packet_io_last_error(ctx_fso));
        }

        /* Idle sleep */
        if (!got_packet) {
            usleep(100);
        }

        /* Progress report every PROGRESS_INTERVAL packets */
        if (total >= last_print + PROGRESS_INTERVAL && total > last_print) {
            printf("echo_test: forwarded %lu packets"
                   " (lan->fso: %lu, fso->lan: %lu)\n",
                   total, lan_to_fso, fso_to_lan);
            last_print = total;
        }
    }

    /* --- final summary --------------------------------------------------- */
    printf("echo_test: DONE — total forwarded: %lu"
           " (lan->fso: %lu, fso->lan: %lu)\n",
           total, lan_to_fso, fso_to_lan);

    /* --- cleanup --------------------------------------------------------- */
    packet_io_close(ctx_fso);
    packet_io_close(ctx_lan);

    return 0;
}
