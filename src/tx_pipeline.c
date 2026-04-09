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
#include <stdlib.h>
#include <string.h>

#include "block_builder.h"
#include "config.h"
#include "fec_wrapper.h"
#include "interleaver.h"
#include "logging.h"
#include "packet_fragmenter.h"
#include "packet_io.h"
#include "symbol.h"
#include "tx_pipeline.h"
#include "types.h"

/* -------------------------------------------------------------------------- */
/* Internal constants                                                          */
/* -------------------------------------------------------------------------- */

#define TX_BLOCK_TIMEOUT_MS  10.0

/* Receive buffer size — must hold the largest possible Ethernet frame */
#define TX_RX_BUF_SIZE       9200U

/* Wire header size in bytes */
#define WIRE_HDR_SIZE        18U

/* -------------------------------------------------------------------------- */
/* Internal struct                                                             */
/* -------------------------------------------------------------------------- */

struct tx_pipeline {
    struct config     cfg;                /* copy of runtime configuration    */
    packet_io_ctx_t  *rx_ctx;            /* LAN NIC — receive side           */
    packet_io_ctx_t  *tx_ctx;            /* FSO NIC — transmit side          */
    block_builder_t   builder;           /* accumulates K source symbols      */
    fec_handle_t      fec;              /* Wirehair FEC context              */
    interleaver_t    *interleaver;       /* matrix interleaver                */
    uint32_t          packet_id_counter; /* monotonically incrementing ID     */
    unsigned char    *source_buf;        /* k * symbol_size heap buffer       */
    symbol_t         *frag_syms;         /* heap, k elements                  */
    symbol_t         *repair_syms;       /* heap, m elements                  */
};

/* -------------------------------------------------------------------------- */
/* Forward declarations                                                        */
/* -------------------------------------------------------------------------- */

static int encode_and_drain(tx_pipeline_t *pl);
static int tx_serialize_and_send(tx_pipeline_t *pl, const symbol_t *sym);

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

