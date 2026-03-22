#ifndef PACKET_REASSEMBLER_H
#define PACKET_REASSEMBLER_H

#include <stddef.h>
#include <stdint.h>
#include "types.h"

/**
 * @brief Reassembles an original packet from an array of symbols.
 *
 * This function verifies that:
 *  - All symbols belong to the same packet_id
 *  - The symbol indices are valid
 *  - The symbols fit inside the output packet buffer
 *
 * The function places each symbol payload into the correct offset according to:
 *      offset = symbol_index * payload_len_base
 *
 * Since all symbols except the last are expected to have the same payload size,
 * the base symbol stride is derived from the non-last symbols.
 *
 * @param in_symbols        Input array of symbols.
 * @param num_symbols       Number of valid symbols in the input array.
 * @param out_packet_buffer Output buffer for the reconstructed packet.
 * @param max_packet_size   Maximum capacity of the output buffer.
 *
 * @return Total reconstructed packet length on success, or -1 on error.
 */
int reassemble_packet(const symbol_t *in_symbols,
                      uint16_t num_symbols,
                      unsigned char *out_packet_buffer,
                      size_t max_packet_size);

#endif /* PACKET_REASSEMBLER_H */