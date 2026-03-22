/**
 * @file symbol.c
 * @brief Implementation of symbol lifecycle and debug helpers.
 */

#include <string.h>

#include "symbol.h"
#include "logging.h"

/**
 * @brief Initialize a symbol structure to zero.
 *
 * We use memset() here because symbol_t is currently a plain-old-data struct
 * containing only integer fields and a byte buffer. Zeroing the whole object
 * gives us a predictable clean state:
 *
 * - packet_id     = 0
 * - symbol_index  = 0
 * - total_symbols = 0
 * - payload_len   = 0
 * - data[]        = all zero
 *
 * This is convenient for:
 * - avoiding stale data reuse
 * - preparing reusable symbol buffers
 * - ensuring deterministic debug behavior
 *
 * @param sym Pointer to the symbol to initialize.
 */
void symbol_init(symbol_t *sym)
{
    if (sym == NULL) {
        return;
    }

    memset(sym, 0, sizeof(*sym));
}

/**
 * @brief Log the metadata of a symbol at debug level.
 *
 * This function only logs metadata fields because:
 * - metadata is what we need for validating slicing/reassembly/interleaving
 * - dumping full payloads would make logs very noisy
 * - payload bytes may be binary and not human-readable
 *
 * @param sym Pointer to the symbol to log.
 */
void symbol_dump(const symbol_t *sym)
{
    if (sym == NULL) {
        LOG_DEBUG("symbol_dump called with NULL symbol");
        return;
    }

    LOG_DEBUG(
        "Symbol dump: packet_id=%u, symbol_index=%u, total_symbols=%u, payload_len=%u",
        sym->packet_id,
        sym->symbol_index,
        sym->total_symbols,
        sym->payload_len
    );
}