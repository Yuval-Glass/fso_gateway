/*
 * main.c — FSO Gateway entry point.
 *
 * Start-up sequence
 * -----------------
 *   1. Logging initialisation.
 *   2. Wirehair global initialisation.
 *   3. FEC self-test  — abort if the encode/decode core is broken.
 *   4. Stats and signal handlers.
 *   5. CLI configuration parsing.
 *   6. Fragmentation / reassembly self-tests.
 *   7. Block builder initialisation.
 *   8. Raw capture socket.
 *   9. Packet-sniffing loop.
 *  10. Clean shutdown (partial-block flush, stats report).
 */

#define _POSIX_C_SOURCE 200112L

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <wirehair/wirehair.h>

#include "block_builder.h"
#include "config.h"
#include "fec_wrapper.h"
#include "logging.h"
#include "network.h"
#include "packet_fragmenter.h"
#include "packet_reassembler.h"
#include "stats.h"
#include "types.h"

/* -------------------------------------------------------------------------- */
/* Compile-time limits                                                         */
/* -------------------------------------------------------------------------- */

#define RX_BUFFER_SIZE            9000U
#define RECONSTRUCTED_BUFFER_SIZE 9000U
#define SYMBOL_BUFFER_SIZE        1024U

#define BLOCK_TIMEOUT_MS  50.0
#define POLL_TIMEOUT_MS   10

/* -------------------------------------------------------------------------- */
/* Process-lifetime globals                                                    */
/* -------------------------------------------------------------------------- */

static volatile sig_atomic_t g_running = 1;
static block_builder_t       g_block_builder;

/* -------------------------------------------------------------------------- */
/* Signal handling                                                             */
/* -------------------------------------------------------------------------- */

static void handle_signal(int signo)
{
    (void)signo;
    g_running = 0;
}

/* -------------------------------------------------------------------------- */
/* Fragmentation / reassembly startup self-tests                              */
/*                                                                             */
/* These tests exercise packet_fragmenter and packet_reassembler across a     */
/* representative range of packet sizes and verify byte-exact round-trips.    */
/* Run independently of FEC so that failures are attributed correctly.        */
/* -------------------------------------------------------------------------- */

static void run_packet_test(size_t        packet_len,
                            int           symbol_size,
                            unsigned char pattern)
{
    unsigned char  packet[9000];
    unsigned char  reconstructed[9000];
    symbol_t      *sym_buf;
    int            num_symbols;
    int            recon_len;
    const uint32_t test_id = 0x12340000U + (uint32_t)packet_len;

    if (packet_len > sizeof(packet) || symbol_size <= 0) {
        LOG_ERROR("[TEST] Packet test (%zuB): invalid parameters", packet_len);
        return;
    }

    sym_buf = (symbol_t *)malloc(sizeof(symbol_t) * SYMBOL_BUFFER_SIZE);
    if (sym_buf == NULL) {
        LOG_ERROR("[TEST] Packet test (%zuB): malloc failed", packet_len);
        return;
    }

    memset(packet,        0,       sizeof(packet));
    memset(reconstructed, 0,       sizeof(reconstructed));
    memset(sym_buf,       0,       sizeof(symbol_t) * SYMBOL_BUFFER_SIZE);
    memset(packet,        pattern, packet_len);

    num_symbols = fragment_packet(packet, packet_len, test_id,
                                  (uint16_t)symbol_size,
                                  sym_buf, SYMBOL_BUFFER_SIZE);

    if (num_symbols < 0) {
        LOG_ERROR("[TEST] Packet test (%zuB): fragment_packet() failed",
                  packet_len);
        free(sym_buf);
        return;
    }

    recon_len = reassemble_packet(sym_buf, (uint16_t)num_symbols,
                                  reconstructed, sizeof(reconstructed));

    if (recon_len < 0 ||
        (size_t)recon_len != packet_len ||
        memcmp(packet, reconstructed, packet_len) != 0)
    {
        LOG_ERROR("[TEST] Packet test (%zuB): FAILED", packet_len);
        free(sym_buf);
        return;
    }

    LOG_INFO("[TEST] Packet test (%zuB): PASSED", packet_len);
    free(sym_buf);
}

