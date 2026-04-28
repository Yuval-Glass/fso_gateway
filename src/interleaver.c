/*
 * src/interleaver.c — Symbol-level matrix interleaver implementation.
 *
 * Row mapping
 * -----------
 * All symbols of the same FEC block must map to the same row.  Callers stamp
 * sym->packet_id with a stable per-block key and the interleaver assigns rows
 * in arrival order within the current window.
 *
 * Duplicate detection uses the per-column received_mask bitset, which gives
 * an O(1) exact answer for any (row, col) pair with no false positives.
 */

#define _POSIX_C_SOURCE 200112L

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "interleaver.h"
#include "logging.h"
#include "types.h"

/* -------------------------------------------------------------------------- */
/* Internal constants                                                          */
/* -------------------------------------------------------------------------- */

#define MAX_COL MAX_SYMBOLS_PER_BLOCK

/* -------------------------------------------------------------------------- */
/* Per-slot (per-row) metadata                                                */
/* -------------------------------------------------------------------------- */

typedef struct {
    int      symbol_count;
    int      occupied;
    uint8_t  received_mask[MAX_COL / 8 + 1];
} slot_meta_t;

/* -------------------------------------------------------------------------- */
/* Opaque interleaver struct                                                   */
/* -------------------------------------------------------------------------- */

struct interleaver {
    int         depth;
    int         n;
    int         symbol_size;

    symbol_t   *matrix;
    slot_meta_t *slots;

    uint32_t   *row_block_key;
    int        *row_assigned;

    int         window_block_count;
    int         flush_timeout_ms;
    struct timespec window_start;
    int         window_has_data;

