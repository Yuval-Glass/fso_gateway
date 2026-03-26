/*
 * src/deinterleaver.c — Block Lifecycle FSM implementation.
 *
 * See include/deinterleaver.h for the full state machine specification,
 * legal transitions, invariants, and eviction policy.
 *
 * Implementation notes
 * --------------------
 *
 * Slot array
 *   self->slots[0..max_active_blocks-1] — contiguous, linear-scan O(D).
 *   D is small (4..256 in practice) so scan cost is negligible.
 *
 * Duplicate detection
 *   received_mask — 33-byte bitset; bit (fec_id%8) of byte (fec_id/8).
 *   O(1) set and test.  Never written after slot leaves BLOCK_FILLING.
 *
 * Timing
 *   CLOCK_MONOTONIC throughout.  now_monotonic() is called at most once per
 *   push_symbol() entry and once per slot in tick().
 *
 * maybe_freeze_slot()
 *   The single authoritative function for FILLING exit transitions.
 *   Three exit conditions (see header):
 *     (a) Full completion          → READY_TO_DECODE.
 *     (b) Stabilization quiet period, holes <= M  → READY_TO_DECODE.
 *         Stabilization, holes > M  → recycle to EMPTY immediately.
 *     (c) Hard timeout, valid >= K AND holes <= M → READY_TO_DECODE.
 *         Hard timeout, otherwise                 → recycle to EMPTY.
 *   In both failure paths the slot is flagged via the return value so the
 *   caller (push_symbol or tick) can call slot_reset() directly.
 *
 * alloc_slot() eviction priority (matches header exactly):
 *   1. BLOCK_EMPTY slot.
 *   2. Oldest BLOCK_READY_TO_DECODE slot — recycled silently.
 *   3. Oldest BLOCK_FILLING slot         — data loss; LOG_WARN issued.
 *   4. No candidate found                → symbol dropped, return NULL.
 *
 * get_ready_block()
 *   Pure read.  State is NOT changed.  Slot stays READY_TO_DECODE.
 *   Idempotent: calling multiple times returns the same block.
 *
 * mark_result()
 *   READY_TO_DECODE → EMPTY directly, in a single call.
 *   No intermediate state is written.  The success flag is informational
 *   only (logged) and does not produce a distinct slot state.
 *
 * tick() irrecoverable-block recycle
 *   Blocks that cannot be recovered (insufficient symbols or holes > M at
 *   timeout) are recycled to EMPTY immediately inside tick().  They are
 *   never READY and cannot be retrieved via get_ready_block().
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

#define MASK_BYTES  (MAX_SYMBOLS_PER_BLOCK / 8 + 1)   /* 33 bytes            */

/*
 * Return values from maybe_freeze_slot() to tell the caller what happened.
 */
#define FREEZE_NONE   0   /* no transition; slot still FILLING               */
#define FREEZE_READY  1   /* slot moved to READY_TO_DECODE                   */
#define FREEZE_RECYCLE 2  /* irrecoverable; caller must call slot_reset()    */

/* -------------------------------------------------------------------------- */
/* Per-slot structure                                                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    block_state_t   state;           /* EMPTY | FILLING | READY_TO_DECODE     */
    uint32_t        block_id;
    int             valid_symbols;   /* count of distinct stored fec_ids      */

    struct timespec first_sym_time;  /* CLOCK_MONOTONIC at first symbol       */
    struct timespec last_sym_time;   /* updated on each accepted symbol       */

    uint8_t         received_mask[MASK_BYTES]; /* O(1) dup detect bitset      */

    block_t         block;           /* sparse symbol storage (by fec_id)     */
} slot_t;

/* -------------------------------------------------------------------------- */
/* Deinterleaver context                                                       */
/* -------------------------------------------------------------------------- */

struct deinterleaver {
    int     max_active_blocks;
    int     symbols_per_block;   /* N                                         */
    int     k;                   /* source symbols — explicit, never guessed  */
    int     m;                   /* repair symbols = N - K                    */
    size_t  symbol_size;

    double  stabilization_ms;
    double  block_max_age_ms;

    slot_t *slots;

    dil_stats_t stats;

