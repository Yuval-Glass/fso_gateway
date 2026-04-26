/*
 * src/rx_pipeline.c — RX pipeline implementation for the FSO Gateway.
 *
 * See include/rx_pipeline.h for the public API.
 *
 * Pipeline:
 *   NIC_FSO receive → deinterleave → FEC decode → reassemble → NIC_LAN transmit
 *
 * Wire format (receive direction, same layout as TX emits):
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

#include <arpa/inet.h>   /* ntohl, ntohs */
#include <stdlib.h>
#include <string.h>

#include "arp_cache.h"
#include "config.h"
#include "deinterleaver.h"
#include "fec_wrapper.h"
#include "logging.h"
#include "packet_io.h"
#include "packet_reassembler.h"
#include "rx_pipeline.h"
#include "stats.h"
#include "symbol.h"
#include "types.h"

/* -------------------------------------------------------------------------- */
/* Internal constants                                                          */
/* -------------------------------------------------------------------------- */

/* Receive buffer — must hold the largest possible wire frame */
#define RX_WIRE_BUF_SIZE     9200U

/* Wire header size in bytes */
#define WIRE_HDR_SIZE        18U

/* block_max_age for the deinterleaver */
#define RX_BLOCK_MAX_AGE_MS  50.0

/* Output buffer for reassemble_packet — max Ethernet jumbo frame */
#define REASSEM_BUF_SIZE     9200U

/* -------------------------------------------------------------------------- */
/* Internal struct                                                             */
/* -------------------------------------------------------------------------- */

struct rx_pipeline {
    struct config     cfg;        /* copy of runtime configuration            */
    packet_io_ctx_t  *rx_ctx;    /* FSO NIC — receive side                   */
    packet_io_ctx_t  *tx_ctx;    /* LAN NIC — transmit side                  */
    deinterleaver_t  *dil;       /* block collector / deinterleaver           */
    fec_handle_t      fec;       /* Wirehair FEC context                      */
    unsigned char    *recon_buf; /* k * symbol_size, heap-allocated           */
    block_t          *block_buf;     /* heap-allocated, one block_t            */
    symbol_t         *pkt_syms_buf;  /* heap, MAX_SYMBOLS_PER_BLOCK elements   */
    arp_cache_t      *arp_cache;    /* optional: populate on decoded ARP pkts */
};

/* -------------------------------------------------------------------------- */
/* Forward declaration                                                         */
/* -------------------------------------------------------------------------- */

static void drain_ready_blocks(rx_pipeline_t *pl);
static void arp_learn_from_packet(rx_pipeline_t *pl,
                                  const unsigned char *pkt, int pkt_len);

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