tx_pipeline_t *tx_pipeline_create(const struct config *cfg,
                                  packet_io_ctx_t     *rx_ctx,
                                  packet_io_ctx_t     *tx_ctx)
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

    pl->cfg               = *cfg;
    pl->rx_ctx            = rx_ctx;
    pl->tx_ctx            = tx_ctx;
    pl->packet_id_counter = 0;

    /* Heap-allocate source_buf (k * symbol_size can be up to ~576 kB) */
    source_buf_size = (size_t)cfg->k * (size_t)cfg->symbol_size;
    pl->source_buf = (unsigned char *)malloc(source_buf_size);
    if (pl->source_buf == NULL) {
        LOG_ERROR("[tx_pipeline] create: malloc(source_buf, %zu) failed",
                  source_buf_size);
        free(pl);
        return NULL;
    }

    /* Initialise block builder */
    if (block_builder_init(&pl->builder, cfg->k) != 0) {
        LOG_ERROR("[tx_pipeline] create: block_builder_init(k=%d) failed",
                  cfg->k);
        free(pl->source_buf);
        free(pl);
        return NULL;
    }

    /* Create FEC context */
    pl->fec = fec_create(cfg->k, cfg->symbol_size);
    if (pl->fec == NULL) {
        LOG_ERROR("[tx_pipeline] create: fec_create(k=%d, sym=%d) failed",
                  cfg->k, cfg->symbol_size);
        block_builder_destroy(&pl->builder);
        free(pl->source_buf);
        free(pl);
        return NULL;
    }

    /* Create interleaver */
    pl->interleaver = interleaver_create(cfg->depth,
                                         cfg->k + cfg->m,
                                         cfg->symbol_size);
    if (pl->interleaver == NULL) {
        LOG_ERROR("[tx_pipeline] create: interleaver_create(d=%d,n=%d,s=%d) failed",
                  cfg->depth, cfg->k + cfg->m, cfg->symbol_size);
        fec_destroy(pl->fec);
        block_builder_destroy(&pl->builder);
        free(pl->source_buf);
        free(pl);
        return NULL;
    }

    /* Heap-allocate frag_syms (k elements) */
    pl->frag_syms = (symbol_t *)malloc(sizeof(symbol_t) * (size_t)cfg->k);
    if (pl->frag_syms == NULL) {
        LOG_ERROR("[tx_pipeline] create: malloc(frag_syms, k=%d) failed",
                  cfg->k);
        interleaver_destroy(pl->interleaver);
        fec_destroy(pl->fec);
        block_builder_destroy(&pl->builder);
        free(pl->source_buf);
        free(pl);
        return NULL;
    }

    /* Heap-allocate repair_syms (m elements) */
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
             cfg->k, cfg->m, cfg->depth, cfg->symbol_size);

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

    if (pl == NULL) {
        LOG_ERROR("[tx_pipeline] run_once: pl is NULL");
        return -1;
    }

    pkt_len = 0;

    /* ------------------------------------------------------------------ */
    /* Step 1 — Receive from LAN NIC                                       */
    /* ------------------------------------------------------------------ */
    rc = packet_io_receive(pl->rx_ctx, rx_buf, sizeof(rx_buf), &pkt_len);

    if (rc == 0) {
        /* No packet available — check block timeout */
        int timeout_rc = block_builder_check_timeout(&pl->builder,
                                                     TX_BLOCK_TIMEOUT_MS);
        if (timeout_rc == 1 && pl->builder.symbol_count > 0) {
            block_builder_finalize_with_padding(&pl->builder);
            if (encode_and_drain(pl) != 0) {
                return -1;
            }
            block_builder_reset(&pl->builder);
        }
        return 0;
    }

    if (rc == -1) {
        LOG_WARN("[tx_pipeline] run_once: packet_io_receive error: %s",
                 packet_io_last_error(pl->rx_ctx));
        return 0;
    }

    /* rc == 1: packet received */

    /* ------------------------------------------------------------------ */
    /* Step 2 — Fragment                                                   */
    /* ------------------------------------------------------------------ */
    num_frags = fragment_packet(rx_buf,
                                pkt_len,
                                pl->packet_id_counter,
                                (uint16_t)pl->cfg.symbol_size,
                                pl->frag_syms,
                                (uint16_t)pl->cfg.k);
    if (num_frags < 0) {
        LOG_WARN("[tx_pipeline] run_once: fragment_packet failed "
                 "(pkt_len=%zu, packet_id=%u)",
                 pkt_len, pl->packet_id_counter);
        return 0;
    }

    pl->packet_id_counter++;

    /* ------------------------------------------------------------------ */
    /* Step 3 — Feed symbols into block builder                            */
    /* ------------------------------------------------------------------ */
    for (i = 0; i < num_frags; ++i) {
        add_rc = block_builder_add_symbol(&pl->builder, &pl->frag_syms[i]);
        if (add_rc == -1) {
            LOG_WARN("[tx_pipeline] run_once: block_builder_add_symbol "
                     "failed at frag %d", i);
            continue;
        }

        if (add_rc == 1) {
            /* Block is full — encode and drain */
            if (encode_and_drain(pl) != 0) {
                return -1;
            }
            block_builder_reset(&pl->builder);
        }
    }

    /* ------------------------------------------------------------------ */
    /* Step 4 — Return success                                             */
    /* ------------------------------------------------------------------ */
    return 0;
}

/* -------------------------------------------------------------------------- */
/* encode_and_drain — internal helper                                          */
/* -------------------------------------------------------------------------- */

/*
 * Called when a block is full (either organically or after timeout padding).
 * Packs source data, generates repair symbols, stamps CRC (if enabled),
 * pushes all K+M symbols to the interleaver, then drains if ready.
 */
