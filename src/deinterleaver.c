/*
 * src/deinterleaver.c — Receiver-side block collector implementation.
 *
 * Critical invariants:
 *
 *   1. block.symbols_per_block is set on every slot allocation via init_slot().
 *      fec_decode_block() uses this as scan_capacity.  If it is 0 the decoder
 *      rejects the call immediately.
 *
 *   2. block.symbols[] is fully zeroed on every slot allocation so that
 *      payload_len == 0 correctly identifies holes (lost symbols).
 *
 *   3. slot_reset() zeroes the entire slot_t re-establishing both invariants
 *      for the next occupant.
 *
 *   4. Symbols are stored at symbols[fec_id] — the array is sparse and
 *      indexed by fec_id, not by arrival order.
 */

#define _POSIX_C_SOURCE 200112L

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "deinterleaver.h"
#include "logging.h"
#include "types.h"

/* -------------------------------------------------------------------------- */
/* Internal constants                                                          */
/* -------------------------------------------------------------------------- */

#define MAX_FEC_ID MAX_SYMBOLS_PER_BLOCK   /* 256 */

/* -------------------------------------------------------------------------- */
/* Per-slot state                                                              */
/* -------------------------------------------------------------------------- */

typedef struct {
    int             occupied;
    int             ready;
    int             timed_out;
    uint32_t        block_id;
    int             symbol_count;
    uint8_t         received_mask[MAX_FEC_ID / 8 + 1];
    struct timespec first_sym_time;
    block_t         block;
} slot_t;

/* -------------------------------------------------------------------------- */
/* Main structure                                                              */
/* -------------------------------------------------------------------------- */

struct deinterleaver {
    int     max_active_blocks;
    int     symbols_per_block;
    size_t  symbol_size;
    slot_t *slots;
};

/* -------------------------------------------------------------------------- */
/* Bitset helpers                                                              */
/* -------------------------------------------------------------------------- */

static inline void bitset_set(uint8_t *mask, int pos)
{
    mask[pos / 8] |= (uint8_t)(1U << (unsigned)(pos % 8));
}

static inline int bitset_test(const uint8_t *mask, int pos)
{
    return (mask[pos / 8] >> (unsigned)(pos % 8)) & 1;
}

/* -------------------------------------------------------------------------- */
/* Wall-clock helper                                                           */
/* -------------------------------------------------------------------------- */

