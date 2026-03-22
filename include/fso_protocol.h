#ifndef FSO_PROTOCOL_H
#define FSO_PROTOCOL_H

/**
 * @file fso_protocol.h
 * @brief Definitions and helpers for the FSO frame header.
 *
 * This header defines the basic on-wire frame format used by the FSO Gateway.
 * The structure is marked as packed so that its memory layout matches the exact
 * byte layout expected on the link, without compiler-inserted padding bytes.
 *
 * The frame header fields are stored in network byte order (big endian) when
 * sent over the network or laser link.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fixed synchronization value used to identify valid FSO frames.
 *
 * Receivers can inspect incoming bytes and look for this magic number to detect
 * frame boundaries and reject garbage or misaligned data.
 */
#define FSO_MAGIC_NUMBER 0xDEADC0DEu

/**
 * @brief Example frame type: user data payload.
 */
#define FSO_FRAME_TYPE_DATA 0x01u

/**
 * @brief Example frame type: control/management message.
 */
#define FSO_FRAME_TYPE_CONTROL 0x02u

/**
 * @brief Example frame type: acknowledgment.
 */
#define FSO_FRAME_TYPE_ACK 0x03u

/**
 * @struct fso_header
 * @brief Packed FSO frame header stored in on-wire format.
 *
 * Field layout:
 * - magic:       Frame synchronization marker.
 * - seq_num:     Sequence number for ordering, loss detection, or replay control.
 * - type:        Frame type identifier.
 * - payload_len: Number of payload bytes following this header.
 * - crc32:       CRC-32 value used to validate frame integrity.
 *
 * Important:
 * This structure is declared packed so the compiler does not insert any padding
 * bytes between members. For protocols, exact byte-for-byte layout is required.
 *
 * All multi-byte fields must be stored in network byte order when transmitted.
 */
struct __attribute__((packed)) fso_header {
    uint32_t magic;
    uint16_t seq_num;
    uint8_t  type;
    uint16_t payload_len;
    uint32_t crc32;
};

/**
 * @brief Initialize an FSO header structure.
 *
 * This function fills the header fields and converts multi-byte values into
 * network byte order so the header is ready for transmission.
 *
 * @param header Pointer to the header structure to initialize.
 * @param seq_num Sequence number to assign to the frame.
 * @param type Frame type value.
 * @param payload_len Length of the payload in bytes.
 * @param crc32 CRC-32 value calculated over the appropriate frame content.
 *
 * @return 0 on success, -1 on invalid input.
 */
int fso_protocol_init_header(struct fso_header *header,
                             uint16_t seq_num,
                             uint8_t type,
                             uint16_t payload_len,
                             uint32_t crc32);

/**
 * @brief Print a human-readable representation of an FSO header.
 *
 * This function converts network-order fields back into host byte order before
 * printing, so logs show meaningful numeric values on the local machine.
 *
 * @param header Pointer to the header structure to print.
 */
void fso_protocol_print_header(const struct fso_header *header);

#ifdef __cplusplus
}
#endif

#endif /* FSO_PROTOCOL_H */