static void run_startup_tests(int symbol_size)
{
    /* Jumbo frame — 9000 bytes, uniform 0xAA fill */
    {
        unsigned char  jumbo[9000];
        unsigned char  recon[9000];
        symbol_t      *sym_buf;
        int            nsym;
        int            rlen;

        memset(jumbo,   0xAA, sizeof(jumbo));
        memset(recon,   0,    sizeof(recon));

        sym_buf = (symbol_t *)malloc(sizeof(symbol_t) * SYMBOL_BUFFER_SIZE);
        if (sym_buf == NULL) {
            LOG_ERROR("[TEST] Jumbo frame test: malloc failed");
        } else {
            memset(sym_buf, 0, sizeof(symbol_t) * SYMBOL_BUFFER_SIZE);

            nsym = fragment_packet(jumbo, sizeof(jumbo), 0xABCDEF01U,
                                   (uint16_t)symbol_size,
                                   sym_buf, SYMBOL_BUFFER_SIZE);

            rlen = (nsym >= 0)
                   ? reassemble_packet(sym_buf, (uint16_t)nsym,
                                       recon, sizeof(recon))
                   : -1;

            if (rlen < 0 ||
                (size_t)rlen != sizeof(jumbo) ||
                memcmp(jumbo, recon, sizeof(jumbo)) != 0)
            {
                LOG_ERROR("[TEST] Jumbo frame test (9000B): FAILED");
            } else {
                LOG_INFO("[TEST] Jumbo frame test (9000B): PASSED");
            }

            free(sym_buf);
        }
    }

    /* Variable-size packet suite */
    run_packet_test(64U,   symbol_size, 0x11);
    run_packet_test(128U,  symbol_size, 0x22);
    run_packet_test(511U,  symbol_size, 0x33);
    run_packet_test(1500U, symbol_size, 0x44);
    run_packet_test(4096U, symbol_size, 0x55);
    run_packet_test(8999U, symbol_size, 0x66);
}

/* -------------------------------------------------------------------------- */
/* Block builder helpers                                                       */
/* -------------------------------------------------------------------------- */

static void process_incoming_symbols(symbol_t *syms, int count)
{
    int i;

    if (syms == NULL || count < 0) {
        LOG_ERROR("process_incoming_symbols: invalid arguments");
        return;
    }

    for (i = 0; i < count; ++i) {
        int rc = block_builder_add_symbol(&g_block_builder, &syms[i]);

        if (rc < 0) {
            LOG_ERROR("process_incoming_symbols: add failed at index %d", i);
            return;
        }

        if (rc == 1) {
            LOG_INFO("Block %lu complete: %d symbols — ready for FEC encoding",
                     (unsigned long)g_block_builder.block_id,
                     g_block_builder.symbol_count);

            block_builder_reset(&g_block_builder);
        }
    }
}

static void process_block_timeout_if_needed(void)
{
    int rc = block_builder_check_timeout(&g_block_builder, BLOCK_TIMEOUT_MS);

    if (rc < 0) {
        LOG_ERROR("block_builder_check_timeout() failed");
        return;
    }

    if (rc == 1) {
        int orig    = g_block_builder.symbol_count;
        int padding = g_block_builder.k_limit - orig;

        block_builder_finalize_with_padding(&g_block_builder);

        LOG_WARN("Block %lu timed out — finalized: %d symbols + %d padding",
                 (unsigned long)g_block_builder.block_id, orig, padding);

        block_builder_reset(&g_block_builder);
    }
}