rx_pipeline_t *rx_pipeline_create(const struct config *cfg,
                                  packet_io_ctx_t     *rx_ctx,
                                  packet_io_ctx_t     *tx_ctx,
                                  arp_cache_t         *arp_cache)
{
    rx_pipeline_t *pl;
    size_t         recon_size;

    if (cfg == NULL) {
        LOG_ERROR("[rx_pipeline] create: cfg is NULL");
        return NULL;
    }
    if (rx_ctx == NULL) {
        LOG_ERROR("[rx_pipeline] create: rx_ctx is NULL");
        return NULL;
    }
    if (tx_ctx == NULL) {
        LOG_ERROR("[rx_pipeline] create: tx_ctx is NULL");
        return NULL;
    }

    pl = (rx_pipeline_t *)malloc(sizeof(rx_pipeline_t));
    if (pl == NULL) {
        LOG_ERROR("[rx_pipeline] create: malloc(rx_pipeline_t) failed");
        return NULL;
    }
    memset(pl, 0, sizeof(rx_pipeline_t));

    pl->cfg       = *cfg;
    pl->rx_ctx    = rx_ctx;
    pl->tx_ctx    = tx_ctx;
    pl->arp_cache = arp_cache;

    /* Heap-allocate recon_buf (k * symbol_size can be up to ~576 kB) */
    recon_size = (size_t)cfg->k * (size_t)cfg->symbol_size;
    pl->recon_buf = (unsigned char *)malloc(recon_size);
    if (pl->recon_buf == NULL) {
        LOG_ERROR("[rx_pipeline] create: malloc(recon_buf, %zu) failed",
                  recon_size);
        free(pl);
        return NULL;
    }

    /* Create FEC context */
    pl->fec = fec_create(cfg->k, cfg->symbol_size);
    if (pl->fec == NULL) {
        LOG_ERROR("[rx_pipeline] create: fec_create(k=%d, sym=%d) failed",
                  cfg->k, cfg->symbol_size);
        free(pl->recon_buf);
        free(pl);
        return NULL;
    }

    /* Create deinterleaver */
    pl->dil = deinterleaver_create(cfg->depth * 4,
                                   cfg->k + cfg->m,
                                   cfg->k,
                                   (size_t)cfg->symbol_size,
                                   0.0,
                                   RX_BLOCK_MAX_AGE_MS);
    if (pl->dil == NULL) {
        LOG_ERROR("[rx_pipeline] create: deinterleaver_create failed "
                  "(depth*4=%d, k+m=%d, k=%d, sym=%d)",
                  cfg->depth * 4, cfg->k + cfg->m, cfg->k, cfg->symbol_size);
        fec_destroy(pl->fec);
        free(pl->recon_buf);
        free(pl);
        return NULL;
    }

    /* Heap-allocate block_buf (sizeof(block_t) ~ 2.3 MB — too large for stack) */
    pl->block_buf = (block_t *)malloc(sizeof(block_t));
    if (pl->block_buf == NULL) {
        LOG_ERROR("[rx_pipeline] create: malloc(block_buf) failed");
        deinterleaver_destroy(pl->dil);
        fec_destroy(pl->fec);
        free(pl->recon_buf);
        free(pl);
        return NULL;
    }

    /* Heap-allocate pkt_syms_buf (256 × ~9024 bytes — too large for stack) */
    pl->pkt_syms_buf = (symbol_t *)malloc(
        sizeof(symbol_t) * MAX_SYMBOLS_PER_BLOCK);
    if (pl->pkt_syms_buf == NULL) {
        LOG_ERROR("[rx_pipeline] create: malloc(pkt_syms_buf) failed");
        free(pl->block_buf);
        deinterleaver_destroy(pl->dil);
        fec_destroy(pl->fec);
        free(pl->recon_buf);
        free(pl);
        return NULL;
    }

    LOG_INFO("[rx_pipeline] Created: k=%d m=%d depth=%d symbol_size=%d",
             cfg->k, cfg->m, cfg->depth, cfg->symbol_size);

    return pl;
}

void rx_pipeline_destroy(rx_pipeline_t *pl)
{
    if (pl == NULL) {
        return;
    }

    deinterleaver_destroy(pl->dil);
    fec_destroy(pl->fec);
    free(pl->pkt_syms_buf);
    free(pl->block_buf);
    free(pl->recon_buf);
    free(pl);
}

deinterleaver_t *rx_pipeline_get_deinterleaver(rx_pipeline_t *pl)
{
    return (pl != NULL) ? pl->dil : NULL;
}

/* -------------------------------------------------------------------------- */
/* rx_pipeline_run_once                                                        */
/* -------------------------------------------------------------------------- */

