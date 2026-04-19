/*
 * src/gateway.c — Full-duplex FSO Gateway implementation.
 *
 * See include/gateway.h for the public API.
 *
 * Two pthreads run the TX and RX pipelines simultaneously:
 *
 *   TX thread: NIC_LAN → fragment → FEC encode → interleave → NIC_FSO
 *   RX thread: NIC_FSO → deinterleave → FEC decode → reassemble → NIC_LAN
 *
 * The stop condition is gw->running (volatile int).  Either thread may
 * clear it on a fatal pipeline error; gateway_stop() also clears it from
 * outside (e.g. a signal handler).
 *
 * gateway_run() joins both threads before returning so the caller can
 * safely call gateway_destroy() immediately afterwards.
 */

#define _POSIX_C_SOURCE 200112L

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "gateway.h"
#include "logging.h"
#include "packet_io.h"
#include "rx_pipeline.h"
#include "tx_pipeline.h"

/* -------------------------------------------------------------------------- */
/* Internal constants                                                          */
/* -------------------------------------------------------------------------- */

#define ERRBUF_SIZE 256

/* -------------------------------------------------------------------------- */
/* Internal struct                                                             */
/* -------------------------------------------------------------------------- */

struct gateway {
    struct config     cfg;
    packet_io_ctx_t  *ctx_lan_rx;   /* LAN NIC — promiscuous, TX source      */
    packet_io_ctx_t  *ctx_fso_tx;   /* FSO NIC — non-promiscuous, TX sink    */
    packet_io_ctx_t  *ctx_fso_rx;   /* FSO NIC — promiscuous, RX source      */
    packet_io_ctx_t  *ctx_lan_tx;   /* LAN NIC — non-promiscuous, RX sink    */
    tx_pipeline_t    *tx_pl;
    rx_pipeline_t    *rx_pl;
    volatile int      running;
    int               tx_error;     /* 1 if TX thread had a fatal error       */
    int               rx_error;     /* 1 if RX thread had a fatal error       */
    pthread_t         tx_tid;
    pthread_t         rx_tid;
};

/* -------------------------------------------------------------------------- */
/* Thread functions                                                            */
/* -------------------------------------------------------------------------- */

static void *tx_thread_func(void *arg)
{
    gateway_t *gw = (gateway_t *)arg;
    int        rc;

    LOG_INFO("[gateway] TX thread started");

    while (gw->running) {
        rc = tx_pipeline_run_once(gw->tx_pl);
        if (rc == -1) {
            LOG_ERROR("[gateway] TX thread: tx_pipeline_run_once fatal error");
            gw->tx_error = 1;
            gw->running  = 0;
            break;
        }
    }

    LOG_INFO("[gateway] TX thread stopped");
    return NULL;
}

