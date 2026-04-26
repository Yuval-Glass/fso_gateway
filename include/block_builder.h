#ifndef BLOCK_BUILDER_H
#define BLOCK_BUILDER_H

#include <time.h>

#include "types.h"

/**
 * @brief Runtime object that accumulates symbols into a FEC block.
 *
 * Unlike block_t, this builder also tracks timing state so that partially
 * filled blocks can be finalized after a timeout.
 */
typedef struct block_builder_t {
    uint64_t block_id;
    int symbol_count;
    int k_limit;
    symbol_t *symbols;
    struct timespec first_activity;
} block_builder_t;

/**
 * @brief Initialize a block builder for k data symbols.
 *
 * Allocates heap memory for k symbols and resets internal metadata.
 *
 * @param bb Pointer to block builder object.
 * @param k  Target number of data symbols in each block.
 *
 * @return 0 on success, -1 on failure.
 */
int block_builder_init(block_builder_t *bb, int k);

/**
 * @brief Add one symbol into the current block.
 *
 * Copies the symbol into the internal heap array and increments the count.
 * Updates the block activity timestamp.
 *
 * @param bb  Pointer to block builder.
 * @param sym Symbol to add.
 *
 * @return 1 if the block became full after this insertion,
 *         0 if symbol added but block is not yet full,
 *        -1 on error.
 */
int block_builder_add_symbol(block_builder_t *bb, const symbol_t *sym);

/**
 * @brief Check whether the current partially-filled block has timed out.
 *
 * If the block is empty, this returns 0.
 *
 * @param bb         Pointer to block builder.
 * @param timeout_ms Timeout threshold in milliseconds.
 *
 * @return 1 if timed out, 0 if not timed out, -1 on error.
 */
int block_builder_check_timeout(block_builder_t *bb, double timeout_ms);

/**
 * @brief Finalize a partial block by padding remaining symbol slots with zeros.
 *
 * Every remaining symbol from symbol_count to k_limit-1 is zero-filled.
 * After this function returns, symbol_count becomes k_limit.
 *
 * @param bb Pointer to block builder.
 */
void block_builder_finalize_with_padding(block_builder_t *bb);

/**
 * @brief Reset the builder for a new block.
 *
 * Clears the current symbol count and increments block_id so the next block
 * has a unique identifier.
 *
 * @param bb Pointer to block builder.
 */
void block_builder_reset(block_builder_t *bb);

/**
 * @brief Destroy the block builder and release heap memory.
 *
 * @param bb Pointer to block builder.
 */
void block_builder_destroy(block_builder_t *bb);

#endif /* BLOCK_BUILDER_H */