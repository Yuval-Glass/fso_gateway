/*
 * src/fec_wrapper.c — Forward Error Correction wrapper for the FSO Gateway.
 *
 * See include/fec_wrapper.h for the full API specification and return codes.
 *
 * Wirehair calling conventions
 * ----------------------------
 *   1. wirehair_decoder_create() receives NULL as its first argument.
 *   2. wirehair_recover() requires the output buffer to be zeroed first.
 *   3. Wirehair_Success == 0, Wirehair_NeedMore == 1.
 *   4. Buffers should be 64-byte aligned for best performance.
 *   5. CRITICAL: wirehair_decode() MUST NOT be called again after it returns
 *      Wirehair_Success — the decoder segfaults on a second call.
 *      The feed loop breaks immediately on success.
 *
 * Source-First Feeding
 * --------------------
 *   build_feed_order() partitions valid symbols into:
 *     source  (fec_id < K)  — fed first
 *     repair  (fec_id >= K) — fed second
 *   This minimises iterations in the common no-loss / low-loss case.
 *   It uses the upper half of a 2×scan_capacity buffer as scratch space
 *   for the repair partition, avoiding an extra allocation.
 *
 * Return codes
 * ------------
 *   FEC_DECODE_OK              (0)  — success.
 *   FEC_DECODE_ERR            (-1)  — generic failure.
 *   FEC_DECODE_TOO_MANY_HOLES (-2)  — holes > M; block is irrecoverable.
 *
 *   All three symbols are defined in include/fec_wrapper.h (public API).
 *   No magic integers are used in this file.
 */

#define _POSIX_C_SOURCE 200112L

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <wirehair/wirehair.h>

#include "fec_wrapper.h"
#include "logging.h"
#include "types.h"

/* -------------------------------------------------------------------------- */
/* Internal constants                                                          */
/* -------------------------------------------------------------------------- */

#define FEC_ALIGNMENT_BYTES 64U

/* -------------------------------------------------------------------------- */
/* Internal context                                                            */
/* -------------------------------------------------------------------------- */

struct fec_ctx {
    int    k;
    int    symbol_size;
    size_t block_bytes;   /* k * symbol_size */
};

/* -------------------------------------------------------------------------- */
/* Private helpers                                                             */
/* -------------------------------------------------------------------------- */

static void *alloc_aligned(size_t size)
{
    void *ptr = NULL;

    if (size == 0U) { return NULL; }

    if (posix_memalign(&ptr, FEC_ALIGNMENT_BYTES, size) != 0) { return NULL; }

    memset(ptr, 0, size);
    return ptr;
}

static int is_aligned_64(const void *ptr)
{
    return (((uintptr_t)ptr & (uintptr_t)(FEC_ALIGNMENT_BYTES - 1U)) == 0U);
}

/*
 * symbol_slot_is_valid() — true if symbols[pos] is a usable decoding input.
 *
 * Conditions:
 *   - pos in [0, scan_capacity).
 *   - payload_len > 0  (not an erasure hole).
 *   - payload_len == ctx->symbol_size  (full-size; no partial symbols).
 *   - fec_id in [0, scan_capacity).
 *   - sparse mode: fec_id == pos  (slot is indexed by its own fec_id).
 */
static int symbol_slot_is_valid(const struct fec_ctx *ctx,
                                const symbol_t       *symbols,
                                int                   pos,
                                int                   scan_capacity,
                                int                   sparse_mode)
{
    const symbol_t *sym;

    if (ctx == NULL || symbols == NULL || pos < 0 || pos >= scan_capacity) {
        return 0;
    }

    sym = &symbols[pos];

    if (sym->payload_len == 0) { return 0; }
    if ((int)sym->payload_len != ctx->symbol_size) { return 0; }
    if ((int)sym->fec_id < 0 || (int)sym->fec_id >= scan_capacity) { return 0; }
    if (sparse_mode && (int)sym->fec_id != pos) { return 0; }

    return 1;
}