/* -------------------------------------------------------------------------- */
/* main                                                                        */
/* -------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    struct config cfg;
    int           raw_fd    = -1;
    uint32_t      packet_id = 0;
    int           exit_code = 0;
    struct pollfd pfd;

    unsigned char rx_buffer[RX_BUFFER_SIZE];

    memset(&g_block_builder, 0, sizeof(g_block_builder));

    /* ------------------------------------------------------------------ */
    /* Logging                                                             */
    /* ------------------------------------------------------------------ */
    log_init();

    /* ------------------------------------------------------------------ */
    /* Wirehair global initialisation                                      */
    /* ------------------------------------------------------------------ */
    if (wirehair_init() != Wirehair_Success) {
        LOG_ERROR("[FEC] Wirehair initialisation FAILED");
        return 1;
    }

    LOG_INFO("[FEC] Wirehair library initialised");

    /* ------------------------------------------------------------------ */
    /* FEC self-test                                                       */
    /*                                                                     */
    /* Verifies a single-loss encode/decode round-trip with byte-exact    */
    /* reconstruction.  The gateway must not start with a broken FEC      */
    /* core — this gate is non-negotiable.                                */
    /* ------------------------------------------------------------------ */
    if (fec_run_self_test() != 0) {
        LOG_ERROR("[FEC] Self-test FAILED — aborting startup");
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* Stats and signal handlers                                           */
    /* ------------------------------------------------------------------ */
    stats_init();

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    /* ------------------------------------------------------------------ */
    /* CLI configuration                                                   */
    /* ------------------------------------------------------------------ */
    if (config_parse(argc, argv, &cfg) != 0) {
        LOG_ERROR("Failed to parse configuration");
        return 1;
    }

    LOG_INFO("Config: lan_iface=%s fso_iface=%s "
             "k=%d m=%d depth=%d symbol_size=%d",
             cfg.lan_iface, cfg.fso_iface,
             cfg.k, cfg.m, cfg.depth, cfg.symbol_size);

    /* ------------------------------------------------------------------ */
    /* Fragmentation / reassembly self-tests                              */
    /* ------------------------------------------------------------------ */
    LOG_INFO("[TEST] Starting fragmentation/reassembly self-tests");
    run_startup_tests(cfg.symbol_size);
    LOG_INFO("[TEST] Startup self-tests complete");

    /* ------------------------------------------------------------------ */
    /* Block builder                                                       */
    /* ------------------------------------------------------------------ */
    if (block_builder_init(&g_block_builder, cfg.k) != 0) {
        LOG_ERROR("block_builder_init() failed for k=%d", cfg.k);
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* Raw capture socket                                                  */
    /* ------------------------------------------------------------------ */
    raw_fd = net_open_raw_socket(cfg.lan_iface);
    if (raw_fd < 0) {
        LOG_ERROR("net_open_raw_socket() failed on %s", cfg.lan_iface);
        block_builder_destroy(&g_block_builder);
        return 1;
    }

    pfd.fd      = raw_fd;
    pfd.events  = POLLIN;
    pfd.revents = 0;

    LOG_INFO("Raw socket open on %s — entering sniffing loop", cfg.lan_iface);

    /* ================================================================== */
    /* Packet-sniffing loop                                                */
    /* ================================================================== */
    while (g_running) {
        int poll_rc;

        pfd.revents = 0;
        poll_rc     = poll(&pfd, 1, POLL_TIMEOUT_MS);

        if (poll_rc < 0) {
            if (errno == EINTR) { continue; }
            LOG_ERROR("poll() failed: %s", strerror(errno));
            exit_code = 1;
            break;
        }

        /* Timeout — check for stalled block */
        if (poll_rc == 0) {
            process_block_timeout_if_needed();
            continue;
        }

        /* ---- Incoming data ------------------------------------------- */
        if ((pfd.revents & POLLIN) != 0) {
            ssize_t rx_len;

            rx_len = recv(raw_fd, rx_buffer, sizeof(rx_buffer), 0);

            if (rx_len < 0) {
                if (errno == EINTR) { continue; }
                LOG_ERROR("recv() failed: %s", strerror(errno));
                exit_code = 1;
                break;
            }

            if (rx_len == 0) {
                LOG_WARN("Empty frame — skipping");
                continue;
            }


            ++packet_id;

            LOG_DEBUG("Packet %u captured: %zd bytes", packet_id, rx_len);

            {
                symbol_t     *sym_buf;
                unsigned char recon_buf[RECONSTRUCTED_BUFFER_SIZE];
                int           num_symbols;
                int           recon_len;

                sym_buf = (symbol_t *)malloc(
                    sizeof(symbol_t) * SYMBOL_BUFFER_SIZE);

                if (sym_buf == NULL) {
                    LOG_ERROR("malloc() failed for symbol buffer "
                              "(packet %u)", packet_id);
                    continue;
                }

                memset(sym_buf,   0, sizeof(symbol_t) * SYMBOL_BUFFER_SIZE);
                memset(recon_buf, 0, sizeof(recon_buf));

                num_symbols = fragment_packet(rx_buffer,
                                              (size_t)rx_len,
                                              packet_id,
                                              (uint16_t)cfg.symbol_size,
                                              sym_buf,
                                              SYMBOL_BUFFER_SIZE);

                if (num_symbols < 0) {
                    LOG_ERROR("fragment_packet() failed (packet %u)",
                              packet_id);
                    free(sym_buf);
                    continue;
                }

                LOG_DEBUG("Packet %u -> %d symbols", packet_id, num_symbols);

                recon_len = reassemble_packet(sym_buf,
                                             (uint16_t)num_symbols,
                                             recon_buf,
                                             sizeof(recon_buf));

                if (recon_len < 0) {
                    LOG_ERROR("reassemble_packet() failed (packet %u)",
                              packet_id);
                    free(sym_buf);
                    continue;
                }

                if ((size_t)recon_len != (size_t)rx_len) {
                    LOG_ERROR("Packet %u reassembly length mismatch: "
                              "expected=%zd got=%d",
                              packet_id, rx_len, recon_len);
                    free(sym_buf);
                    continue;
                }

                if (memcmp(rx_buffer, recon_buf, (size_t)rx_len) != 0) {
                    LOG_ERROR("Packet %u reassembly payload mismatch",
                              packet_id);
                    free(sym_buf);
                    continue;
                }

                LOG_DEBUG("Packet %u reassembly OK", packet_id);

                process_incoming_symbols(sym_buf, num_symbols);
                free(sym_buf);
            }

            continue;
        }

        /* ---- Socket error -------------------------------------------- */
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            LOG_ERROR("poll() socket error: revents=0x%x", pfd.revents);
            exit_code = 1;
            break;
        }
    }

    /* ================================================================== */
    /* Shutdown                                                            */
    /* ================================================================== */

    /* Flush any partial block that was in-flight at shutdown */
    if (g_block_builder.symbol_count > 0) {
        int orig    = g_block_builder.symbol_count;
        int padding = g_block_builder.k_limit - orig;

        block_builder_finalize_with_padding(&g_block_builder);

        LOG_WARN("Shutdown flush: block %lu finalized "
                 "(%d symbols + %d padding)",
                 (unsigned long)g_block_builder.block_id, orig, padding);

        block_builder_reset(&g_block_builder);
    }

    LOG_INFO("Shutting down");
    stats_report();

    if (raw_fd >= 0) {
        close(raw_fd);
    }

    block_builder_destroy(&g_block_builder);

    return exit_code;
}