    int         complete_slots;
    int         pop_col;
    int         pop_row;
    int         ready;
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
/* Time helpers                                                                */
/* -------------------------------------------------------------------------- */

static void interleaver_get_now(struct timespec *ts)
{
    if (ts == NULL) {
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, ts);
}

static double interleaver_elapsed_ms(const struct timespec *start,
                                     const struct timespec *end)
{
    double sec_diff;
    double nsec_diff;
    double elapsed;

    sec_diff = (double)(end->tv_sec - start->tv_sec);
    nsec_diff = (double)(end->tv_nsec - start->tv_nsec);
    elapsed = sec_diff * 1000.0 + nsec_diff / 1000000.0;

    if (elapsed < 0.0) {
        return 0.0;
    }

    return elapsed;
}

/* -------------------------------------------------------------------------- */
/* Matrix cell access                                                          */
/* -------------------------------------------------------------------------- */

static inline symbol_t *cell(interleaver_t *il, int row, int col)
{
    return &il->matrix[row * il->n + col];
}

static inline const symbol_t *cell_const(const interleaver_t *il,
                                         int row,
                                         int col)
{
    return &il->matrix[row * il->n + col];
}

/* -------------------------------------------------------------------------- */
/* Window reset                                                                */
/* -------------------------------------------------------------------------- */

static void reset_window(interleaver_t *il)
{
    if (il == NULL) {
        return;
    }

    /* Don't memset the matrix — push_symbol overwrites every field before pop reads it. */
    memset(il->slots, 0,
           sizeof(slot_meta_t) * (size_t)il->depth);
    memset(il->row_block_key, 0,
           sizeof(uint32_t) * (size_t)il->depth);
    memset(il->row_assigned, 0,
           sizeof(int) * (size_t)il->depth);

    il->window_block_count = 0;
    il->window_start.tv_sec = 0;
    il->window_start.tv_nsec = 0;
    il->window_has_data    = 0;
    il->complete_slots     = 0;
    il->pop_col            = 0;
    il->pop_row            = 0;
    il->ready              = 0;
}

/* -------------------------------------------------------------------------- */
/* Row helpers                                                                 */
/* -------------------------------------------------------------------------- */

static int find_row_for_block_key(const interleaver_t *il, uint32_t block_key)
{
    int row;

    if (il == NULL) {
        return -1;
    }

    for (row = 0; row < il->depth; ++row) {
        if (il->row_assigned[row] && il->row_block_key[row] == block_key) {
            return row;
        }
    }

    return -1;
}

static int assign_row_for_block_key(interleaver_t *il, uint32_t block_key)
{
    int row;

    if (il == NULL) {
        LOG_ERROR("[IL] assign_row_for_block_key: il is NULL");
        return -1;
    }

    if (il->window_block_count >= il->depth) {
        LOG_ERROR("[IL] assign_row_for_block_key: window already has %d rows "
                  "assigned (depth=%d, block_key=%u)",
                  il->window_block_count,
                  il->depth,
                  (unsigned)block_key);
        return -1;
    }

    row = il->window_block_count % il->depth;
    il->row_block_key[row] = block_key;
    il->row_assigned[row] = 1;
    il->window_block_count++;

    return row;
}

static int resolve_row(interleaver_t *il, uint32_t block_key)
{
    int row;

    row = find_row_for_block_key(il, block_key);
    if (row >= 0) {
        return row;
    }

    return assign_row_for_block_key(il, block_key);
}

static void force_fill_row(interleaver_t *il, int row)
{
    slot_meta_t *slot;
    uint32_t     block_key;
    int          col;
    symbol_t    *dst;

    slot = &il->slots[row];
    block_key = il->row_assigned[row] ? il->row_block_key[row] : 0U;

    for (col = 0; col < il->n; ++col) {
        if (bitset_test(slot->received_mask, col)) {
            continue;
        }

        dst = cell(il, row, col);
        dst->packet_id = block_key;
        dst->fec_id = (uint32_t)col;
        dst->symbol_index = (uint16_t)col;
        dst->total_symbols = (uint16_t)il->n;
        dst->payload_len = 0;
        dst->crc32 = 0U;
        memset(dst->data, 0, (size_t)il->symbol_size);

        bitset_set(slot->received_mask, col);
    }

    slot->symbol_count = il->n;
    slot->occupied = 1;
    if (!il->row_assigned[row]) {
        il->row_assigned[row] = 1;
        il->row_block_key[row] = block_key;
    }
}

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

interleaver_t *interleaver_create(int depth,
                                  int k_plus_m,
                                  int symbol_size,
                                  int flush_timeout_ms)
{
    interleaver_t *il;
    size_t         matrix_bytes;
    size_t         slots_bytes;
    size_t         row_key_bytes;
    size_t         row_assigned_bytes;

    if (depth < 1 || k_plus_m < 1 || k_plus_m > MAX_COL || symbol_size <= 0) {
        LOG_ERROR("[IL] create: invalid params depth=%d n=%d sym_size=%d",
                  depth, k_plus_m, symbol_size);
        return NULL;
    }

    if (flush_timeout_ms < 0) {
        LOG_ERROR("[IL] create: invalid flush_timeout_ms=%d",
                  flush_timeout_ms);
        return NULL;
    }

    il = (interleaver_t *)malloc(sizeof(interleaver_t));
    if (il == NULL) {
        LOG_ERROR("[IL] create: malloc(interleaver_t) failed");
        return NULL;
    }

    memset(il, 0, sizeof(interleaver_t));

    il->depth = depth;
    il->n = k_plus_m;
    il->symbol_size = symbol_size;
    il->flush_timeout_ms = flush_timeout_ms;

    matrix_bytes = sizeof(symbol_t) * (size_t)depth * (size_t)k_plus_m;
    il->matrix = (symbol_t *)malloc(matrix_bytes);
    if (il->matrix == NULL) {
        LOG_ERROR("[IL] create: malloc(matrix) failed (%zu bytes)",
                  matrix_bytes);
        free(il);
        return NULL;
    }

    slots_bytes = sizeof(slot_meta_t) * (size_t)depth;
    il->slots = (slot_meta_t *)malloc(slots_bytes);
    if (il->slots == NULL) {
        LOG_ERROR("[IL] create: malloc(slots) failed");
        free(il->matrix);
        free(il);
        return NULL;
    }

    row_key_bytes = sizeof(uint32_t) * (size_t)depth;
    il->row_block_key = (uint32_t *)malloc(row_key_bytes);
    if (il->row_block_key == NULL) {
        LOG_ERROR("[IL] create: malloc(row_block_key) failed");
        free(il->slots);
        free(il->matrix);
        free(il);
        return NULL;
    }

    row_assigned_bytes = sizeof(int) * (size_t)depth;
    il->row_assigned = (int *)malloc(row_assigned_bytes);
    if (il->row_assigned == NULL) {
        LOG_ERROR("[IL] create: malloc(row_assigned) failed");
        free(il->row_block_key);
        free(il->slots);
        free(il->matrix);
        free(il);
        return NULL;
    }

    reset_window(il);

    LOG_INFO("[IL] Created: depth=%d n=%d symbol_size=%d flush_timeout_ms=%d "
             "matrix_bytes=%zu",
             depth,
             k_plus_m,
             symbol_size,
             flush_timeout_ms,
             matrix_bytes);

    return il;
}

void interleaver_destroy(interleaver_t *il)
{
    if (il == NULL) {
        return;
    }

    free(il->row_assigned);
    free(il->row_block_key);
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
        LOG_WARN("[IL] push: matrix is draining — packet_id=%u fec_id=%u rejected",
                 (unsigned)sym->packet_id,
                 (unsigned)sym->fec_id);
        return -1;
    }