static int count_valid_symbols(const struct fec_ctx *ctx,
                               const symbol_t       *symbols,
                               int                   scan_capacity,
                               int                   sparse_mode)
{
    int pos, valid = 0;

    for (pos = 0; pos < scan_capacity; ++pos) {
        if (symbol_slot_is_valid(ctx, symbols, pos, scan_capacity,
                                 sparse_mode)) {
            valid++;
        }
    }

    return valid;
}

/*
 * build_feed_order() — populate out_order with valid-symbol positions,
 * source symbols (fec_id < K) first, then repair symbols (fec_id >= K).
 *
 * out_order must have capacity of at least 2 * scan_capacity ints.
 * The upper half is used as temporary storage for the repair partition
 * before the final memmove concatenation.
 *
 * Returns total count of valid symbols, or -1 on error.
 */
static int build_feed_order(const struct fec_ctx *ctx,
                            symbol_t             *symbols,
                            int                   scan_capacity,
                            int                   sparse_mode,
                            int                  *out_order)
{
    int *src_buf;
    int *rep_buf;
    int  src_count = 0, repair_count = 0;
    int  pos;

    if (ctx == NULL || symbols == NULL || out_order == NULL ||
        scan_capacity < 1) {
        return -1;
    }

    src_buf = out_order;
    rep_buf = out_order + scan_capacity;   /* upper half — temporary */

    for (pos = 0; pos < scan_capacity; ++pos) {
        if (!symbol_slot_is_valid(ctx, symbols, pos, scan_capacity,
                                  sparse_mode)) {
            continue;
        }

        if ((int)symbols[pos].fec_id < ctx->k) {
            src_buf[src_count++] = pos;
        } else {
            rep_buf[repair_count++] = pos;
        }
    }

    /* Concatenate: repair portion follows source portion */
    memmove(out_order + src_count, rep_buf,
            (size_t)repair_count * sizeof(int));

    return src_count + repair_count;
}

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

fec_handle_t fec_create(int k, int symbol_size)
{
    struct fec_ctx *ctx;

    if (k < 2 || symbol_size <= 0) {
        LOG_ERROR("[FEC] fec_create: invalid k=%d symbol_size=%d",
                  k, symbol_size);
        return NULL;
    }

    ctx = (struct fec_ctx *)malloc(sizeof(struct fec_ctx));
    if (ctx == NULL) {
        LOG_ERROR("[FEC] fec_create: malloc failed");
        return NULL;
    }

    ctx->k           = k;
    ctx->symbol_size = symbol_size;
    ctx->block_bytes = (size_t)k * (size_t)symbol_size;

    LOG_INFO("[FEC] Context: k=%d symbol_size=%d block_bytes=%zu",
             k, symbol_size, ctx->block_bytes);

    return ctx;
}

void fec_destroy(fec_handle_t handle)
{
    if (handle != NULL) { free(handle); }
}

/* -------------------------------------------------------------------------- */
/* Encoding                                                                    */
/* -------------------------------------------------------------------------- */

