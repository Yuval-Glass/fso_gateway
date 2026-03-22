#include "packet_reassembler.h"

#include <string.h>

#include "logging.h"

/**
 * @brief Reassemble a packet from an ordered/unordered collection of symbols.
 *
 * Design notes:
 * - All symbols must belong to the same packet_id.
 * - symbol_index tells us where each symbol belongs in the packet.
 * - payload_len tells us how many bytes to copy from that symbol.
 * - total_symbols should be consistent across all symbols.
 *
 * Assumption:
 * - All non-last symbols use the same payload size (the configured symbol_size
 *   used during fragmentation).
 * - The last symbol may be shorter.
 *
 * Therefore, packet offsets are reconstructed as:
 *      offset = symbol_index * base_symbol_size
 *
 * where base_symbol_size is inferred from the first non-last symbol found.
 *
 * @param in_symbols        Input symbol array.
 * @param num_symbols       Number of symbols in input array.
 * @param out_packet_buffer Destination packet buffer.
 * @param max_packet_size   Size of destination buffer.
 *
 * @return Reconstructed packet length on success, -1 on failure.
 */
int reassemble_packet(const symbol_t *in_symbols,
                      uint16_t num_symbols,
                      unsigned char *out_packet_buffer,
                      size_t max_packet_size)
{
    uint32_t packet_id;
    uint16_t expected_total_symbols;
    uint16_t i;
    uint16_t base_symbol_size = 0;
    size_t reconstructed_len = 0;
    int found_non_last = 0;

    if (in_symbols == NULL || out_packet_buffer == NULL || num_symbols == 0) {
        LOG_ERROR("reassemble_packet: invalid input pointer or zero symbols");
        return -1;
    }

    packet_id = in_symbols[0].packet_id;
    expected_total_symbols = in_symbols[0].total_symbols;

    if (expected_total_symbols == 0) {
        LOG_ERROR("reassemble_packet: invalid total_symbols=0");
        return -1;
    }

    if (num_symbols != expected_total_symbols) {
        LOG_ERROR("reassemble_packet: num_symbols=%u does not match expected total_symbols=%u",
                  num_symbols, expected_total_symbols);
        return -1;
    }

    /*
     * Infer the stride (symbol size used during fragmentation).
     * Any symbol that is not the last one should have the fixed full size.
     */
    for (i = 0; i < num_symbols; ++i) {
        if (in_symbols[i].symbol_index >= expected_total_symbols) {
            LOG_ERROR("reassemble_packet: invalid symbol_index=%u (expected < %u)",
                      in_symbols[i].symbol_index, expected_total_symbols);
            return -1;
        }

        if (in_symbols[i].packet_id != packet_id) {
            LOG_ERROR("reassemble_packet: packet_id mismatch at symbol %u", i);
            return -1;
        }

        if (in_symbols[i].total_symbols != expected_total_symbols) {
            LOG_ERROR("reassemble_packet: total_symbols mismatch at symbol %u", i);
            return -1;
        }

        if (in_symbols[i].symbol_index != (expected_total_symbols - 1U)) {
            base_symbol_size = in_symbols[i].payload_len;
            found_non_last = 1;
            break;
        }
    }

    /*
     * Special case: packet consists of exactly one symbol.
     * In that case, the full packet length is simply payload_len of that symbol.
     */
    if (!found_non_last) {
        if (in_symbols[0].payload_len > max_packet_size) {
            LOG_ERROR("reassemble_packet: single-symbol packet exceeds output buffer");
            return -1;
        }

        memcpy(out_packet_buffer, in_symbols[0].data, in_symbols[0].payload_len);
        return (int)in_symbols[0].payload_len;
    }

    if (base_symbol_size == 0) {
        LOG_ERROR("reassemble_packet: inferred base_symbol_size is zero");
        return -1;
    }

    /*
     * Zero the destination buffer first so any unused tail remains clean.
     */
    memset(out_packet_buffer, 0, max_packet_size);

    /*
     * Copy each symbol into its correct offset using symbol_index.
     */
    for (i = 0; i < num_symbols; ++i) {
        size_t offset;
        size_t end_offset;

        offset = (size_t)in_symbols[i].symbol_index * (size_t)base_symbol_size;
        end_offset = offset + (size_t)in_symbols[i].payload_len;

        if (end_offset > max_packet_size) {
            LOG_ERROR("reassemble_packet: symbol %u would exceed output buffer "
                      "(offset=%zu payload_len=%u max=%zu)",
                      i,
                      offset,
                      in_symbols[i].payload_len,
                      max_packet_size);
            return -1;
        }

        memcpy(out_packet_buffer + offset,
               in_symbols[i].data,
               in_symbols[i].payload_len);

        if (end_offset > reconstructed_len) {
            reconstructed_len = end_offset;
        }
    }

    return (int)reconstructed_len;
}