static void *rx_thread_func(void *arg)
{
    gateway_t *gw = (gateway_t *)arg;
    int        rc;

    LOG_INFO("[gateway] RX thread started");

    while (gw->running) {
        rc = rx_pipeline_run_once(gw->rx_pl);
        if (rc == -1) {
            LOG_ERROR("[gateway] RX thread: rx_pipeline_run_once fatal error");
            gw->rx_error = 1;
            gw->running  = 0;
            break;
        }
    }

    LOG_INFO("[gateway] RX thread stopped");
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

gateway_t *gateway_create(const struct config *cfg)
{
    gateway_t *gw;
    char       errbuf[ERRBUF_SIZE];

    if (cfg == NULL) {
        LOG_ERROR("[gateway] create: cfg is NULL");
        return NULL;
    }

    gw = (gateway_t *)malloc(sizeof(gateway_t));
    if (gw == NULL) {
        LOG_ERROR("[gateway] create: malloc(gateway_t) failed");
        return NULL;
    }
    memset(gw, 0, sizeof(gateway_t));
    gw->cfg     = *cfg;
    gw->running = 0;

    /* ---- Open all four packet_io handles -------------------------------- */

    gw->ctx_lan_rx = packet_io_open(cfg->lan_iface, 1,
                                     errbuf, sizeof(errbuf));
    if (gw->ctx_lan_rx == NULL) {
        LOG_ERROR("[gateway] create: packet_io_open(lan_rx \"%s\") failed: %s",
                  cfg->lan_iface, errbuf);
        free(gw);
        return NULL;
    }

    packet_io_set_direction_in(gw->ctx_lan_rx);
    gw->ctx_fso_tx = packet_io_open(cfg->fso_iface, 0,
                                     errbuf, sizeof(errbuf));
    if (gw->ctx_fso_tx == NULL) {
        LOG_ERROR("[gateway] create: packet_io_open(fso_tx \"%s\") failed: %s",
                  cfg->fso_iface, errbuf);
        packet_io_close(gw->ctx_lan_rx);
        free(gw);
        return NULL;
    }

    gw->ctx_fso_rx = packet_io_open(cfg->fso_iface, 1,
                                     errbuf, sizeof(errbuf));
    if (gw->ctx_fso_rx == NULL) {
        LOG_ERROR("[gateway] create: packet_io_open(fso_rx \"%s\") failed: %s",
                  cfg->fso_iface, errbuf);
        packet_io_close(gw->ctx_fso_tx);
        packet_io_close(gw->ctx_lan_rx);
        free(gw);
        return NULL;
    }

    gw->ctx_lan_tx = packet_io_open(cfg->lan_iface, 0,
                                     errbuf, sizeof(errbuf));
    if (gw->ctx_lan_tx == NULL) {
        LOG_ERROR("[gateway] create: packet_io_open(lan_tx \"%s\") failed: %s",
                  cfg->lan_iface, errbuf);
        packet_io_close(gw->ctx_fso_rx);
        packet_io_close(gw->ctx_fso_tx);
        packet_io_close(gw->ctx_lan_rx);
        free(gw);
        return NULL;
    }

    /* ---- Create TX pipeline -------------------------------------------- */

    gw->tx_pl = tx_pipeline_create(cfg, gw->ctx_lan_rx, gw->ctx_fso_tx);
    if (gw->tx_pl == NULL) {
        LOG_ERROR("[gateway] create: tx_pipeline_create failed");
        packet_io_close(gw->ctx_lan_tx);
        packet_io_close(gw->ctx_fso_rx);
        packet_io_close(gw->ctx_fso_tx);
        packet_io_close(gw->ctx_lan_rx);
        free(gw);
        return NULL;
    }

    /* ---- Create RX pipeline -------------------------------------------- */

    gw->rx_pl = rx_pipeline_create(cfg, gw->ctx_fso_rx, gw->ctx_lan_tx);
    if (gw->rx_pl == NULL) {
        LOG_ERROR("[gateway] create: rx_pipeline_create failed");
        tx_pipeline_destroy(gw->tx_pl);
        packet_io_close(gw->ctx_lan_tx);
        packet_io_close(gw->ctx_fso_rx);
        packet_io_close(gw->ctx_fso_tx);
        packet_io_close(gw->ctx_lan_rx);
        free(gw);
        return NULL;
    }

    LOG_INFO("[gateway] Created: lan=%s fso=%s k=%d m=%d depth=%d sym=%d",
             cfg->lan_iface, cfg->fso_iface,
             cfg->k, cfg->m, cfg->depth, cfg->symbol_size);

    return gw;
}

int gateway_run(gateway_t *gw)
{
    int rc_tx;
    int rc_rx;

    if (gw == NULL) {
        LOG_ERROR("[gateway] run: gw is NULL");
        return -1;
    }

    gw->running  = 1;
    gw->tx_error = 0;
    gw->rx_error = 0;

    rc_tx = pthread_create(&gw->tx_tid, NULL, tx_thread_func, gw);
    if (rc_tx != 0) {
        LOG_ERROR("[gateway] run: pthread_create(TX) failed (rc=%d)", rc_tx);
        gw->running = 0;
        return -1;
    }

    rc_rx = pthread_create(&gw->rx_tid, NULL, rx_thread_func, gw);
    if (rc_rx != 0) {
        LOG_ERROR("[gateway] run: pthread_create(RX) failed (rc=%d)", rc_rx);
        gw->running = 0;
        pthread_join(gw->tx_tid, NULL);
        return -1;
    }

    LOG_INFO("[gateway] Both pipeline threads running");

    pthread_join(gw->tx_tid, NULL);
    pthread_join(gw->rx_tid, NULL);

    LOG_INFO("[gateway] Both pipeline threads joined");

    if (gw->tx_error || gw->rx_error) {
        return -1;
    }
    return 0;
}

void gateway_stop(gateway_t *gw)
{
    if (gw == NULL) {
        return;
    }
    gw->running = 0;
}

void gateway_destroy(gateway_t *gw)
{
    if (gw == NULL) {
        return;
    }

    rx_pipeline_destroy(gw->rx_pl);
    tx_pipeline_destroy(gw->tx_pl);

    packet_io_close(gw->ctx_lan_tx);
    packet_io_close(gw->ctx_fso_rx);
    packet_io_close(gw->ctx_fso_tx);
    packet_io_close(gw->ctx_lan_rx);

    free(gw);
}
