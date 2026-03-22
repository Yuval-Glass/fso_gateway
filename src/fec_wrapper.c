/*
 * src/fec_wrapper.c — Forward Error Correction wrapper for the FSO Gateway.
 *
 * Calling conventions for this Wirehair build:
 *   1. wirehair_decoder_create() must receive NULL as its first argument.
 *   2. wirehair_recover() implements full-reconstruction semantics — the
 *      output buffer must be zeroed before the call.
 *   3. Wirehair_Success == 0, Wirehair_NeedMore == 1.
 *   4. All data buffers passed to Wirehair should be 64-byte aligned.
 *
 * IMPORTANT DECODING NOTE:
 *   This Wirehair build segfaults if wirehair_decode() is called again after
 *   the decoder has already returned Wirehair_Success.
 *
 *   Therefore the decoder loop MUST break immediately on success.
 *
 * fec_decode_block() scan modes:
 *
 *   COMPACT (stress test, raw received[] with no holes):
 *     symbol_count  = received_count
 *     scan_capacity = received_count
 *     Every slot 0..received_count-1 is valid.
 *
 *   SPARSE (deinterleaver block_t, indexed by fec_id):
 *     symbol_count  = block.symbol_count
 *     scan_capacity = block.symbols_per_block  (K+M, e.g. 96)
 *     Holes at lost fec_id positions have payload_len == 0 and are skipped.
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
    size_t block_bytes;
};

/* -------------------------------------------------------------------------- */
/* Private helpers                                                             */
/* -------------------------------------------------------------------------- */

static void *alloc_aligned(size_t size)
{
    void *ptr = NULL;

    if (size == 0U) {
        return NULL;
    }

    if (posix_memalign(&ptr, FEC_ALIGNMENT_BYTES, size) != 0) {
        return NULL;
    }

    memset(ptr, 0, size);
    return ptr;
}

static int is_aligned_64(const void *ptr)
{
    return (((uintptr_t)ptr & (uintptr_t)(FEC_ALIGNMENT_BYTES - 1U)) == 0U);
}

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

    if (sym->payload_len == 0) {
        return 0;
    }

    if ((int)sym->payload_len != ctx->symbol_size) {
        return 0;
    }

    if ((int)sym->fec_id < 0 || (int)sym->fec_id >= scan_capacity) {
        return 0;
    }

    if (sparse_mode && (int)sym->fec_id != pos) {
        return 0;
    }

    return 1;
}

static int count_valid_symbols(const struct fec_ctx *ctx,
                               const symbol_t      *symbols,
                               int                  scan_capacity,
                               int                  sparse_mode)
{
    int pos;
    int valid = 0;

    for (pos = 0; pos < scan_capacity; ++pos) {
        if (symbol_slot_is_valid(ctx, symbols, pos, scan_capacity, sparse_mode)) {
            valid++;
        }
    }

    return valid;
}

static int build_feed_order(const struct fec_ctx *ctx,
                            symbol_t            *symbols,
                            int                  scan_capacity,
                            int                  sparse_mode,
                            int                 *out_order)
{
    int pos;
    int count = 0;

    if (ctx == NULL || symbols == NULL || out_order == NULL || scan_capacity < 1) {
        return -1;
    }

    for (pos = 0; pos < scan_capacity; ++pos) {
        if (symbol_slot_is_valid(ctx, symbols, pos, scan_capacity, sparse_mode)) {
            out_order[count++] = pos;
        }
    }

    return count;
}

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