int rx_pipeline_run_once(rx_pipeline_t *pl)
{
    unsigned char wire_buf[RX_WIRE_BUF_SIZE];
    size_t        wire_len;
    int           rc;
    symbol_t      sym;
    uint32_t      tmp32;
    uint16_t      tmp16;

    if (pl == NULL) {
        LOG_ERROR("[rx_pipeline] run_once: pl is NULL");
        return -1;
    }

    wire_len = 0;

    /* ------------------------------------------------------------------ */
    /* Step 1 — Receive from FSO NIC                                       */
    /* ------------------------------------------------------------------ */
    rc = packet_io_receive(pl->rx_ctx, wire_buf, sizeof(wire_buf), &wire_len);

    if (rc == 0) {
        /* No packet — tick the deinterleaver and try to drain */
        deinterleaver_tick(pl->dil, -1.0);
        drain_ready_blocks(pl);
        return 0;
    }

    if (rc == -1) {
        LOG_WARN("[rx_pipeline] run_once: packet_io_receive error: %s",
                 packet_io_last_error(pl->rx_ctx));
        return 0;
    }

    /* rc == 1: packet received */

    /* ------------------------------------------------------------------ */
    /* Step 2 — Deserialize wire format → symbol_t                        */
    /* ------------------------------------------------------------------ */

    /* Minimum header check */
    if (wire_len < WIRE_HDR_SIZE) {
        LOG_DEBUG("[rx_pipeline] run_once: frame too short (%zu < %u), "
                  "dropping", wire_len, WIRE_HDR_SIZE);
        return 0;
    }

    memset(&sym, 0, sizeof(sym));

    memcpy(&tmp32, wire_buf + 0,  4); sym.packet_id     = ntohl(tmp32);
    memcpy(&tmp32, wire_buf + 4,  4); sym.fec_id        = ntohl(tmp32);
    memcpy(&tmp16, wire_buf + 8,  2); sym.symbol_index  = ntohs(tmp16);
    memcpy(&tmp16, wire_buf + 10, 2); sym.total_symbols = ntohs(tmp16);
    memcpy(&tmp16, wire_buf + 12, 2); sym.payload_len   = ntohs(tmp16);
    memcpy(&tmp32, wire_buf + 14, 4); sym.crc32         = ntohl(tmp32);

    if (sym.packet_id == 0 && sym.payload_len == 0) {
        /* Interleaver empty-row padding: packet_id=0 marks unfilled rows.
         * Block-builder zero-padding symbols (packet_id != 0, payload_len=0)
         * are valid source symbols and must reach the FEC decoder.          */
        LOG_DEBUG("[rx_pipeline] padding symbol packet_id=%u fec_id=%u -> erasure",
                  (unsigned)sym.packet_id,
                  (unsigned)sym.fec_id);
        deinterleaver_push_symbol(pl->dil, &sym);
        drain_ready_blocks(pl);
        return 0;
    }

    /* Validate payload_len */
    if ((int)sym.payload_len > pl->cfg.symbol_size) {
        LOG_DEBUG("[rx_pipeline] run_once: payload_len=%u > symbol_size=%d, "
                  "dropping", sym.payload_len, pl->cfg.symbol_size);
        return 0;
    }
    if (sym.payload_len > MAX_SYMBOL_DATA_SIZE) {
        LOG_DEBUG("[rx_pipeline] run_once: payload_len=%u > "
                  "MAX_SYMBOL_DATA_SIZE=%u, dropping",
                  sym.payload_len, MAX_SYMBOL_DATA_SIZE);
        return 0;
    }
    if (wire_len < WIRE_HDR_SIZE + sym.payload_len) {
        LOG_DEBUG("[rx_pipeline] run_once: frame truncated "
                  "(wire_len=%zu < hdr+payload=%u), dropping",
                  wire_len, WIRE_HDR_SIZE + sym.payload_len);
        return 0;
    }

    memcpy(sym.data, wire_buf + WIRE_HDR_SIZE, sym.payload_len);

    /* CRC check */
    if (pl->cfg.internal_symbol_crc_enabled) {
        if (symbol_verify_crc(&sym) == 0) {
            deinterleaver_inc_crc_drop(pl->dil);
            stats_inc_crc_drop_symbol();
            LOG_DEBUG("[rx_pipeline] run_once: CRC fail — "
                      "packet_id=%u fec_id=%u dropped as erasure",
                      sym.packet_id, sym.fec_id);
            return 0;
        }
    }

    /* Push into deinterleaver */
    deinterleaver_push_symbol(pl->dil, &sym);

    /* ------------------------------------------------------------------ */
    /* Step 3 — Drain ready blocks                                         */
    /* ------------------------------------------------------------------ */
    drain_ready_blocks(pl);

    return 0;
}

/* -------------------------------------------------------------------------- */
/* drain_ready_blocks — internal helper                                        */
/* -------------------------------------------------------------------------- */

/*
 * Drain all blocks that the deinterleaver has promoted to READY_TO_DECODE.
 * For each block: FEC-decode → reconstruct source symbols → group by
 * packet_id → reassemble → send to LAN NIC.
 * deinterleaver_mark_result() is called unconditionally for every block.
 */
