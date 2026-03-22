/*
 * src/interleaver.c — Symbol-level matrix interleaver implementation.
 *
 * Row mapping:  row = sym->packet_id % depth
 * Column mapping: col = sym->fec_id  (0..N-1)
 *
 * REQUIREMENT on callers: every symbol pushed must have packet_id set to
 * the block's sequence number (0..depth-1 within one window).  This
 * includes repair symbols — the caller is responsible for stamping
 * packet_id on repair symbols before calling push_symbol.
 *
 * Duplicate detection uses the per-column received_mask bitset, which
 * gives an O(1) exact answer for any (row, col) pair with no false
 * positives.  The previous `symbol_count > col` proxy was incorrect.
 */

#define _POSIX_C_SOURCE 200112L

#include <stdlib.h>
#include <string.h>

#include "interleaver.h"
#include "logging.h"
#include "types.h"

/* -------------------------------------------------------------------------- */
/* Internal constants                                                          */
/* -------------------------------------------------------------------------- */

/* Maximum column index (fec_id) the interleaver will accept.               */
#define MAX_COL MAX_SYMBOLS_PER_BLOCK   /* 256, from types.h                 */

/* -------------------------------------------------------------------------- */
/* Per-slot (per-row) metadata                                                */
/* -------------------------------------------------------------------------- */

typedef struct {
    int     symbol_count;                  /* distinct columns filled         */
    int     occupied;                      /* 1 after first symbol arrives    */
    uint8_t received_mask[MAX_COL / 8 + 1]; /* per-column occupancy bitset   */
} slot_meta_t;

/* -------------------------------------------------------------------------- */
/* Opaque interleaver struct                                                   */
/* -------------------------------------------------------------------------- */

struct interleaver {
    int      depth;
    int      n;
    int      symbol_size;

    symbol_t    *matrix;   /* depth × n contiguous symbol_t cells            */
    slot_meta_t *slots;    /* depth slot_meta_t entries                      */

