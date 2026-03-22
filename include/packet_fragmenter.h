#ifndef PACKET_FRAGMENTER_H
#define PACKET_FRAGMENTER_H

#include <stddef.h>
#include <stdint.h>

#include "types.h"

/**
 * @brief Fragment a raw packet buffer into fixed-size symbol_t objects.
 *
 * This function slices a raw Ethernet packet into multiple symbols according
 * to the configured symbol_size. Each symbol receives metadata describing:
 * - which packet it belongs to
 * - its index within the packet
 * - the total number of symbols for that packet
 * - the actual payload length stored in this symbol
 *
 * The packet payload bytes are copied into symbol_t::data.
 *
 * @param packet_data   Pointer to the raw packet bytes.
 * @param packet_len    Total packet length in bytes.
 * @param packet_id     Unique ID assigned to this packet.
 * @param symbol_size   Desired payload size per symbol.
 * @param out_symbols   Caller-provided output array of symbol_t.
 * @param max_symbols   Maximum number of entries available in out_symbols.
 *
 * @return Number of generated symbols on success.
 * @return -1 on error (invalid arguments, overflow, insufficient output space).
 */
int fragment_packet(const unsigned char *packet_data,
                    size_t packet_len,
                    uint32_t packet_id,
                    uint16_t symbol_size,
                    symbol_t *out_symbols,
                    uint16_t max_symbols);

#endif /* PACKET_FRAGMENTER_H */