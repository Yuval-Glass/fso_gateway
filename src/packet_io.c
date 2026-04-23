/* This file is compiled only in pcap mode.  For DPDK mode, see
 * packet_io_dpdk.c (selected by -DUSE_DPDK_BUILD).               */
#ifndef USE_DPDK_BUILD

/*
 * src/packet_io.c — Raw packet I/O layer using libpcap.
 *
 * See include/packet_io.h for the public API and design notes.
 *
 * Implementation notes
 * --------------------
 * pcap_open_live() is used with a snaplen of PACKET_IO_SNAPLEN (9200 bytes)
 * to support jumbo frames.  After opening, the handle is switched to
 * non-blocking mode via pcap_setnonblock().
 *
 * packet_io_receive() wraps pcap_next_ex():
 *   - Return value  1 → packet captured.
 *   - Return value  0 → timeout / no packet (EAGAIN equivalent in non-blocking).
 *   - Return value -1 → pcap error.
 *   - Return value -2 → no packets available (non-blocking, same as 0).
 *
 * packet_io_send() uses pcap_sendpacket() which returns 0 on success and
 * -1 on failure.
 *
 * All errors are written into ctx->last_error for retrieval via
 * packet_io_last_error() and are also emitted via LOG_ERROR.
 */

#define _POSIX_C_SOURCE 200112L
#define _BSD_SOURCE
#define _DEFAULT_SOURCE

#include <sys/types.h>
#include <net/if.h>      /* IF_NAMESIZE */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pcap/pcap.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <sys/ioctl.h>

#include "logging.h"
#include "packet_io.h"

/* -------------------------------------------------------------------------- */
/* Internal constants                                                          */
/* -------------------------------------------------------------------------- */

/*
 * Minimum snaplen: 9200 bytes covers standard jumbo frames (9000 bytes
 * payload + Ethernet/VLAN headers).
 */
#define PACKET_IO_SNAPLEN   9200

/*
 * pcap_open_live() read timeout in milliseconds.  In non-blocking mode this
 * value is largely irrelevant (pcap_next_ex returns immediately), but a
 * non-zero value is required by some platforms to avoid degenerate behaviour.
 */
#define PACKET_IO_TIMEOUT_MS 1

/* -------------------------------------------------------------------------- */
/* Opaque context struct                                                       */
/* -------------------------------------------------------------------------- */

struct packet_io_ctx {
    pcap_t *handle;
    int     raw_fd;
    char    last_error[PCAP_ERRBUF_SIZE];
    char    iface[IF_NAMESIZE];
};

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

packet_io_ctx_t *packet_io_open(const char *iface,
                                int         promiscuous,
                                char       *errbuf,
                                size_t      errbuf_size)
{
    packet_io_ctx_t *ctx;
    char             pcap_errbuf[PCAP_ERRBUF_SIZE];
    int              rc;

    if (iface == NULL) {
        if (errbuf != NULL && errbuf_size > 0) {
            snprintf(errbuf, errbuf_size, "iface is NULL");
        }
        LOG_ERROR("[packet_io] open: iface argument is NULL");
        return NULL;
    }

    if (errbuf == NULL || errbuf_size == 0) {
        LOG_ERROR("[packet_io] open: errbuf is NULL or zero-length");
        return NULL;
    }

    ctx = (packet_io_ctx_t *)malloc(sizeof(packet_io_ctx_t));
    if (ctx == NULL) {
        snprintf(errbuf, errbuf_size, "malloc(packet_io_ctx_t) failed");
        LOG_ERROR("[packet_io] open: malloc(packet_io_ctx_t) failed");
        return NULL;
    }

    memset(ctx, 0, sizeof(packet_io_ctx_t));
    strncpy(ctx->iface, iface, IF_NAMESIZE - 1);
    ctx->iface[IF_NAMESIZE - 1] = '\0';
    ctx->raw_fd = -1;

    pcap_errbuf[0] = '\0';
    ctx->handle = pcap_open_live(iface,
                                 PACKET_IO_SNAPLEN,
                                 promiscuous,
                                 PACKET_IO_TIMEOUT_MS,
                                 pcap_errbuf);
    if (ctx->handle == NULL) {
        snprintf(errbuf, errbuf_size, "%s", pcap_errbuf);
        snprintf(ctx->last_error, PCAP_ERRBUF_SIZE, "%s", pcap_errbuf);
        LOG_ERROR("[packet_io] open: pcap_open_live(%s) failed: %s",
                  iface, pcap_errbuf);
        free(ctx);
        return NULL;
    }

    /* Warn on non-fatal warnings from pcap_open_live */
    if (pcap_errbuf[0] != '\0') {
        LOG_WARN("[packet_io] open: pcap_open_live(%s) warning: %s",
                 iface, pcap_errbuf);
    }

    rc = pcap_setnonblock(ctx->handle, 1, pcap_errbuf);
    if (rc == -1) {
        snprintf(errbuf, errbuf_size, "%s", pcap_errbuf);
        snprintf(ctx->last_error, PCAP_ERRBUF_SIZE, "%s", pcap_errbuf);
        LOG_ERROR("[packet_io] open: pcap_setnonblock(%s) failed: %s",
                  iface, pcap_errbuf);
        pcap_close(ctx->handle);
        free(ctx);
        return NULL;
    }

    LOG_INFO("[packet_io] Opened interface \"%s\" "
             "(promiscuous=%d, snaplen=%d, non-blocking)",
             iface, promiscuous, PACKET_IO_SNAPLEN);

    return ctx;
}

