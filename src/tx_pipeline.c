/*
 * src/tx_pipeline.c — TX pipeline implementation for the FSO Gateway.
 *
 * See include/tx_pipeline.h for the public API.
 *
 * Pipeline:
 *   NIC_LAN receive → fragment → block build → FEC encode →
 *   interleave → NIC_FSO transmit
 *
 * Wire format per symbol (18-byte header + payload):
 *   Offset  Size  Field
 *    0       4    packet_id      big-endian uint32
 *    4       4    fec_id         big-endian uint32
 *    8       2    symbol_index   big-endian uint16
 *   10       2    total_symbols  big-endian uint16
 *   12       2    payload_len    big-endian uint16
 *   14       4    crc32          big-endian uint32
 *   18       N    data[0..payload_len-1]
 */

#define _POSIX_C_SOURCE 200112L

#include <arpa/inet.h>   /* htonl, htons */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arp_cache.h"
#include "block_builder.h"
#include "config.h"
#include "fec_wrapper.h"
#include "interleaver.h"
#include "logging.h"
#include "packet_fragmenter.h"
#include "packet_io.h"
#include "stats.h"
#include "symbol.h"
#include "tx_pipeline.h"
#include "types.h"

/* -------------------------------------------------------------------------- */
/* Internal constants                                                          */
/* -------------------------------------------------------------------------- */

#define TX_BLOCK_TIMEOUT_MS               2.0
#define TX_INTERLEAVER_FLUSH_TIMEOUT_MS   5

/* Receive buffer size — must hold the largest possible Ethernet frame */
#define TX_RX_BUF_SIZE       9200U

/* Maximum packet length supported (jumbo frame) */
#define TX_MAX_PACKET_LEN    9000U

/* Wire header size in bytes */
#define WIRE_HDR_SIZE        18U

/* FSO Ethernet II header prepended to every FEC wire frame.
 * EtherType 0x7FEC (≥ 0x0600) ensures mlx5 classifies the frame as
 * Ethernet II, which the DPDK rte_flow catch-all rule can steer to queue 0.
 * Without this header, payload_len (bytes 12-13) ≈ 1400 < 0x0600, and the
 * NIC treats the frame as IEEE 802.3 — only ~1/N frames reach DPDK via RSS. */
#define FSO_ETH_HDR_SIZE     14U
static const unsigned char FSO_ETH_HDR[FSO_ETH_HDR_SIZE] = {
    0x01, 0x7f, 0x45, 0x43, 0x00, 0x01,  /* dst: multicast, locally-administered */
    0x02, 0x7f, 0x45, 0x43, 0x00, 0x01,  /* src: locally-administered */
    0x7f, 0xec                             /* EtherType 0x7FEC */
};

/* -------------------------------------------------------------------------- */
/* Internal struct                                                             */
/* -------------------------------------------------------------------------- */

struct tx_pipeline {
    struct config     cfg;
    packet_io_ctx_t  *rx_ctx;
    packet_io_ctx_t  *tx_ctx;
    block_builder_t   builder;
    fec_handle_t      fec;
    interleaver_t    *interleaver;
    uint32_t          packet_id_counter;
    unsigned char    *source_buf;
    symbol_t         *frag_syms;
    int               max_frags_per_packet;
    symbol_t         *repair_syms;
    uint8_t           lan_mac[6];
    arp_cache_t      *arp_cache;
};

/* -------------------------------------------------------------------------- */
/* Forward declarations                                                        */
/* -------------------------------------------------------------------------- */

static int encode_and_drain(tx_pipeline_t *pl);
static int tx_serialize_and_send(tx_pipeline_t *pl, const symbol_t *sym);
static void drain_interleaver(tx_pipeline_t *pl);
static int tick_and_drain_interleaver(tx_pipeline_t *pl);
static int try_proxy_arp(tx_pipeline_t *pl,
                         const unsigned char *pkt, size_t pkt_len);

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

