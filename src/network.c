/**
 * @file network.c
 * @brief Raw socket operations for the FSO Gateway.
 *
 * This file implements low-level Linux networking helpers that operate
 * directly on Ethernet frames. The main purpose is to allow the gateway
 * to observe traffic at Layer 2 and later extend this into custom frame
 * processing for the FSO path.
 */

#include "network.h"

#include "logging.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/**
 * @brief Open a raw packet socket and bind it to a specific interface.
 *
 * Linux provides AF_PACKET sockets for direct access to Ethernet frames.
 * We use:
 *
 *   socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL))
 *
 * Explanation:
 * - AF_PACKET:
 *   This address family allows user-space programs to send/receive packets
 *   directly at the data-link layer (Layer 2). Unlike normal sockets such
 *   as AF_INET, this gives access to complete Ethernet frames.
 *
 * - SOCK_RAW:
 *   Indicates that we want the raw packet contents, including link-layer
 *   headers. This is important for inspection, sniffing, and custom protocol
 *   development.
 *
 * - ETH_P_ALL:
 *   Means "receive all Ethernet protocols", not just one specific EtherType.
 *   This is useful for a generic sniffer during early bring-up and testing.
 *
 * Why sudo/root is usually required:
 * Raw sockets bypass the normal transport/network stack abstractions and
 * expose low-level traffic directly. Because this can be used for sniffing,
 * crafting frames, and other sensitive operations, Linux usually restricts
 * raw socket creation to privileged users (root or CAP_NET_RAW capability).
 *
 * @param iface Name of the interface to bind to, e.g. "eth0".
 *
 * @return Socket file descriptor on success, or -1 on failure.
 */
int net_open_raw_socket(const char *iface)
{
    int sockfd;
    struct sockaddr_ll sll;
    unsigned int ifindex;

    if (iface == NULL || iface[0] == '\0') {
        LOG_ERROR("net_open_raw_socket: interface name is NULL or empty");
        return -1;
    }

    sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sockfd < 0) {
        LOG_ERROR("Failed to create raw socket on interface '%s': %s",
                  iface, strerror(errno));
        return -1;
    }

    /**
     * if_nametoindex() converts a human-readable interface name such as
     * "eth0" into the kernel interface index used by low-level APIs.
     */
    ifindex = if_nametoindex(iface);
    if (ifindex == 0U) {
        LOG_ERROR("Failed to resolve interface '%s': %s",
                  iface, strerror(errno));
        close(sockfd);
        return -1;
    }

    memset(&sll, 0, sizeof(sll));
    sll.sll_family   = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex  = (int)ifindex;

    /**
     * bind() attaches the raw socket to one specific interface so that the
     * process listens only on that interface instead of all interfaces.
     */
    if (bind(sockfd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        LOG_ERROR("Failed to bind raw socket to interface '%s': %s",
                  iface, strerror(errno));
        close(sockfd);
        return -1;
    }

    return sockfd;
}

/**
 * @brief Run a basic packet-sniffing loop on a raw socket.
 *
 * This function repeatedly calls recvfrom() until a fixed number of packets
 * have been received.
 *
 * About recvfrom():
 * - recvfrom() reads data from a socket into a user-provided buffer.
 * - For raw packet sockets, the received data includes the full Ethernet
 *   frame contents that fit into the provided buffer.
 * - By default, recvfrom() is a blocking system call. That means:
 *     - if no packet is available yet, the function waits,
 *     - the thread sleeps until data arrives or an error occurs.
 *   This behavior is actually useful for a simple sniffer because it avoids
 *   busy-waiting and unnecessary CPU usage.
 *
 * Why a 2048-byte buffer?
 * - A standard Ethernet MTU is 1500 bytes for payload.
 * - In addition to payload, an Ethernet frame includes headers such as the
 *   Ethernet header itself, and sometimes VLAN tags or other encapsulation.
 * - A 2048-byte buffer is comfortably larger than the normal 1500-byte MTU
 *   plus common Layer 2/Layer 3 headers, making it a safe and simple choice
 *   for standard Ethernet capture during initial testing.
 * - It is also a convenient power-of-two-sized buffer often used in practice.
 *
 * Error handling:
 * - If recvfrom() is interrupted by a signal (EINTR), we retry and continue.
 * - Other errors are logged and the loop continues, since transient failures
 *   should not necessarily stop packet capture.
 *
 * @param sockfd      Raw socket file descriptor.
 * @param max_packets Number of packets to capture before returning.
 */
void net_sniff_loop(int sockfd, int max_packets)
{
    unsigned char buffer[2048];
    int packets_received = 0;

    if (sockfd < 0) {
        LOG_ERROR("net_sniff_loop: invalid socket fd: %d", sockfd);
        return;
    }

    if (max_packets <= 0) {
        LOG_WARN("net_sniff_loop called with non-positive max_packets=%d",
                 max_packets);
        return;
    }

    LOG_INFO("Starting sniff loop, waiting for %d packets", max_packets);

    while (packets_received < max_packets) {
        ssize_t bytes_received;

        /**
         * recvfrom() blocks here until a frame arrives on the socket.
         *
         * Parameters:
         * - sockfd: the raw socket file descriptor
         * - buffer: destination memory for the received frame
         * - sizeof(buffer): maximum bytes we allow recvfrom() to write
         * - 0: no special flags
         * - NULL, NULL: we do not currently need source address metadata
         *
         * On success, recvfrom() returns the number of bytes captured.
         * On failure, it returns -1 and sets errno.
         */
        bytes_received = recvfrom(sockfd, buffer, sizeof(buffer), 0, NULL, NULL);

        if (bytes_received < 0) {
            if (errno == EINTR) {
                /**
                 * EINTR means the call was interrupted by a signal before
                 * data was received. This is not a fatal error; retry.
                 */
                LOG_WARN("recvfrom interrupted by signal, retrying");
                continue;
            }

            LOG_ERROR("recvfrom failed: %s", strerror(errno));
            continue;
        }

        LOG_DEBUG("Captured packet of size: %zd bytes", bytes_received);
        packets_received++;
    }

    LOG_INFO("Sniff loop finished after capturing %d packets", packets_received);
}