static void now_monotonic(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static double elapsed_ms(const struct timespec *start,
                         const struct timespec *end)
{
    double diff_s  = (double)(end->tv_sec  - start->tv_sec);
    double diff_ns = (double)(end->tv_nsec - start->tv_nsec);
    double ms      = diff_s * 1000.0 + diff_ns / 1.0e6;
    return (ms < 0.0) ? 0.0 : ms;
}

/* -------------------------------------------------------------------------- */
/* Slot helpers                                                                */
/* -------------------------------------------------------------------------- */

static void slot_reset(slot_t *s)
{
    memset(s, 0, sizeof(slot_t));
}

static void init_slot(slot_t *s, uint32_t block_id, int symbols_per_block)
{
    s->occupied                = 1;
    s->block_id                = block_id;
    s->block.block_id          = (uint64_t)block_id;
    s->block.symbol_count      = 0;
    s->block.symbols_per_block = symbols_per_block;
    now_monotonic(&s->first_sym_time);
}

static slot_t *find_slot(deinterleaver_t *self, uint32_t block_id)
{
    int i;

    for (i = 0; i < self->max_active_blocks; ++i) {
        if (self->slots[i].occupied &&
            self->slots[i].block_id == block_id)
        {
            return &self->slots[i];
        }
    }

    return NULL;
}

static slot_t *alloc_slot(deinterleaver_t *self, uint32_t block_id)
{
    int             i;
    slot_t         *oldest     = NULL;
    double          oldest_age = -1.0;
    struct timespec now;

    for (i = 0; i < self->max_active_blocks; ++i) {
        if (!self->slots[i].occupied) {
            slot_reset(&self->slots[i]);
            init_slot(&self->slots[i], block_id, self->symbols_per_block);
            return &self->slots[i];
        }
    }

    now_monotonic(&now);

    for (i = 0; i < self->max_active_blocks; ++i) {
        if (self->slots[i].ready) {
            continue;
        }

        {
            double age = elapsed_ms(&self->slots[i].first_sym_time, &now);

            if (age > oldest_age) {
                oldest_age = age;
                oldest     = &self->slots[i];
            }
        }
    }

    if (oldest == NULL) {
        LOG_WARN("[DIL] All %d slots are ready-but-undrained; "
                 "dropping symbol for block_id=%u",
                 self->max_active_blocks, (unsigned)block_id);
        return NULL;
    }

    LOG_WARN("[DIL] Evicting slot block_id=%u (age=%.1f ms, %d/%d symbols) "
             "to make room for block_id=%u",
             (unsigned)oldest->block_id, oldest_age,
             oldest->symbol_count, self->symbols_per_block,
             (unsigned)block_id);

    oldest->ready     = 1;
    oldest->timed_out = 1;
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

deinterleaver_t *deinterleaver_create(int    max_active_blocks,
                                      int    symbols_per_block,
                                      size_t symbol_size)
{
    deinterleaver_t *self;

    if (max_active_blocks < 1 || symbols_per_block < 1 ||
        symbol_size == 0 || symbols_per_block > MAX_SYMBOLS_PER_BLOCK)
    {
        LOG_ERROR("[DIL] deinterleaver_create: invalid parameters "
                  "max_active=%d spb=%d sym_size=%zu",
                  max_active_blocks, symbols_per_block, symbol_size);
        return NULL;
    }

    self = (deinterleaver_t *)malloc(sizeof(deinterleaver_t));
    if (self == NULL) {
        LOG_ERROR("[DIL] deinterleaver_create: malloc(deinterleaver_t) failed");
        return NULL;
    }

    memset(self, 0, sizeof(deinterleaver_t));

    self->max_active_blocks = max_active_blocks;
    self->symbols_per_block = symbols_per_block;
    self->symbol_size       = symbol_size;

    self->slots = (slot_t *)calloc((size_t)max_active_blocks, sizeof(slot_t));
    if (self->slots == NULL) {
        LOG_ERROR("[DIL] deinterleaver_create: calloc(slots) failed");
        free(self);
        return NULL;
    }

    LOG_INFO("[DIL] Created: max_active=%d symbols_per_block=%d "
             "symbol_size=%zu slot_size=%zu total=%zu bytes",
             max_active_blocks, symbols_per_block, symbol_size,
             sizeof(slot_t),
             sizeof(slot_t) * (size_t)max_active_blocks);

    return self;
}

void deinterleaver_destroy(deinterleaver_t *self)
{
    if (self == NULL) {
        return;
    }

    free(self->slots);
    free(self);
}

/* -------------------------------------------------------------------------- */
/* Ingestion                                                                   */
/* -------------------------------------------------------------------------- */

int deinterleaver_push_symbol(deinterleaver_t *self, const symbol_t *sym)
{
    slot_t  *slot;
    int      fec_pos;

    if (self == NULL || sym == NULL) {
        LOG_ERROR("[DIL] push_symbol: NULL argument");
        return -1;
    }

    /*
     * Hard safety guard:
     * payload_len == 0 is the simulator's erasure sentinel.  Such a symbol
     * must never be admitted into the sparse block array, otherwise a lost
     * position turns into a ghost symbol and the FEC layer will over-count
     * valid inputs.
     */
    if (sym->payload_len == 0) {
        LOG_DEBUG("[DIL] push_symbol: dropping erased symbol block_id=%u fec_id=%u",
                  (unsigned)sym->packet_id, (unsigned)sym->fec_id);
        return 0;
    }

    if ((size_t)sym->payload_len > self->symbol_size) {
        LOG_ERROR("[DIL] push_symbol: invalid payload_len=%u > symbol_size=%zu for block_id=%u fec_id=%u",
                  (unsigned)sym->payload_len,
                  self->symbol_size,
                  (unsigned)sym->packet_id,
                  (unsigned)sym->fec_id);
        return -1;
    }

    if (sym->total_symbols != 0 &&
        (int)sym->total_symbols != self->symbols_per_block)
    {
        LOG_WARN("[DIL] push_symbol: total_symbols=%u does not match deinterleaver N=%d for block_id=%u fec_id=%u",
                 (unsigned)sym->total_symbols,
                 self->symbols_per_block,
                 (unsigned)sym->packet_id,
                 (unsigned)sym->fec_id);
    }

    fec_pos = (int)sym->fec_id;

    if (fec_pos < 0 || fec_pos >= self->symbols_per_block) {
        LOG_ERROR("[DIL] push_symbol: fec_id=%u out of range [0..%d]",
                  (unsigned)sym->fec_id, self->symbols_per_block - 1);
        return -1;
    }

    slot = find_slot(self, sym->packet_id);

    if (slot == NULL) {
        slot = alloc_slot(self, sym->packet_id);

        if (slot == NULL) {
            return (deinterleaver_ready_count(self) > 0) ? 1 : -1;
        }
    }

    if (bitset_test(slot->received_mask, fec_pos)) {
        LOG_DEBUG("[DIL] Duplicate block_id=%u fec_id=%u — dropped",
                  (unsigned)sym->packet_id, (unsigned)sym->fec_id);
        return 0;
    }

    slot->block.symbols[fec_pos] = *sym;
    slot->block.symbols[fec_pos].total_symbols = (uint16_t)self->symbols_per_block;

    bitset_set(slot->received_mask, fec_pos);
    slot->symbol_count++;
    slot->block.symbol_count = slot->symbol_count;

    LOG_DEBUG("[DIL] block_id=%u fec_id=%u stored (%d/%d) payload_len=%u",
              (unsigned)sym->packet_id, (unsigned)sym->fec_id,
              slot->symbol_count, self->symbols_per_block,
              (unsigned)sym->payload_len);

    if (slot->symbol_count == self->symbols_per_block) {
        slot->ready = 1;

        LOG_INFO("[DIL] Block %u complete (%d symbols)",
                 (unsigned)sym->packet_id, slot->symbol_count);

        return 1;
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Retrieval                                                                   */
/* -------------------------------------------------------------------------- */

int deinterleaver_get_ready_block(deinterleaver_t *self, block_t *out_block)
{
    int i;

    if (self == NULL || out_block == NULL) {
        LOG_ERROR("[DIL] get_ready_block: NULL argument");
        return -1;
    }

    for (i = 0; i < self->max_active_blocks; ++i) {
        slot_t *s = &self->slots[i];

        if (s->occupied && s->ready) {
            LOG_INFO("[DIL] Releasing block_id=%u spb=%d sc=%d",
                     (unsigned)s->block_id,
                     s->block.symbols_per_block,
                     s->symbol_count);

            if (s->block.symbols_per_block == 0) {
                LOG_ERROR("[DIL] BUG: block_id=%u symbols_per_block=0 "
                          "at release — forcing to %d",
                          (unsigned)s->block_id,
                          self->symbols_per_block);
                s->block.symbols_per_block = self->symbols_per_block;
            }

            *out_block = s->block;

            LOG_INFO("[DIL] Released block_id=%u (%d symbols, %s)",
                     (unsigned)s->block_id,
                     s->symbol_count,
                     s->timed_out ? "timeout-flushed" : "complete");

            slot_reset(s);
            return 0;
        }
    }

    return -1;
}

/* -------------------------------------------------------------------------- */
/* Timeout management                                                          */
/* -------------------------------------------------------------------------- */

int deinterleaver_tick(deinterleaver_t *self, double timeout_ms)
{
    struct timespec now;
    int             newly_ready = 0;
    int             i;

    if (self == NULL) {
        return 0;
    }

    now_monotonic(&now);

    for (i = 0; i < self->max_active_blocks; ++i) {
        slot_t *s = &self->slots[i];

        if (!s->occupied || s->ready) {
            continue;
        }

        if (elapsed_ms(&s->first_sym_time, &now) >= timeout_ms) {
            LOG_WARN("[DIL] Timeout: block_id=%u flushed with %d/%d symbols",
                     (unsigned)s->block_id,
                     s->symbol_count,
                     self->symbols_per_block);

            s->ready     = 1;
            s->timed_out = 1;
            newly_ready++;
        }
    }

    return newly_ready;
}

/* -------------------------------------------------------------------------- */
/* Introspection                                                               */
/* -------------------------------------------------------------------------- */

int deinterleaver_active_blocks(const deinterleaver_t *self)
{
    int count = 0;
    int i;

    if (self == NULL) {
        return 0;
    }

    for (i = 0; i < self->max_active_blocks; ++i) {
        if (self->slots[i].occupied) {
            count++;
        }
    }

    return count;
}

int deinterleaver_ready_count(const deinterleaver_t *self)
{
    int count = 0;
    int i;

    if (self == NULL) {
        return 0;
    }

    for (i = 0; i < self->max_active_blocks; ++i) {
        if (self->slots[i].occupied && self->slots[i].ready) {
            count++;
        }
    }

    return count;
}