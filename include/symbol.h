#ifndef SYMBOL_H
#define SYMBOL_H

/**
 * @file symbol.h
 * @brief Symbol lifecycle, CRC-32C helpers, and debugging utilities.
 *
 * CRC algorithm
 * -------------
 * CRC-32C (Castagnoli), polynomial 0x1EDC6F41 (reflected: 0x82F63B78).
 * This is the polynomial used by iSCSI, SCTP, Btrfs, and hardware-accelerated
 * CRC instructions (SSE4.2 _mm_crc32_u8/u32/u64, ARMv8 CRC32C).
 *
 * The software implementation uses a pre-computed 256-entry table and is
 * structured so that a future hardware-accelerated backend can be substituted
 * by replacing the single internal update primitive without changing the API.
 *
 * CRC coverage
 * ------------
 * The internal per-symbol CRC-32C protects exactly the following fields,
 * serialised big-endian (14 bytes of header, then payload_len bytes of data):
 *
 *   packet_id     (4 bytes, big-endian)
 *   fec_id        (4 bytes, big-endian)
 *   symbol_index  (2 bytes, big-endian)
 *   total_symbols (2 bytes, big-endian)
 *   payload_len   (2 bytes, big-endian)
 *   data[0..payload_len-1]  (payload_len bytes)
 *
 * Padding bytes beyond payload_len in data[] are EXCLUDED — determinism
 * is preserved regardless of how the buffer was initialised.
 * The crc32 field itself is never included in the CRC input.
 *
 * TX path (source and repair symbols):
 *   1. Populate all protected fields fully.
 *   2. Call symbol_compute_crc(sym) to stamp sym->crc32.
 *   3. Push to the interleaver.
 *   Only call when internal_symbol_crc_enabled != 0.
 *
 * RX path:
 *   1. If internal_symbol_crc_enabled != 0, call symbol_verify_crc(sym).
 *   2. On return 0 — discard, treat as erasure.
 *   3. On return 1 — forward to deinterleaver_push_symbol().
 */

#include <stdint.h>
#include <stddef.h>
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize a symbol to a clean zeroed state.
 * Passing NULL is a no-op.
 */
void symbol_init(symbol_t *sym);

/**
 * @brief Dump symbol metadata (including crc32) to the debug log.
 * Only metadata is printed; the payload buffer is not dumped.
 * Passing NULL logs a note and returns.
 */
void symbol_dump(const symbol_t *sym);

/**
 * @brief Compute and stamp the CRC-32C for a symbol.
 *
 * Computes CRC-32C (Castagnoli, reflected polynomial 0x82F63B78) over the
 * 14-byte serialised header and data[0..payload_len-1], then stores the
 * result in sym->crc32.  Any existing crc32 value is overwritten.
 *
 * Must be called after all protected fields are fully populated.
 * Only call when the CRC feature is enabled.
 *
 * @param sym  Symbol to stamp.  NULL is a no-op.
 */
void symbol_compute_crc(symbol_t *sym);

/**
 * @brief Verify the CRC-32C of a received symbol.
 *
 * Recomputes CRC-32C over the same fields as symbol_compute_crc() and
 * compares to sym->crc32.
 *
 * Returns 1 if CRC matches (symbol intact).
 * Returns 0 if CRC mismatches (corrupted) or sym is NULL.
 *
 * On return 0 the caller MUST discard the symbol and treat it as an erasure.
 * Only call when the CRC feature is enabled.
 *
 * @param sym  Symbol to verify.
 * @return 1 on match, 0 on mismatch or NULL.
 */
int symbol_verify_crc(const symbol_t *sym);

/**
 * @brief Compute CRC-32C over an arbitrary byte buffer.
 *
 * Pure software CRC-32C (Castagnoli), reflected polynomial 0x82F63B78.
 * Initial seed 0xFFFFFFFF, final XOR 0xFFFFFFFF.
 *
 * @param data    Input bytes.
 * @param length  Number of bytes.
 * @return CRC-32C value.
 */
uint32_t symbol_crc32c(const unsigned char *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* SYMBOL_H */