    col = (int)sym->fec_id;
    if (col < 0 || col >= il->n) {
        LOG_ERROR("[IL] push: fec_id=%u out of range [0..%d] (packet_id=%u)",
                  (unsigned)sym->fec_id,
                  il->n - 1,
                  (unsigned)sym->packet_id);
        return -1;
    }

    row = resolve_row(il, sym->packet_id);
    if (row < 0 || row >= il->depth) {
        LOG_ERROR("[IL] push: failed to resolve row for block_key=%u fec_id=%u",
                  (unsigned)sym->packet_id,
                  (unsigned)sym->fec_id);
        return -1;
    }

    slot = &il->slots[row];

    if (!il->window_has_data) {
        interleaver_get_now(&il->window_start);
        il->window_has_data = 1;
    }

    if (bitset_test(slot->received_mask, col)) {
        LOG_DEBUG("[IL] push: duplicate packet_id=%u fec_id=%u (row=%d col=%d) — dropped",
                  (unsigned)sym->packet_id,
                  (unsigned)sym->fec_id,
                  row,
                  col);
        return 0;
    }

    dst = cell(il, row, col);
    dst->packet_id     = sym->packet_id;
    dst->fec_id        = sym->fec_id;
    dst->symbol_index  = sym->symbol_index;
    dst->total_symbols = sym->total_symbols;
    dst->payload_len   = sym->payload_len;
    dst->crc32         = sym->crc32;
    memcpy(dst->data, sym->data, (size_t)il->symbol_size);

    bitset_set(slot->received_mask, col);
    slot->symbol_count++;
    slot->occupied = 1;

    LOG_DEBUG("[IL] push: packet_id=%u fec_id=%u -> row=%d col=%d "
              "slot_count=%d/%d complete_slots=%d/%d",
              (unsigned)sym->packet_id,
              (unsigned)sym->fec_id,
              row,
              col,
              slot->symbol_count,
              il->n,
              il->complete_slots,
              il->depth);

    if (slot->symbol_count == il->n) {
        il->complete_slots++;

        LOG_DEBUG("[IL] Row %d complete — complete_slots=%d/%d",
                  row,
                  il->complete_slots,
                  il->depth);

        if (il->complete_slots == il->depth) {
            il->ready = 1;
            il->pop_col = 0;
            il->pop_row = 0;

            LOG_INFO("[IL] Window ready: depth=%d × n=%d = %d symbols",
                     il->depth,
                     il->n,
                     il->depth * il->n);

            return 1;
        }
    }

    return 0;
}

int interleaver_tick(interleaver_t *il)
{
    struct timespec now;
    double          elapsed_ms;
    int             pre_complete_rows;
    int             padded_rows;
    int             row;

    if (il == NULL) {
        LOG_ERROR("[IL] tick: il is NULL");
        return -1;
    }

    if (il->flush_timeout_ms == 0) {
        return 0;
    }

    if (il->ready || !il->window_has_data || il->complete_slots < 1) {
        return 0;
    }

    interleaver_get_now(&now);
    elapsed_ms = interleaver_elapsed_ms(&il->window_start, &now);
    if (elapsed_ms < (double)il->flush_timeout_ms) {
        return 0;
    }

    pre_complete_rows = il->complete_slots;
    padded_rows = 0;

    for (row = 0; row < il->depth; ++row) {
        if (il->slots[row].symbol_count < il->n) {
            force_fill_row(il, row);
            padded_rows++;
        }
    }

    il->complete_slots = il->depth;
    il->ready = 1;
    il->pop_col = 0;
    il->pop_row = 0;

    LOG_INFO("[IL] flush timeout: forced ready with %d complete rows (%d padded)",
             pre_complete_rows,
             padded_rows);

    return 1;
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

    src = cell_const(il, il->pop_row, il->pop_col);
    out_sym->packet_id     = src->packet_id;
    out_sym->fec_id        = src->fec_id;
    out_sym->symbol_index  = src->symbol_index;
    out_sym->total_symbols = src->total_symbols;
    out_sym->payload_len   = src->payload_len;
    out_sym->crc32         = src->crc32;
    memcpy(out_sym->data, src->data, (size_t)il->symbol_size);

    LOG_DEBUG("[IL] pop: row=%d col=%d -> packet_id=%u fec_id=%u",
              il->pop_row,
              il->pop_col,
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