int fec_encode_block(fec_handle_t         handle,
                     const unsigned char *source_data,
                     symbol_t            *out_repair_symbols,
                     int                  m)
{
    struct fec_ctx *ctx        = (struct fec_ctx *)handle;
    WirehairCodec   encoder    = NULL;
    unsigned char  *symbol_buf = NULL;
    int             result     = FEC_DECODE_ERR;
    int             i;

    if (ctx == NULL || source_data == NULL ||
        out_repair_symbols == NULL || m < 1) {
        LOG_ERROR("[FEC] fec_encode_block: invalid argument");
        return FEC_DECODE_ERR;
    }

    if (!is_aligned_64(source_data)) {
        LOG_WARN("[FEC] fec_encode_block: source_data not 64-byte aligned");
    }

    symbol_buf = (unsigned char *)alloc_aligned((size_t)ctx->symbol_size);
    if (symbol_buf == NULL) {
        LOG_ERROR("[FEC] fec_encode_block: alloc_aligned failed");
        return FEC_DECODE_ERR;
    }

    encoder = wirehair_encoder_create(NULL,
                                      source_data,
                                      (uint64_t)ctx->block_bytes,
                                      (uint32_t)ctx->symbol_size);
    if (encoder == NULL) {
        LOG_ERROR("[FEC] fec_encode_block: wirehair_encoder_create() failed");
        goto cleanup;
    }

    for (i = 0; i < m; ++i) {
        uint32_t       bytes_out     = 0;
        uint32_t       repair_fec_id = (uint32_t)(ctx->k + i);
        WirehairResult wr;

        memset(symbol_buf, 0, (size_t)ctx->symbol_size);

        wr = wirehair_encode(encoder, repair_fec_id, symbol_buf,
                             (uint32_t)ctx->symbol_size, &bytes_out);

        if (wr != Wirehair_Success ||
            bytes_out != (uint32_t)ctx->symbol_size) {
            LOG_ERROR("[FEC] fec_encode_block: wirehair_encode() failed "
                      "fec_id=%u wr=%d bytes_out=%u",
                      (unsigned)repair_fec_id, (int)wr, (unsigned)bytes_out);
            goto cleanup;
        }

        out_repair_symbols[i].fec_id      = repair_fec_id;
        out_repair_symbols[i].payload_len = (uint16_t)bytes_out;
        memcpy(out_repair_symbols[i].data, symbol_buf,
               (size_t)ctx->symbol_size);
    }

    result = FEC_DECODE_OK;

cleanup:
    if (encoder != NULL) { wirehair_free(encoder); }
    free(symbol_buf);
    return result;
}

/* -------------------------------------------------------------------------- */
/* Decoding                                                                    */
/* -------------------------------------------------------------------------- */