static int encode_and_drain(tx_pipeline_t *pl)
{
    int      k          = pl->cfg.k;
    int      m          = pl->cfg.m;
    int      sym_size   = pl->cfg.symbol_size;
    int      crc_on     = pl->cfg.internal_symbol_crc_enabled;
    uint32_t block_id;
    int      i;
    int      rc;
    symbol_t out_sym;

    memset(pl->repair_syms, 0, sizeof(symbol_t) * (size_t)m);

    /* ---- 1. Pack source data ----------------------------------------- */
    for (i = 0; i < k; ++i) {
        memcpy(pl->source_buf + (size_t)i * (size_t)sym_size,
               pl->builder.symbols[i].data,
               (size_t)sym_size);
    }

    /* ---- 2. Generate repair symbols ------------------------------------ */
    rc = fec_encode_block(pl->fec, pl->source_buf, pl->repair_syms, m);
    if (rc != 0) {
        LOG_ERROR("[tx_pipeline] encode_and_drain: fec_encode_block failed");
        return -1;
    }

    block_id = (uint32_t)pl->builder.block_id;

    /* ---- 3. Set metadata on repair symbols ----------------------------- */
    for (i = 0; i < m; ++i) {
        pl->repair_syms[i].packet_id     = block_id;
        pl->repair_syms[i].fec_id        = (uint32_t)(k + i);
        pl->repair_syms[i].symbol_index  = (uint16_t)(k + i);
        pl->repair_syms[i].total_symbols = (uint16_t)(k + m);
        pl->repair_syms[i].payload_len   = (uint16_t)sym_size;
    }

    /* ---- 4. Update total_symbols on source symbols --------------------- */
    for (i = 0; i < k; ++i) {
        pl->builder.symbols[i].total_symbols = (uint16_t)(k + m);
    }

    /* ---- 5. CRC stamp -------------------------------------------------- */
    if (crc_on) {
        for (i = 0; i < k; ++i) {
            symbol_compute_crc(&pl->builder.symbols[i]);
        }
        for (i = 0; i < m; ++i) {
            symbol_compute_crc(&pl->repair_syms[i]);
        }
    }

    /* ---- 6. Push all K+M symbols to interleaver ----------------------- */
    for (i = 0; i < k; ++i) {
        rc = interleaver_push_symbol(pl->interleaver,
                                     &pl->builder.symbols[i]);
        if (rc == -1) {
            LOG_ERROR("[tx_pipeline] encode_and_drain: "
                      "interleaver_push_symbol(src %d) failed", i);
            return -1;
        }
    }
    for (i = 0; i < m; ++i) {
        rc = interleaver_push_symbol(pl->interleaver, &pl->repair_syms[i]);
        if (rc == -1) {
            LOG_ERROR("[tx_pipeline] encode_and_drain: "
                      "interleaver_push_symbol(repair %d) failed", i);
            return -1;
        }
    }

    /* ---- Drain interleaver -------------------------------------------- */
    if (interleaver_is_ready(pl->interleaver)) {
        while (1) {
            rc = interleaver_pop_ready_symbol(pl->interleaver, &out_sym);
            if (rc == -1) {
                /* Not ready / empty — stop draining */
                break;
            }
            if (tx_serialize_and_send(pl, &out_sym) != 0) {
                /* LOG_WARN already emitted inside; continue best-effort */
            }
            if (rc == 1) {
                /* Last symbol in window — matrix reset */
                break;
            }
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* tx_serialize_and_send — internal helper                                     */
/* -------------------------------------------------------------------------- */

/*
 * Serialise one symbol_t into the wire format and inject it via packet_io_send.
 *
 * Wire layout (18 bytes header + payload):
 *   [0..3]   packet_id      big-endian uint32
 *   [4..7]   fec_id         big-endian uint32
 *   [8..9]   symbol_index   big-endian uint16
 *   [10..11] total_symbols  big-endian uint16
 *   [12..13] payload_len    big-endian uint16
 *   [14..17] crc32          big-endian uint32
 *   [18..]   data[0..payload_len-1]
 */
static int tx_serialize_and_send(tx_pipeline_t *pl, const symbol_t *sym)
{
    unsigned char wire[WIRE_HDR_SIZE + MAX_SYMBOL_DATA_SIZE];
    uint32_t      tmp32;
    uint16_t      tmp16;
    size_t        wire_len;

    /* packet_id */
    tmp32 = htonl(sym->packet_id);
    memcpy(wire + 0, &tmp32, 4);

    /* fec_id */
    tmp32 = htonl(sym->fec_id);
    memcpy(wire + 4, &tmp32, 4);

    /* symbol_index */
    tmp16 = htons(sym->symbol_index);
    memcpy(wire + 8, &tmp16, 2);

    /* total_symbols */
    tmp16 = htons(sym->total_symbols);
    memcpy(wire + 10, &tmp16, 2);

    /* payload_len */
    tmp16 = htons(sym->payload_len);
    memcpy(wire + 12, &tmp16, 2);

    /* crc32 */
    tmp32 = htonl(sym->crc32);
    memcpy(wire + 14, &tmp32, 4);

    /* payload */
    if (sym->payload_len > 0) {
        memcpy(wire + WIRE_HDR_SIZE, sym->data, sym->payload_len);
    }

    wire_len = WIRE_HDR_SIZE + sym->payload_len;

    if (packet_io_send(pl->tx_ctx, wire, wire_len) != 0) {
        LOG_WARN("[tx_pipeline] tx_serialize_and_send: "
                 "packet_io_send failed (packet_id=%u fec_id=%u): %s",
                 sym->packet_id, sym->fec_id,
                 packet_io_last_error(pl->tx_ctx));
        return -1;
    }

    return 0;
}
