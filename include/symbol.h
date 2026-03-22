#ifndef SYMBOL_H
#define SYMBOL_H

/**
 * @file symbol.h
 * @brief Symbol lifecycle and debugging helpers.
 */

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize a symbol structure to a clean zeroed state.
 *
 * This function clears all metadata fields and the payload buffer.
 *
 * @param sym Pointer to the symbol to initialize.
 *
 * @note
 * Passing NULL is allowed; the function will simply return without action.
 */
void symbol_init(symbol_t *sym);

/**
 * @brief Dump symbol metadata to the debug log.
 *
 * This function prints the symbol's metadata fields using LOG_DEBUG.
 * It is intended for development, troubleshooting, and verification of the
 * packet slicing/reassembly pipeline.
 *
 * @param sym Pointer to the symbol to inspect.
 *
 * @note
 * Passing NULL is allowed; the function will log that a NULL symbol was given.
 *
 * @note
 * Only metadata is logged. The raw data buffer is intentionally not dumped,
 * to keep logs compact and avoid noisy output.
 */
void symbol_dump(const symbol_t *sym);

#ifdef __cplusplus
}
#endif

#endif /* SYMBOL_H */