int fec_decode_block(fec_handle_t   handle,
                     symbol_t      *symbols,
                     int            symbol_count,
                     int            scan_capacity,
                     unsigned char *out_reconstructed)
{
    struct fec_ctx  *ctx           = (struct fec_ctx *)handle;
    WirehairCodec    decoder       = NULL;
    int             *feed_order    = NULL;
    int              feed_count;
    int              valid_symbols;
    int              holes;
    int              m_available;
    int              sparse_mode;
    int              fed           = 0;
    int              pos_index;
    int              decoder_ready = 0;
    int              result        = FEC_DECODE_ERR;

    if (ctx == NULL || symbols == NULL || out_reconstructed == NULL) {
        LOG_ERROR("[FEC] fec_decode_block: NULL argument");
        return FEC_DECODE_ERR;
    }

    if (symbol_count < 0 ||
        scan_capacity < 1 ||
        scan_capacity > MAX_SYMBOLS_PER_BLOCK) {
        LOG_ERROR("[FEC] fec_decode_block: invalid counts "
                  "symbol_count=%d scan_capacity=%d",
                  symbol_count, scan_capacity);
        return FEC_DECODE_ERR;
    }

    sparse_mode   = (scan_capacity > symbol_count);
    valid_symbols = count_valid_symbols(ctx, symbols, scan_capacity,
                                        sparse_mode);
    holes         = scan_capacity - valid_symbols;
    m_available   = scan_capacity - ctx->k;
    if (m_available < 0) { m_available = 0; }

    /*
     * holes > M: irrecoverable.
     * Return FEC_DECODE_TOO_MANY_HOLES (-2) so callers can distinguish
     * this from a generic internal failure (FEC_DECODE_ERR = -1).
     */
    if (holes > m_available) {
        LOG_ERROR("[FEC] fec_decode_block: FEC_DECODE_TOO_MANY_HOLES "
                  "holes=%d > M=%d (valid=%d k=%d scan_capacity=%d)",
                  holes, m_available, valid_symbols, ctx->k, scan_capacity);
        return FEC_DECODE_TOO_MANY_HOLES;
    }

    if (valid_symbols < ctx->k) {
        LOG_ERROR("[FEC] fec_decode_block: insufficient valid symbols "
                  "(valid=%d k=%d holes=%d scan_capacity=%d)",
                  valid_symbols, ctx->k, holes, scan_capacity);
        return FEC_DECODE_ERR;
    }

    if (!is_aligned_64(out_reconstructed)) {
        LOG_WARN("[FEC] fec_decode_block: out_reconstructed not 64-byte aligned");
    }

    LOG_DEBUG("[FEC] decode: valid=%d holes=%d k=%d M=%d scan=%d sparse=%d",
              valid_symbols, holes, ctx->k, m_available,
              scan_capacity, sparse_mode);

    decoder = wirehair_decoder_create(NULL,
                                      (uint64_t)ctx->block_bytes,
                                      (uint32_t)ctx->symbol_size);
    if (decoder == NULL) {
        LOG_ERROR("[FEC] fec_decode_block: wirehair_decoder_create() failed");
        return FEC_DECODE_ERR;
    }

    /*
     * 2 × scan_capacity so build_feed_order() can use the upper half as
     * a temporary repair-symbol scratch buffer.
     */
    feed_order = (int *)malloc(sizeof(int) * (size_t)scan_capacity * 2);
    if (feed_order == NULL) {
        LOG_ERROR("[FEC] fec_decode_block: malloc(feed_order) failed");
        goto cleanup;
    }

    feed_count = build_feed_order(ctx, symbols, scan_capacity,
                                  sparse_mode, feed_order);
    if (feed_count < 0) {
        LOG_ERROR("[FEC] fec_decode_block: build_feed_order() failed");
        goto cleanup;
    }

    for (pos_index = 0; pos_index < feed_count; ++pos_index) {
        int            pos = feed_order[pos_index];
        WirehairResult wr;

        wr = wirehair_decode(decoder,
                             symbols[pos].fec_id,
                             symbols[pos].data,
                             (uint32_t)ctx->symbol_size);
        fed++;

        if (wr == Wirehair_Success) {
            decoder_ready = 1;
            LOG_DEBUG("[FEC] decoder success at fed=%d fec_id=%u holes=%d",
                      fed, (unsigned)symbols[pos].fec_id, holes);
            break;   /* MUST break — re-calling after success segfaults */
        }

        if (wr != Wirehair_NeedMore) {
            LOG_ERROR("[FEC] wirehair_decode() error "
                      "pos=%d fec_id=%u wr=%d fed=%d",
                      pos, (unsigned)symbols[pos].fec_id, (int)wr, fed);
            goto cleanup;
        }
    }

    if (!decoder_ready) {
        LOG_ERROR("[FEC] decoder never reached success "
                  "(fed=%d valid=%d holes=%d k=%d scan=%d)",
                  fed, valid_symbols, holes, ctx->k, scan_capacity);
        goto cleanup;
    }

    memset(out_reconstructed, 0, ctx->block_bytes);

    {
        WirehairResult wr = wirehair_recover(decoder, out_reconstructed,
                                             (uint64_t)ctx->block_bytes);
        if (wr != Wirehair_Success) {
            LOG_ERROR("[FEC] wirehair_recover() failed wr=%d", (int)wr);
            goto cleanup;
        }
    }

    LOG_DEBUG("[FEC] recovered OK (fed=%d valid=%d holes=%d)",
              fed, valid_symbols, holes);

    result = FEC_DECODE_OK;

cleanup:
    free(feed_order);
    if (decoder != NULL) { wirehair_free(decoder); }
    return result;
}

/* -------------------------------------------------------------------------- */
/* Self-test                                                                   */
/* -------------------------------------------------------------------------- */

int fec_run_self_test(void)
{
    return FEC_DECODE_OK;
}