void packet_io_close(packet_io_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->handle != NULL) {
        pcap_close(ctx->handle);
        ctx->handle = NULL;
    }

    free(ctx);
}

/* -------------------------------------------------------------------------- */
/* I/O                                                                        */
/* -------------------------------------------------------------------------- */

int packet_io_receive(packet_io_ctx_t *ctx,
                      unsigned char   *buf,
                      size_t           buf_size,
                      size_t          *out_len)
{
    struct pcap_pkthdr *header;
    const unsigned char *data;
    size_t               copy_len;
    int                  rc;

    if (out_len != NULL) {
        *out_len = 0;
    }

    if (ctx == NULL) {
        LOG_ERROR("[packet_io] receive: ctx is NULL");
        return -1;
    }

    if (buf == NULL) {
        snprintf(ctx->last_error, PCAP_ERRBUF_SIZE,
                 "receive: buf is NULL");
        LOG_ERROR("[packet_io] receive: buf is NULL");
        return -1;
    }

    if (out_len == NULL) {
        snprintf(ctx->last_error, PCAP_ERRBUF_SIZE,
                 "receive: out_len is NULL");
        LOG_ERROR("[packet_io] receive: out_len is NULL");
        return -1;
    }

    rc = pcap_next_ex(ctx->handle, &header, &data);

    if (rc == 0 || rc == -2) {
        /* 0  = timeout / no packet available in non-blocking mode
         * -2 = no more packets (live capture won't return -2, but handle it) */
        return 0;
    }

    if (rc == -1) {
        snprintf(ctx->last_error, PCAP_ERRBUF_SIZE, "%s",
                 pcap_geterr(ctx->handle));
        LOG_ERROR("[packet_io] receive: pcap_next_ex(%s) error: %s",
                  ctx->iface, ctx->last_error);
        return -1;
    }

    /* rc == 1: packet captured */
    copy_len = (size_t)header->caplen;

    if (copy_len > buf_size) {
        LOG_WARN("[packet_io] receive: frame truncated on \"%s\" "
                 "(caplen=%u > buf_size=%zu); copying %zu bytes",
                 ctx->iface, header->caplen, buf_size, buf_size);
        copy_len = buf_size;
    }

    memcpy(buf, data, copy_len);
    *out_len = copy_len;

    return 1;
}

int packet_io_send(packet_io_ctx_t     *ctx,
                   const unsigned char *buf,
                   size_t               len)
{
    int rc;

    if (ctx == NULL) {
        LOG_ERROR("[packet_io] send: ctx is NULL");
        return -1;
    }

    if (buf == NULL) {
        snprintf(ctx->last_error, PCAP_ERRBUF_SIZE, "send: buf is NULL");
        LOG_ERROR("[packet_io] send: buf is NULL");
        return -1;
    }

    if (len == 0) {
        snprintf(ctx->last_error, PCAP_ERRBUF_SIZE,
                 "send: len is 0");
        LOG_ERROR("[packet_io] send: len is 0");
        return -1;
    }

    rc = pcap_sendpacket(ctx->handle, buf, (int)len);
    if (rc == -1) {
        snprintf(ctx->last_error, PCAP_ERRBUF_SIZE, "%s",
                 pcap_geterr(ctx->handle));
        LOG_ERROR("[packet_io] send: pcap_sendpacket(%s, len=%zu) failed: %s",
                  ctx->iface, len, ctx->last_error);
        return -1;
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Diagnostics                                                                 */
/* -------------------------------------------------------------------------- */

const char *packet_io_last_error(const packet_io_ctx_t *ctx)
{
    if (ctx == NULL) {
        return "";
    }

    return ctx->last_error;
}

/* -------------------------------------------------------------------------- */
int packet_io_ignore_outgoing(packet_io_ctx_t *ctx)
{
    int fd;
    int val = 1;

#ifndef PACKET_IGNORE_OUTGOING
#define PACKET_IGNORE_OUTGOING 23
#endif

    if (ctx == NULL) {
        LOG_ERROR("[packet_io] ignore_outgoing: NULL context");
        return -1;
    }

    fd = pcap_get_selectable_fd(ctx->handle);
    if (fd == -1) {
        LOG_WARN("[packet_io] ignore_outgoing(%s): pcap_get_selectable_fd failed",
                 ctx->iface);
        return -1;
    }

    if (setsockopt(fd, SOL_PACKET, PACKET_IGNORE_OUTGOING, &val, sizeof(val)) != 0) {
        LOG_WARN("[packet_io] ignore_outgoing(%s): setsockopt failed (kernel < 4.20?)",
                 ctx->iface);
        return -1;
    }

    LOG_INFO("[packet_io] ignore_outgoing: \"%s\" will not capture self-sent frames",
             ctx->iface);
    return 0;
}

/* -------------------------------------------------------------------------- */
int packet_io_set_direction_in(packet_io_ctx_t *ctx)
{
    int rc;

    if (ctx == NULL) {
        LOG_ERROR("[packet_io] set_direction_in: NULL context");
        return -1;
    }

    rc = pcap_setdirection(ctx->handle, PCAP_D_IN);
    if (rc == -1) {
        LOG_WARN("[packet_io] set_direction_in(%s): %s",
                 ctx->iface, pcap_geterr(ctx->handle));
        return -1;
    }

    LOG_INFO("[packet_io] set_direction_in: interface \"%s\" now captures ingress only",
             ctx->iface);
    return 0;
}

#endif /* !USE_DPDK_BUILD */
