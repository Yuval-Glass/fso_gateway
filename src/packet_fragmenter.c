#include "packet_fragmenter.h"

#include <string.h>

#include "logging.h"
#include "symbol.h"

/**
 * @brief Fragment a raw packet into symbol_t objects.
 *
 * The number of symbols is computed as:
 *
 *   total_symbols = (packet_len + symbol_size - 1) / symbol_size
 *
 * Each symbol receives a contiguous slice of the original packet buffer.
 * All symbols except the last one normally carry symbol_size bytes.
 * The last symbol carries only the remaining bytes.
 *
 * Safety checks:
 * - packet_data must not be NULL when packet_len > 0
 * - out_symbols must not be NULL
 * - symbol_size must be > 0
 * - symbol_size must fit inside symbol_t::data
 * - total_symbols must not exceed max_symbols
 *
 * @param packet_data   Pointer to input packet bytes.
 * @param packet_len    Length of the input packet.
 * @param packet_id     Packet identifier.
 * @param symbol_size   Requested symbol payload size.
 * @param out_symbols   Output symbol array.
 * @param max_symbols   Capacity of output symbol array.
 *
 * @return Number of produced symbols on success, -1 on failure.
 */
int fragment_packet(const unsigned char *packet_data,
                    size_t packet_len,
                    uint32_t packet_id,
                    uint16_t symbol_size,
                    symbol_t *out_symbols,
                    uint16_t max_symbols)
{
    size_t total_symbols_sz;
    uint16_t total_symbols;
    size_t offset;
    uint16_t i;

    if ((packet_data == NULL && packet_len > 0U) || out_symbols == NULL) {
        LOG_ERROR("fragment_packet: invalid NULL pointer argument");
        return -1;
    }

    if (symbol_size == 0U) {
        LOG_ERROR("fragment_packet: symbol_size must be greater than zero");
        return -1;
    }

    if (symbol_size > sizeof(out_symbols[0].data)) {
        LOG_ERROR("fragment_packet: symbol_size=%u exceeds symbol buffer capacity=%zu",
                  symbol_size, sizeof(out_symbols[0].data));
        return -1;
    }

    /*
     * Round up division:
     * total_symbols = ceil(packet_len / symbol_size)
     *
     * For packet_len == 0, this naturally becomes 0.
     */
    total_symbols_sz = (packet_len + (size_t)symbol_size - 1U) / (size_t)symbol_size;

    if (total_symbols_sz > (size_t)UINT16_MAX) {
        LOG_ERROR("fragment_packet: total_symbols overflow (%zu)", total_symbols_sz);
        return -1;
    }

    total_symbols = (uint16_t)total_symbols_sz;

    if (total_symbols > max_symbols) {
        LOG_ERROR("fragment_packet: need %u symbols but max_symbols=%u",
                  total_symbols, max_symbols);
        return -1;
    }

    /*
     * Special case:
     * An empty packet produces zero symbols.
     */
    if (packet_len == 0U) {
        return 0;
    }

    offset = 0U;

    for (i = 0U; i < total_symbols; ++i) {
        size_t remaining = packet_len - offset;
        uint16_t payload_len = (remaining > (size_t)symbol_size)
                                   ? symbol_size
                                   : (uint16_t)remaining;

        symbol_init(&out_symbols[i]);

        out_symbols[i].packet_id = packet_id;
        out_symbols[i].symbol_index = i;
        out_symbols[i].total_symbols = total_symbols;
        out_symbols[i].payload_len = payload_len;

        memcpy(out_symbols[i].data, packet_data + offset, payload_len);

        offset += payload_len;
    }

    return (int)total_symbols;
}