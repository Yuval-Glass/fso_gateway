#include "block_builder.h"

#include <stdlib.h>
#include <string.h>

#include "logging.h"

/**
 * @brief Get current monotonic time.
 *
 * CLOCK_MONOTONIC is used so timeout logic is not affected by wall-clock
 * adjustments such as NTP or manual time changes.
 *
 * @param ts Output timespec.
 *
 * @return 0 on success, -1 on failure.
 */
static int block_builder_get_now(struct timespec *ts)
{
    if (ts == NULL) {
        return -1;
    }

    if (clock_gettime(CLOCK_MONOTONIC, ts) != 0) {
        return -1;
    }

    return 0;
}

/**
 * @brief Compute elapsed time in milliseconds between two timestamps.
 *
 * @param start Start timestamp.
 * @param end   End timestamp.
 *
 * @return Elapsed milliseconds.
 */
static double block_builder_elapsed_ms(const struct timespec *start,
                                       const struct timespec *end)
{
    double sec_diff;
    double nsec_diff;

    sec_diff = (double)(end->tv_sec - start->tv_sec);
    nsec_diff = (double)(end->tv_nsec - start->tv_nsec);

    return (sec_diff * 1000.0) + (nsec_diff / 1000000.0);
}

int block_builder_init(block_builder_t *bb, int k)
{
    if (bb == NULL) {
        LOG_ERROR("block_builder_init: bb is NULL");
        return -1;
    }

    if (k <= 0) {
        LOG_ERROR("block_builder_init: invalid k=%d", k);
        return -1;
    }

    /*
     * Defensive initialization in case caller passed a non-zeroed structure.
     */
    bb->block_id = 0;
    bb->symbol_count = 0;
    bb->k_limit = 0;
    bb->symbols = NULL;
    bb->first_activity.tv_sec = 0;
    bb->first_activity.tv_nsec = 0;

    bb->symbols = (symbol_t *)malloc((size_t)k * sizeof(symbol_t));
    if (bb->symbols == NULL) {
        LOG_ERROR("block_builder_init: malloc failed for k=%d symbols", k);
        return -1;
    }

    memset(bb->symbols, 0, (size_t)k * sizeof(symbol_t));

    bb->block_id = 1U;
    bb->symbol_count = 0;
    bb->k_limit = k;
    bb->first_activity.tv_sec = 0;
    bb->first_activity.tv_nsec = 0;

    LOG_INFO("Block builder initialized: block_id=%lu k=%d",
             (unsigned long)bb->block_id,
             bb->k_limit);

    return 0;
}

int block_builder_add_symbol(block_builder_t *bb, const symbol_t *sym)
{
    if (bb == NULL || sym == NULL) {
        LOG_ERROR("block_builder_add_symbol: invalid input pointer");
        return -1;
    }

    if (bb->symbols == NULL) {
        LOG_ERROR("block_builder_add_symbol: builder not initialized");
        return -1;
    }

    if (bb->k_limit <= 0) {
        LOG_ERROR("block_builder_add_symbol: invalid k_limit=%d", bb->k_limit);
        return -1;
    }

    if (bb->symbol_count < 0 || bb->symbol_count >= bb->k_limit) {
        LOG_ERROR("block_builder_add_symbol: block is already full "
                  "(block_id=%lu symbol_count=%d k_limit=%d)",
                  (unsigned long)bb->block_id,
                  bb->symbol_count,
                  bb->k_limit);
        return -1;
    }

    bb->symbols[bb->symbol_count] = *sym;
    bb->symbol_count++;

    /* Record timestamp only on the first symbol so the timeout measures
     * age of the block, not idle time since the last packet. */
    if (bb->symbol_count == 1) {
        if (block_builder_get_now(&bb->first_activity) != 0) {
            LOG_ERROR("block_builder_add_symbol: clock_gettime failed");
            return -1;
        }
    }

    if (bb->symbol_count == bb->k_limit) {
        return 1;
    }

    return 0;
}

int block_builder_check_timeout(block_builder_t *bb, double timeout_ms)
{
    struct timespec now;
    double elapsed_ms;

    if (bb == NULL) {
        LOG_ERROR("block_builder_check_timeout: bb is NULL");
        return -1;
    }

    if (timeout_ms < 0.0) {
        LOG_ERROR("block_builder_check_timeout: invalid timeout_ms=%.3f", timeout_ms);
        return -1;
    }

    if (bb->symbol_count <= 0) {
        return 0;
    }

    if (block_builder_get_now(&now) != 0) {
        LOG_ERROR("block_builder_check_timeout: clock_gettime failed");
        return -1;
    }

    elapsed_ms = block_builder_elapsed_ms(&bb->first_activity, &now);

    if (elapsed_ms >= timeout_ms) {
        return 1;
    }

    return 0;
}

void block_builder_finalize_with_padding(block_builder_t *bb)
{
    int i;
    int padding_count;

    if (bb == NULL) {
        return;
    }

    if (bb->symbols == NULL) {
        return;
    }

    if (bb->k_limit <= 0) {
        return;
    }

    if (bb->symbol_count < 0 || bb->symbol_count > bb->k_limit) {
        return;
    }

    padding_count = bb->k_limit - bb->symbol_count;

    for (i = bb->symbol_count; i < bb->k_limit; ++i) {
        memset(&bb->symbols[i], 0, sizeof(symbol_t));
    }

    LOG_DEBUG("block_builder_finalize_with_padding: block_id=%lu original_symbols=%d padding=%d",
              (unsigned long)bb->block_id,
              bb->symbol_count,
              padding_count);

    bb->symbol_count = bb->k_limit;
}

void block_builder_reset(block_builder_t *bb)
{
    if (bb == NULL) {
        return;
    }

    if (bb->symbols != NULL && bb->k_limit > 0) {
        memset(bb->symbols, 0, (size_t)bb->k_limit * sizeof(symbol_t));
    }

    /*
     * Reset logical state only. Do not free here, so repeated reset calls are safe
     * and do not cause double-free issues.
     */
    bb->symbol_count = 0;

    if (bb->block_id != 0) {
        bb->block_id++;
    } else {
        bb->block_id = 1U;
    }

    bb->first_activity.tv_sec = 0;
    bb->first_activity.tv_nsec = 0;
}

void block_builder_destroy(block_builder_t *bb)
{
    if (bb == NULL) {
        return;
    }

    /*
     * free(NULL) is safe, so repeated destroy calls are harmless.
     */
    free(bb->symbols);
    bb->symbols = NULL;

    bb->block_id = 0;
    bb->symbol_count = 0;
    bb->k_limit = 0;
    bb->first_activity.tv_sec = 0;
    bb->first_activity.tv_nsec = 0;
}