tx_pipeline_t *tx_pipeline_create(const struct config *cfg,
                                  packet_io_ctx_t     *rx_ctx,
                                  packet_io_ctx_t     *tx_ctx,
                                  arp_cache_t         *arp_cache)
{
    tx_pipeline_t *pl;
    size_t         source_buf_size;

    if (cfg == NULL) {
        LOG_ERROR("[tx_pipeline] create: cfg is NULL");
        return NULL;
    }
    if (rx_ctx == NULL) {
        LOG_ERROR("[tx_pipeline] create: rx_ctx is NULL");
        return NULL;
    }
    if (tx_ctx == NULL) {
        LOG_ERROR("[tx_pipeline] create: tx_ctx is NULL");
        return NULL;
    }

    pl = (tx_pipeline_t *)malloc(sizeof(tx_pipeline_t));
    if (pl == NULL) {
        LOG_ERROR("[tx_pipeline] create: malloc(tx_pipeline_t) failed");
        return NULL;
    }
    memset(pl, 0, sizeof(tx_pipeline_t));

    pl->cfg = *cfg;
    /* Read LAN interface MAC to filter out self-injected frames */
    {
        char sys_path[64];
        FILE *f;
        snprintf(sys_path, sizeof(sys_path),
                 "/sys/class/net/%s/address", cfg->lan_iface);
        f = fopen(sys_path, "r");
        if (f) {
            unsigned int m[6] = {0};
            fscanf(f, "%x:%x:%x:%x:%x:%x",
                   &m[0],&m[1],&m[2],&m[3],&m[4],&m[5]);
            fclose(f);
            for (int mi = 0; mi < 6; mi++)
                pl->lan_mac[mi] = (uint8_t)m[mi];
            LOG_INFO("[tx_pipeline] LAN MAC filter: "
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     m[0],m[1],m[2],m[3],m[4],m[5]);
        }
    }
    pl->rx_ctx    = rx_ctx;
    pl->tx_ctx    = tx_ctx;
    pl->arp_cache = arp_cache;
    pl->packet_id_counter = 0;

    source_buf_size = (size_t)cfg->k * (size_t)cfg->symbol_size;
    pl->source_buf = (unsigned char *)malloc(source_buf_size);
    if (pl->source_buf == NULL) {
        LOG_ERROR("[tx_pipeline] create: malloc(source_buf, %zu) failed",
                  source_buf_size);
        free(pl);
        return NULL;
    }

    if (block_builder_init(&pl->builder, cfg->k, cfg->symbol_size) != 0) {
        LOG_ERROR("[tx_pipeline] create: block_builder_init(k=%d,sym=%d) failed",
                  cfg->k, cfg->symbol_size);
        free(pl->source_buf);
        free(pl);
        return NULL;
    }

    pl->fec = fec_create(cfg->k, cfg->symbol_size);
    if (pl->fec == NULL) {
        LOG_ERROR("[tx_pipeline] create: fec_create(k=%d, sym=%d) failed",
                  cfg->k,
                  cfg->symbol_size);
        block_builder_destroy(&pl->builder);
        free(pl->source_buf);
        free(pl);
        return NULL;
    }

    pl->interleaver = interleaver_create(cfg->depth,
                                         cfg->k + cfg->m,
                                         cfg->symbol_size,
                                         TX_INTERLEAVER_FLUSH_TIMEOUT_MS);
    if (pl->interleaver == NULL) {
        LOG_ERROR("[tx_pipeline] create: interleaver_create(d=%d,n=%d,s=%d,flush=%d) failed",
                  cfg->depth,
                  cfg->k + cfg->m,
                  cfg->symbol_size,
                  TX_INTERLEAVER_FLUSH_TIMEOUT_MS);
        fec_destroy(pl->fec);
        block_builder_destroy(&pl->builder);
        free(pl->source_buf);
        free(pl);
        return NULL;
    }

    pl->max_frags_per_packet = (int)((TX_MAX_PACKET_LEN + (size_t)cfg->symbol_size - 1U)
                                     / (size_t)cfg->symbol_size);
    if (pl->max_frags_per_packet < cfg->k) {
        pl->max_frags_per_packet = cfg->k;
    }

    pl->frag_syms = (symbol_t *)malloc(sizeof(symbol_t) * (size_t)pl->max_frags_per_packet);
    if (pl->frag_syms == NULL) {
        LOG_ERROR("[tx_pipeline] create: malloc(frag_syms, max_frags=%d) failed",
                  pl->max_frags_per_packet);
        interleaver_destroy(pl->interleaver);
        fec_destroy(pl->fec);
        block_builder_destroy(&pl->builder);
        free(pl->source_buf);
        free(pl);
        return NULL;
    }

    pl->repair_syms = (symbol_t *)malloc(sizeof(symbol_t) * (size_t)cfg->m);
    if (pl->repair_syms == NULL) {
        LOG_ERROR("[tx_pipeline] create: malloc(repair_syms, m=%d) failed",
                  cfg->m);
        free(pl->frag_syms);
        interleaver_destroy(pl->interleaver);
        fec_destroy(pl->fec);
        block_builder_destroy(&pl->builder);
        free(pl->source_buf);
        free(pl);
        return NULL;
    }

    LOG_INFO("[tx_pipeline] Created: k=%d m=%d depth=%d symbol_size=%d",
             cfg->k,
             cfg->m,
             cfg->depth,
             cfg->symbol_size);

    return pl;
}