static void drain_ready_blocks(rx_pipeline_t *pl)
{
    int           fec_rc;
    int           k;
    int           sym_size;
    int           i;
    size_t        recon_size;

    /* Per-packet_id grouping array — heap-allocated via pl->pkt_syms_buf */
    int           pkt_sym_count;
    uint32_t      cur_packet_id;
    unsigned char reassem_buf[REASSEM_BUF_SIZE];
    int           reassem_len;

    k        = pl->cfg.k;
    sym_size = pl->cfg.symbol_size;
    recon_size = (size_t)k * (size_t)sym_size;

    while (deinterleaver_get_ready_block(pl->dil, pl->block_buf) == 0) {

        /* ---- a. Per-block accounting (before FEC decode) --------------- */
        {
            int n = pl->block_buf->symbols_per_block;
            int c = pl->block_buf->symbol_count;
            int holes = (n > c) ? (n - c) : 0;

            stats_inc_block_attempt();
            stats_add_symbols((uint64_t)n, (uint64_t)holes);
            stats_record_block((uint64_t)holes);
        }

        /* ---- b. FEC decode -------------------------------------------- */
        memset(pl->recon_buf, 0, recon_size);

        fec_rc = fec_decode_block(pl->fec,
                                  pl->block_buf->symbols,
                                  pl->block_buf->symbol_count,
                                  pl->block_buf->symbols_per_block,
                                  pl->recon_buf);

        if (fec_rc == FEC_DECODE_TOO_MANY_HOLES || fec_rc == FEC_DECODE_ERR) {
            stats_inc_block_failure();
            LOG_WARN("[rx_pipeline] drain: FEC decode failed "
                     "(block_id=%lu symbol_count=%d rc=%d)",
                     (unsigned long)pl->block_buf->block_id,
                     pl->block_buf->symbol_count, fec_rc);
            deinterleaver_mark_result(pl->dil,
                                      (uint32_t)pl->block_buf->block_id, 0);
            continue;
        }

        /* FEC decode succeeded for this block. */
        stats_inc_block_success();

        /* ---- b. Reconstruct source symbols from decoded bytes ---------- */
        /*
         * Walk source symbol slots (fec_id < k, payload_len > 0).
         * The metadata in pl->block_buf->symbols[i] (packet_id, symbol_index,
         * total_symbols, payload_len) is authoritative; the payload bytes
         * are taken from the decoded recon_buf at offset i * sym_size.
         *
         * We accumulate symbols per packet_id and call reassemble_packet()
         * as soon as a complete set is collected.
         *
         * Because source symbols within a single FEC block may belong to
         * different packets (a packet may span multiple symbols), we scan
         * all k source slots in a single pass.  When the packet_id changes
         * we flush the accumulated group first.
         *
         * Note: reassemble_packet() requires ALL fragments for a given
         * packet_id.  If total_symbols for a packet exceeds the number of
         * source symbols in this block (i.e. fragments span blocks), we
         * cannot fully reassemble here — we emit what we have and let
         * reassemble_packet() report the error via LOG_WARN.
         */

        pkt_sym_count = 0;
        cur_packet_id = 0;       /* initialised on first valid symbol */

        for (i = 0; i < k; ++i) {
            symbol_t *meta;
            symbol_t  reconstructed;

            /* Only source symbols (fec_id < k) with non-zero payload */
            if ((int)pl->block_buf->symbols[i].fec_id >= k) {
                continue;
            }
            if (pl->block_buf->symbols[i].payload_len == 0) {
                continue;
            }

            meta = &pl->block_buf->symbols[i];

            /* Build the reconstructed symbol */
            memset(&reconstructed, 0, sizeof(reconstructed));
            reconstructed.packet_id     = meta->packet_id;
            reconstructed.fec_id        = meta->fec_id;
            reconstructed.symbol_index  = meta->symbol_index;
            reconstructed.total_symbols = meta->total_symbols;
            LOG_DEBUG("[rx_pipeline] drain: packet_id=%u fec_id=%u symbol_index=%u total_symbols=%u payload_len=%u",
                      (unsigned)meta->packet_id, (unsigned)meta->fec_id,
                      (unsigned)meta->symbol_index, (unsigned)meta->total_symbols,
                      (unsigned)meta->payload_len);
            reconstructed.payload_len   = meta->payload_len;
            memcpy(reconstructed.data,
                   pl->recon_buf + (size_t)i * (size_t)sym_size,
                   meta->payload_len);

            /* Flush the accumulated group when a new original packet starts.
             *
             * Two conditions both indicate a packet boundary:
             *   1. symbol_index == 0: this is the first fragment of a new
             *      original packet.
             *   2. pkt_sym_count >= total_symbols of the first buffered
             *      symbol: all expected fragments for the current packet
             *      have been collected and it is complete.
             *
             * Checking both covers the case where a complete single-fragment
             * packet (total_symbols=1) is followed by a continuation fragment
             * (symbol_index > 0) belonging to a different original packet
             * that spans blocks — without condition 2 the flush would be
             * missed because the continuation fragment does not set
             * symbol_index to 0. */
            if (pkt_sym_count > 0) {
                int complete = (pkt_sym_count >=
                                (int)pl->pkt_syms_buf[0].total_symbols);
                int new_pkt  = (reconstructed.symbol_index == 0);

                if (complete || new_pkt) {
                    memset(reassem_buf, 0, sizeof(reassem_buf));
                    reassem_len = reassemble_packet(pl->pkt_syms_buf,
                                                    (uint16_t)pkt_sym_count,
                                                    reassem_buf,
                                                    sizeof(reassem_buf));
                    if (reassem_len > 0) {
                        arp_learn_from_packet(pl, reassem_buf, reassem_len);
                        stats_inc_recovered((size_t)reassem_len);
                        if (packet_io_send(pl->tx_ctx, reassem_buf,
                                           (size_t)reassem_len) != 0) {
                            LOG_WARN("[rx_pipeline] drain: packet_io_send "
                                     "failed for packet_id=%u: %s",
                                     cur_packet_id,
                                     packet_io_last_error(pl->tx_ctx));
                        }
                    } else {
                        stats_inc_failed_packet();
                        LOG_WARN("[rx_pipeline] drain: reassemble_packet "
                                 "failed for packet_id=%u (sym_count=%d)",
                                 cur_packet_id, pkt_sym_count);
                    }
                    pkt_sym_count = 0;
                }
            }

            if (pkt_sym_count == 0) {
                cur_packet_id = reconstructed.packet_id;
            }

            if (pkt_sym_count < MAX_SYMBOLS_PER_BLOCK) {
                pl->pkt_syms_buf[pkt_sym_count++] = reconstructed;
            } else {
                LOG_WARN("[rx_pipeline] drain: symbol overflow for "
                         "packet_id=%u, dropping symbol", cur_packet_id);
            }

            /* Flush immediately if this symbol completes the packet */
            if (pkt_sym_count > 0 &&
                pkt_sym_count >= (int)pl->pkt_syms_buf[0].total_symbols) {
                memset(reassem_buf, 0, sizeof(reassem_buf));
                reassem_len = reassemble_packet(pl->pkt_syms_buf,
                                                (uint16_t)pkt_sym_count,
                                                reassem_buf,
                                                sizeof(reassem_buf));
                if (reassem_len > 0) {
                    arp_learn_from_packet(pl, reassem_buf, reassem_len);
                    stats_inc_recovered((size_t)reassem_len);
                    if (packet_io_send(pl->tx_ctx, reassem_buf,
                                       (size_t)reassem_len) != 0) {
                        LOG_WARN("[rx_pipeline] drain: packet_io_send failed "
                                 "for packet_id=%u: %s",
                                 cur_packet_id,
                                 packet_io_last_error(pl->tx_ctx));
                    }
                } else {
                    stats_inc_failed_packet();
                    LOG_WARN("[rx_pipeline] drain: reassemble_packet failed "
                             "for packet_id=%u (sym_count=%d)",
                             cur_packet_id, pkt_sym_count);
                }
                pkt_sym_count = 0;
            }
        }

        /* Flush final accumulated group */
        if (pkt_sym_count > 0) {
            memset(reassem_buf, 0, sizeof(reassem_buf));
            reassem_len = reassemble_packet(pl->pkt_syms_buf,
                                            (uint16_t)pkt_sym_count,
                                            reassem_buf,
                                            sizeof(reassem_buf));
            if (reassem_len > 0) {
                arp_learn_from_packet(pl, reassem_buf, reassem_len);
                stats_inc_recovered((size_t)reassem_len);
                if (packet_io_send(pl->tx_ctx, reassem_buf,
                                   (size_t)reassem_len) != 0) {
                    LOG_WARN("[rx_pipeline] drain: packet_io_send failed "
                             "for packet_id=%u: %s",
                             cur_packet_id,
                             packet_io_last_error(pl->tx_ctx));
                }
            } else {
                stats_inc_failed_packet();
                LOG_WARN("[rx_pipeline] drain: reassemble_packet failed "
                         "for packet_id=%u (sym_count=%d)",
                         cur_packet_id, pkt_sym_count);
            }
        }

        /* ---- c. Mark block as successfully processed ------------------- */
        deinterleaver_mark_result(pl->dil,
                                  (uint32_t)pl->block_buf->block_id, 1);
    }
}

/* -------------------------------------------------------------------------- */
/* arp_learn_from_packet — populate arp_cache from decoded ARP packets         */
/* -------------------------------------------------------------------------- */

/*
 * Inspect a fully reassembled packet.  If it is an ARP packet (EtherType
 * 0x0806) extract the sender IP and sender MAC and store them in the cache.
 * This runs on both ARP requests and replies so GW-A learns remote MACs as
 * soon as any ARP traffic from the far side crosses the FSO link.
 */
static void arp_learn_from_packet(rx_pipeline_t       *pl,
                                  const unsigned char *pkt,
                                  int                  pkt_len)
{
    uint32_t sender_ip;

    if (pl->arp_cache == NULL) {
        return;
    }

    /* Minimum ARP frame: 14 (Ethernet hdr) + 28 (ARP body) = 42 bytes */
    if (pkt_len < 42) {
        return;
    }

    /* Check EtherType == 0x0806 */
    if (pkt[12] != 0x08 || pkt[13] != 0x06) {
        return;
    }

    /* sender MAC at offset 22, sender IP at offset 28 */
    memcpy(&sender_ip, pkt + 28, 4);
    arp_cache_learn(pl->arp_cache, sender_ip, pkt + 22);
}