    int complete_slots;    /* rows where symbol_count == n                   */
    int pop_col;
    int pop_row;
    int ready;
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
/* Matrix cell access                                                          */
/* -------------------------------------------------------------------------- */

static inline symbol_t *cell(interleaver_t *il, int row, int col)
{
    return &il->matrix[row * il->n + col];
}

static inline const symbol_t *cell_const(const interleaver_t *il,
                                          int row, int col)
{
    return &il->matrix[row * il->n + col];
}

/* -------------------------------------------------------------------------- */
/* Window reset — zeros matrix, slots, and all cursors                       */
/* -------------------------------------------------------------------------- */

static void reset_window(interleaver_t *il)
{
    memset(il->matrix, 0,
           sizeof(symbol_t) * (size_t)il->depth * (size_t)il->n);
    memset(il->slots,  0,
           sizeof(slot_meta_t) * (size_t)il->depth);

    il->complete_slots = 0;
    il->pop_col        = 0;
    il->pop_row        = 0;
    il->ready          = 0;
}

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

interleaver_t *interleaver_create(int depth, int k_plus_m, int symbol_size)
{
    interleaver_t *il;
    size_t         matrix_bytes;
    size_t         slots_bytes;

    if (depth < 2 || k_plus_m < 1 || k_plus_m > MAX_COL ||
        symbol_size <= 0)
    {
        LOG_ERROR("[IL] create: invalid params depth=%d n=%d sym_size=%d",
                  depth, k_plus_m, symbol_size);
        return NULL;
    }

    il = (interleaver_t *)malloc(sizeof(interleaver_t));
    if (il == NULL) {
        LOG_ERROR("[IL] create: malloc(interleaver_t) failed");
        return NULL;
    }

    memset(il, 0, sizeof(interleaver_t));

    il->depth       = depth;
    il->n           = k_plus_m;
    il->symbol_size = symbol_size;

    matrix_bytes = sizeof(symbol_t) * (size_t)depth * (size_t)k_plus_m;
    il->matrix   = (symbol_t *)malloc(matrix_bytes);
    if (il->matrix == NULL) {
        LOG_ERROR("[IL] create: malloc(matrix) failed (%zu bytes)",
                  matrix_bytes);
        free(il);
        return NULL;
    }

    slots_bytes = sizeof(slot_meta_t) * (size_t)depth;
    il->slots   = (slot_meta_t *)malloc(slots_bytes);
    if (il->slots == NULL) {
        LOG_ERROR("[IL] create: malloc(slots) failed");
        free(il->matrix);
        free(il);
        return NULL;
    }

    reset_window(il);

    LOG_INFO("[IL] Created: depth=%d n=%d symbol_size=%d "
             "matrix_bytes=%zu",
             depth, k_plus_m, symbol_size, matrix_bytes);

    return il;
}

void interleaver_destroy(interleaver_t *il)
{
    if (il == NULL) {
        return;
    }

    free(il->matrix);
    free(il->slots);
    free(il);
}

/* -------------------------------------------------------------------------- */
/* Enqueue — row-major write                                                  */
/* -------------------------------------------------------------------------- */

int interleaver_push_symbol(interleaver_t *il, const symbol_t *sym)
{
    int          row;
    int          col;
    slot_meta_t *slot;
    symbol_t    *dst;

    if (il == NULL || sym == NULL) {
        LOG_ERROR("[IL] push: NULL argument");
        return -1;
    }

    if (il->ready) {
        LOG_WARN("[IL] push: matrix is draining — "
                 "packet_id=%u fec_id=%u rejected",
                 (unsigned)sym->packet_id, (unsigned)sym->fec_id);
        return -1;
    }

    col = (int)sym->fec_id;
    if (col < 0 || col >= il->n) {
        LOG_ERROR("[IL] push: fec_id=%u out of range [0..%d] "
                  "(packet_id=%u)",
                  (unsigned)sym->fec_id, il->n - 1,
                  (unsigned)sym->packet_id);
        return -1;
    }

    row  = (int)((uint32_t)sym->packet_id % (uint32_t)il->depth);
    slot = &il->slots[row];

    /*
     * Duplicate detection via the per-column bitset.
     * This is the authoritative check — symbol_count is never used for
     * duplicate detection, only for completion tracking.
     */
    if (bitset_test(slot->received_mask, col)) {
        LOG_DEBUG("[IL] push: duplicate packet_id=%u fec_id=%u "
                  "(row=%d col=%d) — dropped",
                  (unsigned)sym->packet_id, (unsigned)sym->fec_id,
                  row, col);
        return 0;
    }

    /* Store the symbol */
    dst  = cell(il, row, col);
    *dst = *sym;

    bitset_set(slot->received_mask, col);
    slot->symbol_count++;
    slot->occupied = 1;

    LOG_DEBUG("[IL] push: packet_id=%u fec_id=%u -> row=%d col=%d "
              "slot_count=%d/%d complete_slots=%d/%d",
              (unsigned)sym->packet_id, (unsigned)sym->fec_id,
              row, col,
              slot->symbol_count, il->n,
              il->complete_slots, il->depth);

    /* Row complete? */
    if (slot->symbol_count == il->n) {
        il->complete_slots++;

        LOG_DEBUG("[IL] Row %d complete — complete_slots=%d/%d",
                  row, il->complete_slots, il->depth);

        /* Window complete? */
        if (il->complete_slots == il->depth) {
            il->ready   = 1;
            il->pop_col = 0;
            il->pop_row = 0;

            LOG_INFO("[IL] Window ready: depth=%d × n=%d = %d symbols",
                     il->depth, il->n, il->depth * il->n);

            return 1;
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Dequeue — column-major read                                                */
/* -------------------------------------------------------------------------- */

int interleaver_pop_ready_symbol(interleaver_t *il, symbol_t *out_sym)
{
    const symbol_t *src;
    int             last;

    if (il == NULL || out_sym == NULL) {
        LOG_ERROR("[IL] pop: NULL argument");
        return -1;
    }

    if (!il->ready) {
        return -1;
    }

    src      = cell_const(il, il->pop_row, il->pop_col);
    *out_sym = *src;

    LOG_DEBUG("[IL] pop: row=%d col=%d -> packet_id=%u fec_id=%u",
              il->pop_row, il->pop_col,
              (unsigned)out_sym->packet_id,
              (unsigned)out_sym->fec_id);

    il->pop_row++;
    if (il->pop_row == il->depth) {
        il->pop_row = 0;
        il->pop_col++;
    }

    last = (il->pop_col == il->n);
    if (last) {
        LOG_INFO("[IL] Window drained — resetting");
        reset_window(il);
        return 1;
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Introspection                                                               */
/* -------------------------------------------------------------------------- */

int interleaver_is_ready(const interleaver_t *il)
{
    return (il != NULL) ? il->ready : 0;
}

int interleaver_symbols_pending(const interleaver_t *il)
{
    int drained;

    if (il == NULL || !il->ready) {
        return 0;
    }

    drained = il->pop_col * il->depth + il->pop_row;
    return il->depth * il->n - drained;
}