    deinterleaver_block_final_cb_t final_cb;
    void                          *final_cb_user;

    deinterleaver_eviction_cb_t    eviction_cb;
    void                          *eviction_cb_user;
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
/* Clock helpers                                                               */
/* -------------------------------------------------------------------------- */

static void slot_reset(slot_t *s);

static void emit_block_final_reason(deinterleaver_t *self,
                                   uint32_t          block_id,
                                   deinterleaver_block_final_reason_t reason)
{
    if (self == NULL || reason == DIL_BLOCK_FINAL_NONE) {
        return;
    }

    if (self->final_cb != NULL) {
        self->final_cb(block_id, reason, self->final_cb_user);
    }
}

static void finalize_and_reset_slot(deinterleaver_t                      *self,
                                    slot_t                               *s,
                                    deinterleaver_block_final_reason_t    reason)
{
    uint32_t block_id;

    if (self == NULL || s == NULL || s->state == BLOCK_EMPTY) {
        return;
    }

    block_id = s->block_id;
    emit_block_final_reason(self, block_id, reason);
    slot_reset(s);
}


static void now_monotonic(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static double elapsed_ms(const struct timespec *start,
                          const struct timespec *end)
{
    double s  = (double)(end->tv_sec  - start->tv_sec);
    double ns = (double)(end->tv_nsec - start->tv_nsec);
    double ms = s * 1000.0 + ns / 1.0e6;
    return (ms < 0.0) ? 0.0 : ms;
}

/* -------------------------------------------------------------------------- */
/* Slot helpers                                                                */
/* -------------------------------------------------------------------------- */

static void slot_reset(slot_t *s)
{
    memset(s, 0, sizeof(slot_t));
    /* state == 0 == BLOCK_EMPTY after memset */
}

static void init_slot(slot_t               *s,
                      uint32_t              block_id,
                      int                   n,
                      int                   k,
                      const struct timespec *now)
{
    s->state                   = BLOCK_FILLING;
    s->block_id                = block_id;
    s->valid_symbols           = 0;
    s->first_sym_time          = *now;
    s->last_sym_time           = *now;

    s->block.block_id          = (uint64_t)block_id;
    s->block.symbol_count      = 0;
    s->block.symbols_per_block = n;
    s->block.k_limit           = k;
}

/* find_slot_any — locate a non-EMPTY slot by block_id */
static slot_t *find_slot_any(deinterleaver_t *self, uint32_t block_id)
{
    int i;

    for (i = 0; i < self->max_active_blocks; ++i) {
        slot_t *s = &self->slots[i];
        if (s->state != BLOCK_EMPTY && s->block_id == block_id) {
            return s;
        }
    }

    return NULL;
}

/*
 * alloc_slot() — obtain a slot for a new block_id.
 *
 * Eviction priority (matches header exactly):
 *   1. BLOCK_EMPTY slot              — free; no eviction cost.
 *   2. Oldest BLOCK_READY_TO_DECODE  — recycled silently.
 *   3. Oldest BLOCK_FILLING          — data loss; LOG_WARN issued.
 *   4. No candidate                  — symbol dropped, return NULL.
 */
static slot_t *alloc_slot(deinterleaver_t       *self,
                           uint32_t               block_id,
                           const struct timespec *now)
{
    int     i;
    slot_t *free_slot        = NULL;
    slot_t *oldest_ready     = NULL;
    double  oldest_ready_age = -1.0;
    slot_t *oldest_filling   = NULL;
    double  oldest_fill_age  = -1.0;

    for (i = 0; i < self->max_active_blocks; ++i) {
        slot_t *s = &self->slots[i];
        double  age;

        switch (s->state) {
            case BLOCK_EMPTY:
                if (free_slot == NULL) { free_slot = s; }
                break;

            case BLOCK_READY_TO_DECODE:
                age = elapsed_ms(&s->first_sym_time, now);
                if (age > oldest_ready_age) {
                    oldest_ready_age = age;
                    oldest_ready     = s;
                }
                break;

            case BLOCK_FILLING:
                age = elapsed_ms(&s->first_sym_time, now);
                if (age > oldest_fill_age) {
                    oldest_fill_age = age;
                    oldest_filling  = s;
                }
                break;
        }
    }

    /* Priority 1: free slot */
    if (free_slot != NULL) {
        slot_reset(free_slot);
        init_slot(free_slot, block_id, self->symbols_per_block, self->k, now);
        return free_slot;
    }

    /* Priority 2: oldest READY_TO_DECODE slot */
    if (oldest_ready != NULL) {
        LOG_DEBUG("[DIL] Evicting READY_TO_DECODE block_id=%u (age=%.1f ms) "
                  "for new block_id=%u",
                  (unsigned)oldest_ready->block_id,
                  oldest_ready_age,
                  (unsigned)block_id);
        self->stats.evicted_done_blocks++;

        if (self->eviction_cb != NULL) {
            dil_eviction_info_t ei;
            int                 si;
            int                 ac = 0;

            memset(&ei, 0, sizeof(ei));
            ei.incoming_block_id = (uint64_t)block_id;
            ei.slot_index        = (uint32_t)(oldest_ready - self->slots);
            ei.valid_symbols     = (uint32_t)oldest_ready->valid_symbols;
            ei.holes             = (uint32_t)(self->symbols_per_block -
                                              oldest_ready->valid_symbols);
            ei.expected_symbols  = (uint32_t)self->symbols_per_block;

            for (si = 0; si < self->max_active_blocks; ++si) {
                if (self->slots[si].state != BLOCK_EMPTY) {
                    ac++;
                }
            }
            ei.active_blocks     = (uint32_t)ac;
            ei.max_active_blocks = (uint32_t)self->max_active_blocks;

            ei.snapshot_count = 0;
            for (si = 0; si < self->max_active_blocks &&
                         ei.snapshot_count < DIL_EVICTION_SNAPSHOT_MAX; ++si) {
                slot_t *ss = &self->slots[si];
                uint32_t idx = ei.snapshot_count;
                ei.snapshot_indices[idx]       = (uint32_t)si;
                ei.snapshot_states[idx]        =
                    (ss->state == BLOCK_EMPTY)           ? 'E' :
                    (ss->state == BLOCK_FILLING)         ? 'F' : 'R';
                ei.snapshot_block_ids[idx]     = (uint64_t)ss->block_id;
                ei.snapshot_valid_symbols[idx] = (uint32_t)ss->valid_symbols;
                ei.snapshot_count++;
            }

            self->eviction_cb(oldest_ready->block_id,
                              DIL_BLOCK_FINAL_DISCARDED_READY_EVICTED_BEFORE_MARK,
                              &ei,
                              self->eviction_cb_user);
        }

        finalize_and_reset_slot(self,
                                oldest_ready,
                                DIL_BLOCK_FINAL_DISCARDED_READY_EVICTED_BEFORE_MARK);
        init_slot(oldest_ready, block_id, self->symbols_per_block,
                  self->k, now);
        return oldest_ready;
    }

    /* Priority 3: oldest FILLING slot — data loss */
    if (oldest_filling != NULL) {
        LOG_WARN("[DIL] Evicting FILLING block_id=%u (age=%.1f ms, "
                 "%d/%d syms) for block_id=%u",
                 (unsigned)oldest_filling->block_id,
                 oldest_fill_age,
                 oldest_filling->valid_symbols,
                 self->symbols_per_block,
                 (unsigned)block_id);
        self->stats.evicted_filling_blocks++;

        if (self->eviction_cb != NULL) {
            dil_eviction_info_t ei;
            int                 si;
            int                 ac = 0;

            memset(&ei, 0, sizeof(ei));
            ei.incoming_block_id = (uint64_t)block_id;
            ei.slot_index        = (uint32_t)(oldest_filling - self->slots);
            ei.valid_symbols     = (uint32_t)oldest_filling->valid_symbols;
            ei.holes             = (uint32_t)(self->symbols_per_block -
                                              oldest_filling->valid_symbols);
            ei.expected_symbols  = (uint32_t)self->symbols_per_block;

            for (si = 0; si < self->max_active_blocks; ++si) {
                if (self->slots[si].state != BLOCK_EMPTY) {
                    ac++;
                }
            }
            ei.active_blocks     = (uint32_t)ac;
            ei.max_active_blocks = (uint32_t)self->max_active_blocks;

            ei.snapshot_count = 0;
            for (si = 0; si < self->max_active_blocks &&
                         ei.snapshot_count < DIL_EVICTION_SNAPSHOT_MAX; ++si) {
                slot_t *ss = &self->slots[si];
                uint32_t idx = ei.snapshot_count;
                ei.snapshot_indices[idx]       = (uint32_t)si;
                ei.snapshot_states[idx]        =
                    (ss->state == BLOCK_EMPTY)           ? 'E' :
                    (ss->state == BLOCK_FILLING)         ? 'F' : 'R';
                ei.snapshot_block_ids[idx]     = (uint64_t)ss->block_id;
                ei.snapshot_valid_symbols[idx] = (uint32_t)ss->valid_symbols;
                ei.snapshot_count++;
            }

            self->eviction_cb(oldest_filling->block_id,
                              DIL_BLOCK_FINAL_DISCARDED_EVICTED_BEFORE_DECODE,
                              &ei,
                              self->eviction_cb_user);
        }

        finalize_and_reset_slot(self,
                                oldest_filling,
                                DIL_BLOCK_FINAL_DISCARDED_EVICTED_BEFORE_DECODE);
        init_slot(oldest_filling, block_id, self->symbols_per_block,
                  self->k, now);
        return oldest_filling;
    }

    /* Priority 4: no candidate — all slots are in an unexpected state */
    LOG_WARN("[DIL] No evictable slot found — dropping symbol for block_id=%u",
             (unsigned)block_id);
    return NULL;
}

/*
 * maybe_freeze_slot() — evaluate FILLING exit transitions.
 *
 * Called after every successful symbol store AND from deinterleaver_tick().
 *
 * Checks (in order):
 *   (a) Full: valid_symbols == N → READY_TO_DECODE.
 *   (b) Stabilization / early-promotion (valid >= K):
 *         stab_ms == 0.0 — immediate promotion:
 *           holes <= M -> READY_TO_DECODE immediately (no quiet wait).
 *           holes >  M -> unrecoverable; return FREEZE_RECYCLE immediately.
 *         stab_ms > 0.0 — classic quiet-period gate:
 *           quiet >= stab_ms AND holes <= M -> READY_TO_DECODE.
 *           quiet >= stab_ms AND holes >  M -> irrecoverable; FREEZE_RECYCLE.
 *   (c) Hard timeout (age >= timeout_ms):
 *         valid >= K AND holes <= M -> READY_TO_DECODE.
 *         otherwise                 -> irrecoverable; return FREEZE_RECYCLE.
 *
 * The stab_ms == 0.0 immediate-promotion path prevents recoverable FILLING
 * blocks from being evicted before decode when the active block window fills.
 *
 * Returns:
 *   FREEZE_NONE    (0) — no transition; slot remains FILLING.
 *   FREEZE_READY   (1) — slot moved to READY_TO_DECODE.
 *   FREEZE_RECYCLE (2) — irrecoverable; caller must call slot_reset().
 */
static int maybe_freeze_slot(slot_t                *s,
                              int                    N,
                              int                    K,
                              int                    M,
                              double                 stab_ms,
                              double                 timeout_ms,
                              const struct timespec *now,
                              dil_stats_t           *stats)
{
    int    holes;
    double age_ms;
    double quiet_ms;

    if (s->state != BLOCK_FILLING) {
        return FREEZE_NONE;
    }

    holes = N - s->valid_symbols;

    /* ------------------------------------------------------------------ */
    /* (a) Full completion                                                 */
    /* ------------------------------------------------------------------ */
    if (s->valid_symbols == N) {
        s->state = BLOCK_READY_TO_DECODE;
        stats->blocks_ready++;
        LOG_INFO("[DIL] Block %u → READY_TO_DECODE (full: %d/%d symbols)",
                 (unsigned)s->block_id, s->valid_symbols, N);
        return FREEZE_READY;
    }

    age_ms = elapsed_ms(&s->first_sym_time, now);

    /* ------------------------------------------------------------------ */
    /* (b) Stabilization / early-promotion                                */
    /*                                                                    */
    /* stab_ms > 0.0  — classic quiet-period gate: promote only after    */
    /*                  no new symbol has arrived for stab_ms.            */
    /* stab_ms == 0.0 — immediate-promotion: as soon as valid >= K and   */
    /*                  holes <= M the block is decodable; do not hold it */
    /*                  in FILLING where it risks eviction before decode. */
    /* ------------------------------------------------------------------ */
    if (s->valid_symbols >= K) {
        if (stab_ms == 0.0) {
            /* Immediate-promotion path: no quiet period required. */
            if (holes > M) {
                /*
                 * Already unrecoverable — holes exceed the FEC budget.
                 * More symbols cannot arrive for fec_ids already lost,
                 * so the block is permanently undecodable.  Recycle now.
                 */
                stats->blocks_failed_holes++;
                LOG_WARN("[DIL] Block %u → recycle (unrecoverable: "
                         "holes=%d > M=%d, valid=%d)",
                         (unsigned)s->block_id, holes, M, s->valid_symbols);
                return FREEZE_RECYCLE;
            }

            s->state = BLOCK_READY_TO_DECODE;
            stats->blocks_ready++;
            LOG_INFO("[DIL] Block %u → READY_TO_DECODE "
                     "(early-promote: valid=%d/%d holes=%d)",
                     (unsigned)s->block_id,
                     s->valid_symbols, N, holes);
            return FREEZE_READY;
        }

        /* Classic stabilization quiet-period gate (stab_ms > 0.0). */
        quiet_ms = elapsed_ms(&s->last_sym_time, now);

        if (quiet_ms >= stab_ms) {
            if (holes > M) {
                stats->blocks_failed_holes++;
                LOG_WARN("[DIL] Block %u → recycle (stabilized but "
                         "holes=%d > M=%d, valid=%d)",
                         (unsigned)s->block_id, holes, M, s->valid_symbols);
                return FREEZE_RECYCLE;
            }

            s->state = BLOCK_READY_TO_DECODE;
            stats->blocks_ready++;
            LOG_INFO("[DIL] Block %u → READY_TO_DECODE "
                     "(stabilized: valid=%d/%d holes=%d quiet=%.1f ms)",
                     (unsigned)s->block_id,
                     s->valid_symbols, N, holes, quiet_ms);
            return FREEZE_READY;
        }
    }

    /* ------------------------------------------------------------------ */
    /* (c) Hard timeout                                                   */
    /* ------------------------------------------------------------------ */
    if (age_ms >= timeout_ms) {
        if (s->valid_symbols >= K && holes <= M) {
            s->state = BLOCK_READY_TO_DECODE;
            stats->blocks_ready++;
            LOG_INFO("[DIL] Block %u → READY_TO_DECODE "
                     "(timeout %.1f ms: valid=%d/%d holes=%d)",
                     (unsigned)s->block_id,
                     timeout_ms, s->valid_symbols, N, holes);
            return FREEZE_READY;
        }

        if (holes > M) {
            stats->blocks_failed_holes++;
        } else {
            stats->blocks_failed_timeout++;
        }
        LOG_WARN("[DIL] Block %u → recycle "
                 "(timeout %.1f ms: valid=%d/%d K=%d holes=%d M=%d)",
                 (unsigned)s->block_id,
                 timeout_ms, s->valid_symbols, N, K, holes, M);
        return FREEZE_RECYCLE;
    }

    return FREEZE_NONE;
}

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

deinterleaver_t *deinterleaver_create(int    max_active_blocks,
                                      int    symbols_per_block,
                                      int    k,
                                      size_t symbol_size,
                                      double stabilization_ms,
                                      double block_max_age_ms)
{
    deinterleaver_t *self;

    if (max_active_blocks < 1 ||
        symbols_per_block < 1 ||
        symbols_per_block > MAX_SYMBOLS_PER_BLOCK ||
        k < 1 || k >= symbols_per_block ||
        symbol_size == 0)
    {
        LOG_ERROR("[DIL] deinterleaver_create: invalid parameters "
                  "max_active=%d N=%d k=%d sym_size=%zu",
                  max_active_blocks, symbols_per_block, k, symbol_size);
        return NULL;
    }

    self = (deinterleaver_t *)malloc(sizeof(deinterleaver_t));
    if (self == NULL) {
        LOG_ERROR("[DIL] deinterleaver_create: malloc failed");
        return NULL;
    }

    memset(self, 0, sizeof(deinterleaver_t));

    self->max_active_blocks = max_active_blocks;
    self->symbols_per_block = symbols_per_block;
    self->k                 = k;
    self->m                 = symbols_per_block - k;
    self->symbol_size       = symbol_size;
    self->stabilization_ms  = stabilization_ms;
    self->block_max_age_ms  = block_max_age_ms;

    self->slots = (slot_t *)calloc((size_t)max_active_blocks, sizeof(slot_t));
    if (self->slots == NULL) {
        LOG_ERROR("[DIL] deinterleaver_create: calloc(slots) failed");
        free(self);
        return NULL;
    }

    LOG_INFO("[DIL] Created: max_active=%d N=%d K=%d M=%d sym_size=%zu "
             "stab_ms=%.1f max_age_ms=%.1f",
             max_active_blocks, symbols_per_block, k, self->m,
             symbol_size, stabilization_ms, block_max_age_ms);

    return self;
}

void deinterleaver_destroy(deinterleaver_t *self)
{
    if (self == NULL) { return; }
    free(self->slots);
    free(self);
}

int deinterleaver_set_block_final_callback(deinterleaver_t               *self,
                                           deinterleaver_block_final_cb_t cb,
                                           void                          *user)
{
    if (self == NULL) {
        return -1;
    }

    self->final_cb = cb;
    self->final_cb_user = user;
    return 0;
}

int deinterleaver_set_eviction_callback(deinterleaver_t             *self,
                                        deinterleaver_eviction_cb_t  cb,
                                        void                        *user)
{
    if (self == NULL) {
        return -1;
    }

    self->eviction_cb      = cb;
    self->eviction_cb_user = user;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Ingestion                                                                   */
/* -------------------------------------------------------------------------- */

int deinterleaver_push_symbol(deinterleaver_t *self, const symbol_t *sym)
{
    slot_t         *slot;
    int             fec_pos;
    int             freeze_rc;
    int             holes;
    struct timespec now;

    if (self == NULL || sym == NULL) {
        LOG_ERROR("[DIL] push_symbol: NULL argument");
        return -1;
    }

    /* 1. Erasure sentinel */
    if (sym->payload_len == 0) {
        self->stats.dropped_symbols_erasure++;
        LOG_DEBUG("[DIL] push_symbol: erasure sentinel block_id=%u fec_id=%u",
                  (unsigned)sym->packet_id, (unsigned)sym->fec_id);
        return 0;
    }

    /* 2. Payload size guard */
    if ((size_t)sym->payload_len > self->symbol_size) {
        LOG_ERROR("[DIL] push_symbol: payload_len=%u > symbol_size=%zu "
                  "block_id=%u fec_id=%u",
                  (unsigned)sym->payload_len, self->symbol_size,
                  (unsigned)sym->packet_id, (unsigned)sym->fec_id);
        return -1;
    }

    /* 3. fec_id range check */
    fec_pos = (int)sym->fec_id;
    if (fec_pos < 0 || fec_pos >= self->symbols_per_block) {
        LOG_ERROR("[DIL] push_symbol: fec_id=%u out of range [0..%d]",
                  (unsigned)sym->fec_id, self->symbols_per_block - 1);
        return -1;
    }

    /* 4. total_symbols consistency (non-fatal) */
    if (sym->total_symbols != 0 &&
        (int)sym->total_symbols != self->symbols_per_block)
    {
        LOG_WARN("[DIL] push_symbol: total_symbols=%u != N=%d "
                 "block_id=%u fec_id=%u",
                 (unsigned)sym->total_symbols, self->symbols_per_block,
                 (unsigned)sym->packet_id, (unsigned)sym->fec_id);
    }

    now_monotonic(&now);

    slot = find_slot_any(self, sym->packet_id);

    if (slot == NULL) {
        slot = alloc_slot(self, sym->packet_id, &now);
        if (slot == NULL) {
            return (deinterleaver_ready_count(self) > 0) ? 1 : -1;
        }
    } else {
        /* 5. Freeze Rule — only FILLING slots accept new symbols */
        if (slot->state != BLOCK_FILLING) {
            self->stats.dropped_symbols_frozen++;
            LOG_DEBUG("[DIL] push_symbol: block_id=%u state=%d (frozen) "
                      "— dropping fec_id=%u",
                      (unsigned)sym->packet_id, (int)slot->state,
                      (unsigned)sym->fec_id);
            return 0;
        }
    }

    /* 6. Duplicate detection (O(1) bitmap) */
    if (bitset_test(slot->received_mask, fec_pos)) {
        self->stats.dropped_symbols_duplicate++;
        LOG_DEBUG("[DIL] Duplicate block_id=%u fec_id=%u — dropped",
                  (unsigned)sym->packet_id, (unsigned)sym->fec_id);
        return 0;
    }

    /* Store symbol at its sparse fec_id position */
    slot->block.symbols[fec_pos] = *sym;
    slot->block.symbols[fec_pos].total_symbols =
        (uint16_t)self->symbols_per_block;

    bitset_set(slot->received_mask, fec_pos);
    slot->valid_symbols++;
    slot->block.symbol_count = slot->valid_symbols;
    slot->last_sym_time      = now;

    LOG_DEBUG("[DIL] block_id=%u fec_id=%u stored (%d/%d) payload_len=%u",
              (unsigned)sym->packet_id, (unsigned)sym->fec_id,
              slot->valid_symbols, self->symbols_per_block,
              (unsigned)sym->payload_len);

    /* Evaluate state transition */
    freeze_rc = maybe_freeze_slot(slot,
                                  self->symbols_per_block,
                                  self->k,
                                  self->m,
                                  self->stabilization_ms,
                                  self->block_max_age_ms > 0.0
                                      ? self->block_max_age_ms
                                      : 1.0e15,
                                  &now,
                                  &self->stats);

    if (freeze_rc == FREEZE_RECYCLE) {
        /*
         * Block is irrecoverable (holes > M or timeout with valid < K).
         * Recycle the slot directly to EMPTY — it was never READY so it
         * cannot be retrieved via get_ready_block().
         */
        holes = self->symbols_per_block - slot->valid_symbols;
        LOG_DEBUG("[DIL] Block %u irrecoverable at insertion — recycling",
                  (unsigned)slot->block_id);
        finalize_and_reset_slot(
            self,
            slot,
            (holes > self->m)
                ? DIL_BLOCK_FINAL_DISCARDED_TOO_MANY_HOLES_BEFORE_DECODE
                : DIL_BLOCK_FINAL_DISCARDED_TIMEOUT_BEFORE_DECODE);
        return 0;
    }

    return (freeze_rc == FREEZE_READY) ? 1 : 0;
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

        if (s->state == BLOCK_READY_TO_DECODE) {
            if (s->block.symbols_per_block == 0) {
                LOG_ERROR("[DIL] BUG: block_id=%u symbols_per_block=0 "
                          "— correcting",
                          (unsigned)s->block_id);
                s->block.symbols_per_block = self->symbols_per_block;
            }

            *out_block = s->block;

            /*
             * STATE IS NOT CHANGED HERE.
             * Slot stays READY_TO_DECODE until mark_result() is called.
             * This call is idempotent.
             */
            LOG_INFO("[DIL] get_ready_block: block_id=%u (%d/%d syms) "
                     "— state unchanged; mark_result() required",
                     (unsigned)s->block_id,
                     s->valid_symbols, self->symbols_per_block);
            return 0;
        }
    }

    return -1;
}

/* -------------------------------------------------------------------------- */
/* Explicit acknowledgment                                                     */
/* -------------------------------------------------------------------------- */

int deinterleaver_mark_result(deinterleaver_t *self,
                              uint32_t         block_id,
                              int              success)
{
    slot_t *s;

    if (self == NULL) {
        LOG_ERROR("[DIL] mark_result: NULL self");
        return -1;
    }

    s = find_slot_any(self, block_id);

    if (s == NULL || s->state != BLOCK_READY_TO_DECODE) {
        LOG_WARN("[DIL] mark_result: block_id=%u not found in "
                 "READY_TO_DECODE (state=%d)",
                 (unsigned)block_id,
                 s != NULL ? (int)s->state : -1);
        return -1;
    }

    /*
     * READY_TO_DECODE → EMPTY.
     *
     * The success flag is informational only — it is logged but does not
     * produce a distinct slot state.  The slot is always returned to EMPTY
     * regardless of outcome.
     */
    LOG_INFO("[DIL] mark_result: block_id=%u %s → EMPTY",
             (unsigned)block_id, success ? "(success)" : "(failure)");

    finalize_and_reset_slot(self,
                            s,
                            success
                                ? DIL_BLOCK_FINAL_DECODE_SUCCESS
                                : DIL_BLOCK_FINAL_DECODE_FAILED);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Timeout / stabilization tick                                               */
/* -------------------------------------------------------------------------- */

int deinterleaver_tick(deinterleaver_t *self, double override_timeout_ms)
{
    struct timespec now;
    int             transitioned = 0;
    int             i;
    double          hard_timeout;

    if (self == NULL) { return 0; }

    now_monotonic(&now);

    /*
     * Resolve effective hard timeout:
     *   override > 0.0  → use override.
     *   override == 0.0 → immediate flush (threshold 0; elapsed always fires).
     *   override < 0.0  → use stored block_max_age_ms; if 0 (disabled), use
     *                     a large sentinel so only stabilization can trigger.
     */
    if (override_timeout_ms > 0.0) {
        hard_timeout = override_timeout_ms;
    } else if (override_timeout_ms == 0.0) {
        hard_timeout = 0.0;
    } else {
        hard_timeout = (self->block_max_age_ms > 0.0)
                       ? self->block_max_age_ms
                       : 1.0e15;
    }

    for (i = 0; i < self->max_active_blocks; ++i) {
        slot_t *s = &self->slots[i];
        int     freeze_rc;

        if (s->state != BLOCK_FILLING) { continue; }

        freeze_rc = maybe_freeze_slot(s,
                                      self->symbols_per_block,
                                      self->k,
                                      self->m,
                                      self->stabilization_ms,
                                      hard_timeout,
                                      &now,
                                      &self->stats);

        if (freeze_rc != FREEZE_NONE) {
            transitioned++;
        }

        if (freeze_rc == FREEZE_RECYCLE) {
            int holes = self->symbols_per_block - s->valid_symbols;

            /*
             * Block is irrecoverable — it was never READY and cannot be
             * retrieved.  Recycle to EMPTY so the slot pool is not drained.
             */
            LOG_DEBUG("[DIL] tick: recycling irrecoverable block_id=%u",
                      (unsigned)s->block_id);
            finalize_and_reset_slot(
                self,
                s,
                (holes > self->m)
                    ? DIL_BLOCK_FINAL_DISCARDED_TOO_MANY_HOLES_BEFORE_DECODE
                    : DIL_BLOCK_FINAL_DISCARDED_TIMEOUT_BEFORE_DECODE);
        }
    }

    return transitioned;
}

/* -------------------------------------------------------------------------- */
/* Introspection                                                               */
/* -------------------------------------------------------------------------- */

int deinterleaver_active_blocks(const deinterleaver_t *self)
{
    int count = 0, i;
    if (self == NULL) { return 0; }
    for (i = 0; i < self->max_active_blocks; ++i) {
        if (self->slots[i].state != BLOCK_EMPTY) { count++; }
    }
    return count;
}

int deinterleaver_ready_count(const deinterleaver_t *self)
{
    int count = 0, i;
    if (self == NULL) { return 0; }
    for (i = 0; i < self->max_active_blocks; ++i) {
        if (self->slots[i].state == BLOCK_READY_TO_DECODE) { count++; }
    }
    return count;
}

int deinterleaver_get_stats(const deinterleaver_t *self, dil_stats_t *out)
{
    if (self == NULL || out == NULL) { return -1; }
    *out = self->stats;
    return 0;
}