void tx_pipeline_destroy(tx_pipeline_t *pl)
{
    if (pl == NULL) {
        return;
    }

    interleaver_destroy(pl->interleaver);
    fec_destroy(pl->fec);
    block_builder_destroy(&pl->builder);
    free(pl->repair_syms);
    free(pl->frag_syms);
    free(pl->source_buf);
    free(pl);
}

/* -------------------------------------------------------------------------- */
/* tx_pipeline_run_once                                                        */
/* -------------------------------------------------------------------------- */

int tx_pipeline_run_once(tx_pipeline_t *pl)
{
    unsigned char rx_buf[TX_RX_BUF_SIZE];
    size_t        pkt_len;
    int           rc;
    int           num_frags;
    int           i;
    int           add_rc;
    int           timeout_rc;

    if (pl == NULL) {
        LOG_ERROR("[tx_pipeline] run_once: pl is NULL");
        return -1;
    }

    pkt_len = 0;

    rc = packet_io_receive(pl->rx_ctx, rx_buf, sizeof(rx_buf), &pkt_len);

    if (rc == 0) {
        timeout_rc = block_builder_check_timeout(&pl->builder,
                                                 TX_BLOCK_TIMEOUT_MS);
        if (timeout_rc == -1) {
            LOG_ERROR("[tx_pipeline] run_once: block_builder_check_timeout failed");
            return -1;
        }

        if (timeout_rc == 1 && pl->builder.symbol_count > 0) {
            LOG_INFO("[tx_pipeline] idle-timeout flush: block_id=%lu symbols=%d",
                     pl->builder.block_id, pl->builder.symbol_count);
            block_builder_finalize_with_padding(&pl->builder);
            if (encode_and_drain(pl) != 0) {
                return -1;
            }
            block_builder_reset(&pl->builder);
        }

        if (tick_and_drain_interleaver(pl) != 0) {
            return -1;
        }

        return 0;
    }

    if (rc == -1) {
        LOG_WARN("[tx_pipeline] run_once: packet_io_receive error: %s",
                 packet_io_last_error(pl->rx_ctx));
        return 0;
    }

    /* Drop frames injected by this gateway (src MAC == lan interface MAC) */
    if (pkt_len >= 12 && memcmp(rx_buf + 6, pl->lan_mac, 6) == 0) {
        return 0;
    }

    /* Proxy-ARP: answer ARP requests locally if target IP is cached */
    if (pl->arp_cache != NULL) {
        int proxy_rc = try_proxy_arp(pl, rx_buf, pkt_len);
        if (proxy_rc == 1) {
            return 0;   /* answered locally, don't forward via FSO */
        }
    }

    /* Real LAN packet accepted for forwarding — count as ingress. */
    stats_inc_ingress(pkt_len);

    num_frags = fragment_packet(rx_buf,
                                pkt_len,
                                pl->packet_id_counter,
                                (uint16_t)pl->cfg.symbol_size,
                                pl->frag_syms,
                                (uint16_t)pl->max_frags_per_packet);
    if (num_frags < 0) {
        LOG_WARN("[tx_pipeline] run_once: fragment_packet failed "
                 "(pkt_len=%zu, packet_id=%u)",
                 pkt_len,
                 pl->packet_id_counter);
        return 0;
    }

    pl->packet_id_counter++;

    for (i = 0; i < num_frags; ++i) {
        add_rc = block_builder_add_symbol(&pl->builder, &pl->frag_syms[i]);
        if (add_rc == -1) {
            LOG_WARN("[tx_pipeline] run_once: block_builder_add_symbol failed at frag %d",
                     i);
            continue;
        }

        if (add_rc == 1) {
            if (encode_and_drain(pl) != 0) {
                return -1;
            }
            block_builder_reset(&pl->builder);
            if (tick_and_drain_interleaver(pl) != 0) {
                return -1;
            }
        }
    }

    /* Check block timeout even when packets keep arriving — without this,
     * a partial block containing a SYN packet can be stuck indefinitely
     * behind a stream of background LAN traffic (IPv6 ND, ARP broadcasts). */
    timeout_rc = block_builder_check_timeout(&pl->builder, TX_BLOCK_TIMEOUT_MS);
    if (timeout_rc == -1) {
        LOG_ERROR("[tx_pipeline] run_once: block_builder_check_timeout failed");
        return -1;
    }
    if (timeout_rc == 1 && pl->builder.symbol_count > 0) {
        LOG_INFO("[tx_pipeline] busy-timeout flush: block_id=%lu symbols=%d",
                 pl->builder.block_id, pl->builder.symbol_count);
        block_builder_finalize_with_padding(&pl->builder);
        if (encode_and_drain(pl) != 0) {
            return -1;
        }
        block_builder_reset(&pl->builder);
    }

    if (tick_and_drain_interleaver(pl) != 0) {
        return -1;
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* encode_and_drain — internal helper                                          */
/* -------------------------------------------------------------------------- */

static int encode_and_drain(tx_pipeline_t *pl)
{
    int      k;
    int      m;
    int      sym_size;
    int      crc_on;
    uint32_t block_id;
    int      i;
    int      rc;
    symbol_t out_sym;

    if (pl == NULL) {
        LOG_ERROR("[tx_pipeline] encode_and_drain: pl is NULL");
        return -1;
    }

    k = pl->cfg.k;
    m = pl->cfg.m;
    sym_size = pl->cfg.symbol_size;
    crc_on = pl->cfg.internal_symbol_crc_enabled;

    for (i = 0; i < k; ++i) {
        memcpy(pl->source_buf + (size_t)i * (size_t)sym_size,
               pl->builder.symbols[i].data,
               (size_t)sym_size);
    }

    rc = fec_encode_block(pl->fec, pl->source_buf, pl->repair_syms, m);
    if (rc != 0) {
        LOG_ERROR("[tx_pipeline] encode_and_drain: fec_encode_block failed");
        return -1;
    }

    block_id = (uint32_t)pl->builder.block_id;

    for (i = 0; i < k; ++i) {
        pl->builder.symbols[i].packet_id    = block_id;
        pl->builder.symbols[i].fec_id       = (uint32_t)i;
        /* symbol_index and total_symbols come from fragment_packet() or
         * block_builder_finalize_with_padding() and must not be overwritten:
         * the reassembler uses total_symbols to count original packet fragments. */
    }
    for (i = 0; i < m; ++i) {
        pl->repair_syms[i].packet_id = pl->builder.symbols[0].packet_id;
        pl->repair_syms[i].fec_id = (uint32_t)(k + i);
        pl->repair_syms[i].symbol_index = (uint16_t)(k + i);
        pl->repair_syms[i].total_symbols = (uint16_t)(k + m);
        pl->repair_syms[i].payload_len = (uint16_t)sym_size;
    }

    if (crc_on) {
        for (i = 0; i < k; ++i) {
            symbol_compute_crc(&pl->builder.symbols[i]);
        }
        for (i = 0; i < m; ++i) {
            symbol_compute_crc(&pl->repair_syms[i]);
        }
    }

    while (interleaver_is_ready(pl->interleaver)) {
        rc = interleaver_pop_ready_symbol(pl->interleaver, &out_sym);
        if (rc == -1) {
            break;
        }
        tx_serialize_and_send(pl, &out_sym);
        if (rc == 1) {
            break;
        }
    }

    for (i = 0; i < k; ++i) {
        rc = interleaver_push_symbol(pl->interleaver, &pl->builder.symbols[i]);
        if (rc == -1) {
            LOG_ERROR("[tx_pipeline] encode_and_drain: interleaver_push_symbol(src %d) failed",
                      i);
            return -1;
        }
    }

    for (i = 0; i < m; ++i) {
        rc = interleaver_push_symbol(pl->interleaver, &pl->repair_syms[i]);
        if (rc == -1) {
            LOG_ERROR("[tx_pipeline] encode_and_drain: interleaver_push_symbol(repair %d) failed",
                      i);
            return -1;
        }
    }

    drain_interleaver(pl);

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Interleaver drain helpers                                                   */
/* -------------------------------------------------------------------------- */

static void drain_interleaver(tx_pipeline_t *pl)
{
    int      rc;
    symbol_t out_sym;

    if (pl == NULL) {
        LOG_ERROR("[tx_pipeline] drain_interleaver: pl is NULL");
        return;
    }

    if (!interleaver_is_ready(pl->interleaver)) {
        return;
    }

    while (1) {
        rc = interleaver_pop_ready_symbol(pl->interleaver, &out_sym);
        if (rc == -1) {
            break;
        }
        if (tx_serialize_and_send(pl, &out_sym) != 0) {
            /* LOG_WARN already emitted inside; continue best-effort */
        }
        if (rc == 1) {
            break;
        }
    }
}

static int tick_and_drain_interleaver(tx_pipeline_t *pl)
{
    int rc;

    if (pl == NULL) {
        LOG_ERROR("[tx_pipeline] tick_and_drain_interleaver: pl is NULL");
        return -1;
    }

    rc = interleaver_tick(pl->interleaver);
    if (rc == -1) {
        LOG_ERROR("[tx_pipeline] tick_and_drain_interleaver: interleaver_tick failed");
        return -1;
    }

    if (rc == 1) {
        drain_interleaver(pl);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* tx_serialize_and_send — internal helper                                     */
/* -------------------------------------------------------------------------- */

static int tx_serialize_and_send(tx_pipeline_t *pl, const symbol_t *sym)
{
    unsigned char wire[FSO_ETH_HDR_SIZE + WIRE_HDR_SIZE + MAX_SYMBOL_DATA_SIZE];
    uint32_t      tmp32;
    uint16_t      tmp16;
    size_t        wire_len;

    if (pl == NULL || sym == NULL) {
        LOG_ERROR("[tx_pipeline] tx_serialize_and_send: invalid argument");
        return -1;
    }

    memcpy(wire, FSO_ETH_HDR, FSO_ETH_HDR_SIZE);

    tmp32 = htonl(sym->packet_id);
    memcpy(wire + FSO_ETH_HDR_SIZE + 0, &tmp32, 4);

    tmp32 = htonl(sym->fec_id);
    memcpy(wire + FSO_ETH_HDR_SIZE + 4, &tmp32, 4);

    tmp16 = htons(sym->symbol_index);
    memcpy(wire + FSO_ETH_HDR_SIZE + 8, &tmp16, 2);

    tmp16 = htons(sym->total_symbols);
    memcpy(wire + FSO_ETH_HDR_SIZE + 10, &tmp16, 2);

    tmp16 = htons(sym->payload_len);
    memcpy(wire + FSO_ETH_HDR_SIZE + 12, &tmp16, 2);

    tmp32 = htonl(sym->crc32);
    memcpy(wire + FSO_ETH_HDR_SIZE + 14, &tmp32, 4);

    if (sym->payload_len > 0) {
        memcpy(wire + FSO_ETH_HDR_SIZE + WIRE_HDR_SIZE, sym->data, sym->payload_len);
    }

    wire_len = FSO_ETH_HDR_SIZE + WIRE_HDR_SIZE + sym->payload_len;

    if (packet_io_send(pl->tx_ctx, wire, wire_len) != 0) {
        LOG_WARN("[tx_pipeline] tx_serialize_and_send: packet_io_send failed "
                 "(packet_id=%u fec_id=%u): %s",
                 sym->packet_id,
                 sym->fec_id,
                 packet_io_last_error(pl->tx_ctx));
        return -1;
    }

    /* Wire-level transmitted counter (per symbol, including FEC repair). */
    stats_inc_transmitted(wire_len);

    return 0;
}

/* -------------------------------------------------------------------------- */
/* try_proxy_arp — answer ARP requests locally from cache                     */
/* -------------------------------------------------------------------------- */

/*
 * ARP packet layout (Ethernet frame, 42 bytes minimum):
 *   [0..5]   dst_mac
 *   [6..11]  src_mac
 *   [12..13] ethertype (0x0806)
 *   [14..15] htype (0x0001)
 *   [16..17] ptype (0x0800)
 *   [18]     hlen (6)
 *   [19]     plen (4)
 *   [20..21] oper (1=request, 2=reply)
 *   [22..27] sender MAC
 *   [28..31] sender IP
 *   [32..37] target MAC
 *   [38..41] target IP
 *
 * Returns 1 if an ARP reply was sent (caller should NOT forward via FSO).
 * Returns 0 if not handled (caller proceeds normally).
 */
static int try_proxy_arp(tx_pipeline_t       *pl,
                         const unsigned char *pkt,
                         size_t               pkt_len)
{
    uint32_t      target_ip;
    uint8_t       cached_mac[6];
    unsigned char reply[60];  /* minimum Ethernet frame */
    uint16_t      oper;

    /* Minimum length: 14 (Ethernet hdr) + 28 (ARP body) = 42 */
    if (pkt_len < 42) {
        return 0;
    }

    /* Check EtherType == 0x0806 */
    if (pkt[12] != 0x08 || pkt[13] != 0x06) {
        return 0;
    }

    /* Check ARP oper == 1 (request) */
    memcpy(&oper, pkt + 20, 2);
    if (oper != htons(1)) {
        return 0;
    }

    /* Extract target IP (network byte order, 4 bytes at offset 38) */
    memcpy(&target_ip, pkt + 38, 4);

    /* Look up target IP in cache */
    if (!arp_cache_lookup(pl->arp_cache, target_ip, cached_mac)) {
        return 0;
    }

    LOG_INFO("[tx_pipeline] proxy-ARP: answering request for "
             "%u.%u.%u.%u with cached MAC %02x:%02x:%02x:%02x:%02x:%02x",
             pkt[38], pkt[39], pkt[40], pkt[41],
             cached_mac[0], cached_mac[1], cached_mac[2],
             cached_mac[3], cached_mac[4], cached_mac[5]);

    /* Build ARP reply */
    memset(reply, 0, sizeof(reply));

    /* Ethernet header */
    memcpy(reply + 0, pkt + 6, 6);       /* dst = requester MAC */
    memcpy(reply + 6, cached_mac, 6);    /* src = cached (target) MAC */
    reply[12] = 0x08;
    reply[13] = 0x06;

    /* ARP body */
    reply[14] = 0x00; reply[15] = 0x01;  /* htype = Ethernet */
    reply[16] = 0x08; reply[17] = 0x00;  /* ptype = IPv4 */
    reply[18] = 6;                        /* hlen */
    reply[19] = 4;                        /* plen */
    reply[20] = 0x00; reply[21] = 0x02;  /* oper = reply */
    memcpy(reply + 22, cached_mac, 6);   /* sender MAC = cached (target) MAC */
    memcpy(reply + 28, pkt + 38, 4);     /* sender IP  = target IP from request */
    memcpy(reply + 32, pkt + 22, 6);     /* target MAC = requester (ARP sender MAC) */
    memcpy(reply + 38, pkt + 28, 4);     /* target IP  = sender IP from request */

    if (packet_io_send(pl->rx_ctx, reply, sizeof(reply)) != 0) {
        LOG_WARN("[tx_pipeline] proxy-ARP: packet_io_send failed: %s",
                 packet_io_last_error(pl->rx_ctx));
        return 0;
    }

    return 1;
}
