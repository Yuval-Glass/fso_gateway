/**
 * @file fso_protocol.c
 * @brief Implementation of FSO protocol header helpers.
 */

#include "fso_protocol.h"

#include <arpa/inet.h>
#include <stdio.h>

/**
 * @brief Initialize an FSO header in network byte order.
 *
 * The caller provides values in normal host byte order. This function converts
 * all multi-byte fields into network byte order using htons() and htonl().
 * That ensures the header has a consistent on-wire representation regardless of
 * whether the local CPU is little endian or big endian.
 *
 * @param header Pointer to the header to initialize.
 * @param seq_num Host-order sequence number.
 * @param type Frame type.
 * @param payload_len Host-order payload length in bytes.
 * @param crc32 Host-order CRC-32 value.
 *
 * @return 0 on success, -1 if header is NULL.
 */
int fso_protocol_init_header(struct fso_header *header,
                             uint16_t seq_num,
                             uint8_t type,
                             uint16_t payload_len,
                             uint32_t crc32)
{
    if (header == NULL) {
        return -1;
    }

    header->magic       = htonl(FSO_MAGIC_NUMBER);
    header->seq_num     = htons(seq_num);
    header->type        = type;
    header->payload_len = htons(payload_len);
    header->crc32       = htonl(crc32);

    return 0;
}

/**
 * @brief Print a header after converting multi-byte fields back to host order.
 *
 * The header is assumed to be stored in network byte order, which is the normal
 * representation for transmitted protocol headers. Before printing, each field
 * is converted with ntohs() or ntohl() so the displayed values are correct for
 * the local machine.
 *
 * @param header Pointer to the header to print.
 */
void fso_protocol_print_header(const struct fso_header *header)
{
    uint32_t magic_host;
    uint16_t seq_num_host;
    uint16_t payload_len_host;
    uint32_t crc32_host;

    if (header == NULL) {
        printf("FSO Header: (null)\n");
        return;
    }

    magic_host       = ntohl(header->magic);
    seq_num_host     = ntohs(header->seq_num);
    payload_len_host = ntohs(header->payload_len);
    crc32_host       = ntohl(header->crc32);

    printf("FSO Header {\n");
    printf("  magic       = 0x%08X\n", magic_host);
    printf("  seq_num     = %u\n", seq_num_host);
    printf("  type        = %u\n", header->type);
    printf("  payload_len = %u\n", payload_len_host);
    printf("  crc32       = 0x%08X\n", crc32_host);
    printf("}\n");
}