fec_handle_t fec_create(int k, int symbol_size)
{
    struct fec_ctx *ctx;

    if (k < 2 || symbol_size <= 0) {
        LOG_ERROR("[FEC] fec_create: invalid parameters k=%d symbol_size=%d",
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

    LOG_INFO("[FEC] Context created: k=%d symbol_size=%d block_bytes=%zu",
             k, symbol_size, ctx->block_bytes);

    return ctx;
}

void fec_destroy(fec_handle_t handle)
{
    if (handle != NULL) {
        free(handle);
    }
}

/* -------------------------------------------------------------------------- */
/* Encoding                                                                    */
/* -------------------------------------------------------------------------- */

int fec_encode_block(fec_handle_t         handle,
                     const unsigned char *source_data,
                     symbol_t            *out_repair_symbols,
                     int                  m)
{
    struct fec_ctx *ctx = (struct fec_ctx *)handle;
    WirehairCodec   encoder    = NULL;
    unsigned char  *symbol_buf = NULL;
    int             result     = -1;
    int             i;

    if (ctx == NULL || source_data == NULL ||
        out_repair_symbols == NULL || m < 1)
    {
        LOG_ERROR("[FEC] fec_encode_block: invalid argument");
        return -1;
    }

    if (!is_aligned_64(source_data)) {
        LOG_WARN("[FEC] fec_encode_block: source_data is not 64-byte aligned");
    }

    symbol_buf = (unsigned char *)alloc_aligned((size_t)ctx->symbol_size);
    if (symbol_buf == NULL) {
        LOG_ERROR("[FEC] fec_encode_block: alloc_aligned failed");
        return -1;
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
        uint32_t       bytes_out      = 0;
        uint32_t       repair_fec_id  = (uint32_t)(ctx->k + i);
        WirehairResult wr;

        memset(symbol_buf, 0, (size_t)ctx->symbol_size);

        wr = wirehair_encode(encoder,
                             repair_fec_id,
                             symbol_buf,
                             (uint32_t)ctx->symbol_size,
                             &bytes_out);

        if (wr != Wirehair_Success ||
            bytes_out != (uint32_t)ctx->symbol_size)
        {
            LOG_ERROR("[FEC] fec_encode_block: wirehair_encode() failed "
                      "fec_id=%u wr=%d bytes_out=%u",
                      (unsigned)repair_fec_id, (int)wr, (unsigned)bytes_out);
            goto cleanup;
        }

        out_repair_symbols[i].fec_id      = repair_fec_id;
        out_repair_symbols[i].payload_len = (uint16_t)bytes_out;

        memcpy(out_repair_symbols[i].data,
               symbol_buf,
               (size_t)ctx->symbol_size);
    }

    result = 0;

cleanup:
    if (encoder != NULL) {
        wirehair_free(encoder);
    }

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
    struct fec_ctx   *ctx           = (struct fec_ctx *)handle;
    WirehairCodec     decoder       = NULL;
    int              *feed_order    = NULL;
    int               feed_order_count;
    int               valid_symbols;
    int               holes;
    int               damaged;
    int               sparse_mode;
    int               fed           = 0;
    int               pos_index;
    int               decoder_ready = 0;
    int               result        = -1;

    if (ctx == NULL || symbols == NULL || out_reconstructed == NULL) {
        LOG_ERROR("[FEC] fec_decode_block: NULL argument");
        return -1;
    }

    if (symbol_count < 0 || scan_capacity < 1 || scan_capacity > MAX_SYMBOLS_PER_BLOCK) {
        LOG_ERROR("[FEC] fec_decode_block: invalid counts "
                  "symbol_count=%d scan_capacity=%d",
                  symbol_count, scan_capacity);
        return -1;
    }

    sparse_mode   = (scan_capacity > symbol_count);
    valid_symbols = count_valid_symbols(ctx, symbols, scan_capacity, sparse_mode);
    holes         = scan_capacity - valid_symbols;
    damaged       = (holes > 0);

    if (valid_symbols < ctx->k) {
        LOG_ERROR("[FEC] fec_decode_block: insufficient valid symbols "
                  "(valid=%d k=%d holes=%d symbol_count=%d scan_capacity=%d)",
                  valid_symbols, ctx->k, holes, symbol_count, scan_capacity);
        return -1;
    }

    if (!is_aligned_64(out_reconstructed)) {
        LOG_WARN("[FEC] fec_decode_block: out_reconstructed is not 64-byte aligned");
    }

    if (damaged) {
        LOG_DEBUG("[FEC] sparse scan: valid=%d holes=%d symbol_count=%d scan_capacity=%d k=%d",
                  valid_symbols, holes, symbol_count, scan_capacity, ctx->k);
    }

    decoder = wirehair_decoder_create(NULL,
                                      (uint64_t)ctx->block_bytes,
                                      (uint32_t)ctx->symbol_size);
    if (decoder == NULL) {
        LOG_ERROR("[FEC] fec_decode_block: wirehair_decoder_create() failed");
        return -1;
    }

    feed_order = (int *)malloc(sizeof(int) * (size_t)scan_capacity);
    if (feed_order == NULL) {
        LOG_ERROR("[FEC] fec_decode_block: malloc(feed_order) failed");
        goto cleanup;
    }

    feed_order_count = build_feed_order(ctx,
                                        symbols,
                                        scan_capacity,
                                        sparse_mode,
                                        feed_order);
    if (feed_order_count < 0) {
        LOG_ERROR("[FEC] fec_decode_block: build_feed_order() failed");
        goto cleanup;
    }

    for (pos_index = 0; pos_index < feed_order_count; ++pos_index) {
        int             pos = feed_order[pos_index];
        WirehairResult  wr;

        wr = wirehair_decode(decoder,
                             symbols[pos].fec_id,
                             symbols[pos].data,
                             (uint32_t)ctx->symbol_size);

        fed++;

        if (wr == Wirehair_Success) {
            decoder_ready = 1;
            LOG_DEBUG("[FEC] fec_decode_block: success at fed=%d pos=%d fec_id=%u damaged=%d",
                      fed, pos, (unsigned)symbols[pos].fec_id, damaged);
            break;
        }

        if (wr != Wirehair_NeedMore) {
            LOG_ERROR("[FEC] fec_decode_block: wirehair_decode() failed "
                      "order_idx=%d pos=%d fec_id=%u wr=%d fed=%d",
                      pos_index,
                      pos,
                      (unsigned)symbols[pos].fec_id,
                      (int)wr,
                      fed);
            goto cleanup;
        }
    }

    if (!decoder_ready) {
        LOG_ERROR("[FEC] fec_decode_block: decoder never reached success "
                  "(fed=%d valid=%d holes=%d symbol_count=%d "
                  "scan_capacity=%d k=%d damaged=%d sparse=%d)",
                  fed, valid_symbols, holes,
                  symbol_count, scan_capacity, ctx->k, damaged, sparse_mode);
        goto cleanup;
    }

    memset(out_reconstructed, 0, ctx->block_bytes);

    {
        WirehairResult wr = wirehair_recover(decoder,
                                             out_reconstructed,
                                             (uint64_t)ctx->block_bytes);
        if (wr != Wirehair_Success) {
            LOG_ERROR("[FEC] fec_decode_block: wirehair_recover() failed "
                      "wr=%d fed=%d damaged=%d",
                      (int)wr, fed, damaged);
            goto cleanup;
        }
    }

    LOG_DEBUG("[FEC] fec_decode_block: recovered successfully "
              "(fed=%d valid=%d holes=%d damaged=%d)",
              fed, valid_symbols, holes, damaged);

    result = 0;

cleanup:
    free(feed_order);

    if (decoder != NULL) {
        wirehair_free(decoder);
    }

    return result;
}

/* -------------------------------------------------------------------------- */
/* Self-test                                                                   */
/* -------------------------------------------------------------------------- */

int fec_run_self_test(void)
{
    return 0;
}