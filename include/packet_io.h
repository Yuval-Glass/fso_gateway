/*
 * include/packet_io.h — Raw packet I/O layer using libpcap.
 *
 * This module provides a non-blocking, pcap-based interface for capturing
 * and injecting raw Ethernet frames on a named network interface.  It is
 * used by both the TX pipeline (capturing from the LAN NIC) and the RX
 * pipeline (capturing from the FSO NIC), as well as by forwarding test tools.
 *
 * Jumbo frame support: the capture snaplen is at least 9200 bytes.
 * All handles are opened in non-blocking mode.
 *
 * Opaque handle: struct packet_io_ctx is defined only in packet_io.c.
 */

#ifndef PACKET_IO_H
#define PACKET_IO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Opaque handle                                                               */
/* -------------------------------------------------------------------------- */

typedef struct packet_io_ctx packet_io_ctx_t;

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

/*
 * packet_io_open() — open a live pcap capture handle on a named interface.
 *
 *   iface:        network interface name (e.g. "eth0")
 *   promiscuous:  1 = promiscuous mode, 0 = normal mode
 *   errbuf:       caller-allocated buffer for error messages;
 *                 must be at least PCAP_ERRBUF_SIZE bytes
 *   errbuf_size:  size of errbuf in bytes
 *
 * The handle is placed in non-blocking mode after opening.
 * On success, the opened interface name is logged at INFO level.
 * On failure, the error is written to errbuf AND logged via LOG_ERROR.
 *
 * Returns non-NULL on success, NULL on failure.
 */
packet_io_ctx_t *packet_io_open(const char *iface,
                                int         promiscuous,
                                char       *errbuf,
                                size_t      errbuf_size);

/*
 * packet_io_close() — close the pcap handle and free all memory.
 *
 * Safe to call with NULL — must not crash.
 */
void packet_io_close(packet_io_ctx_t *ctx);

int packet_io_set_direction_in(packet_io_ctx_t *ctx);
/* Restricts capture to ingress-only (PCAP_D_IN).
 * Call on RX handles to prevent loopback of locally-injected frames.
 * Returns 0 on success, -1 on failure (non-fatal). */

int packet_io_ignore_outgoing(packet_io_ctx_t *ctx);
/* Instructs the kernel not to deliver self-sent (egress) frames to this
 * socket (PACKET_IGNORE_OUTGOING, available since Linux 4.20).
 * Use on ctx_fso_rx to prevent a gateway from processing its own TX symbols.
 * Returns 0 on success, -1 on failure (non-fatal, logged as WARN). */

/* -------------------------------------------------------------------------- */
/* I/O                                                                        */
/* -------------------------------------------------------------------------- */

/*
 * packet_io_receive() — attempt to receive one raw Ethernet frame.
 *                       Non-blocking.
 *
 *   buf:      caller-allocated receive buffer
 *   buf_size: size of buf in bytes
 *   out_len:  set to the number of bytes copied on success (return 1),
 *             or 0 on return 0 or -1
 *
 * Return values:
 *    1  — packet received and copied into buf
 *    0  — no packet available (would block)
 *   -1  — error
 *
 * If the captured frame is larger than buf_size, buf_size bytes are copied,
 * a LOG_WARN is emitted, and 1 is returned (truncated but not an error).
 */
int packet_io_receive(packet_io_ctx_t *ctx,
                      unsigned char   *buf,
                      size_t           buf_size,
                      size_t          *out_len);

/*
 * packet_io_send() — inject one raw Ethernet frame onto the interface.
 *
 *   buf: raw frame bytes including Ethernet header
 *   len: total frame length in bytes
 *
 * Returns:
 *    0  — success
 *   -1  — failure (logged via LOG_ERROR)
 */
int packet_io_send(packet_io_ctx_t     *ctx,
                   const unsigned char *buf,
                   size_t               len);

/* -------------------------------------------------------------------------- */
/* Diagnostics                                                                 */
/* -------------------------------------------------------------------------- */

/*
 * packet_io_last_error() — return the last error string stored in ctx.
 *
 * Returns "" (empty string) if ctx is NULL or no error has occurred.
 * The returned pointer is valid until the next call on this ctx.
 */
const char *packet_io_last_error(const packet_io_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* PACKET_IO_H */
