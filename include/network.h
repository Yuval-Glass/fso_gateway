#ifndef NETWORK_H
#define NETWORK_H

/**
 * @file network.h
 * @brief Networking utilities for the FSO Gateway.
 *
 * This module provides helper functions for opening and using raw
 * Ethernet sockets on Linux. Raw sockets are useful when we want
 * to inspect or process frames directly at Layer 2 (Ethernet level),
 * without relying on the normal TCP/IP socket abstractions.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open a raw socket bound to a specific network interface.
 *
 * This function creates a Linux AF_PACKET raw socket and binds it to the
 * given interface (for example, "eth0"). Once opened, the socket can
 * receive Ethernet frames directly from that interface.
 *
 * @param iface Name of the network interface to bind to.
 *
 * @return A valid socket file descriptor on success, or -1 on failure.
 */
int net_open_raw_socket(const char *iface);

/**
 * @brief Receive and log a fixed number of Ethernet frames from a raw socket.
 *
 * This function enters a basic packet capture loop and listens for incoming
 * Ethernet frames using recvfrom(). For each successfully received frame,
 * it logs the captured size in bytes. The loop ends after max_packets frames
 * have been captured.
 *
 * This function is intended as an initial verification step to confirm that:
 * - the raw socket is working,
 * - frames are arriving on the selected interface,
 * - the application can observe Layer 2 traffic.
 *
 * @param sockfd      A valid raw socket file descriptor.
 * @param max_packets Number of packets to capture before returning.
 */
void net_sniff_loop(int sockfd, int max_packets);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_H */