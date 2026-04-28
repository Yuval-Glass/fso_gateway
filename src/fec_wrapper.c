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
 *   Valid source symbols (fec_id < K) are fed first.
 *   Valid repair symbols (fec_id >= K) are fed second.
 *
 * Runtime telemetry
 * -----------------
 *   The decoder records telemetry for the last decode attempt:
 *     - block_id
 *     - number_of_symbols_received
 *     - number_of_missing_symbols
 *     - number_of_repair_symbols_used
 *     - decode_success
 *
 *   sim_runner.c reads this via local extern declarations.
 *
 * CRC note
 * --------
 *   fec_encode_block() produces repair symbol payloads and metadata but does
 *   NOT stamp CRC.  CRC must be stamped by the caller (e.g. sim_runner's
 *   sr_encode_one_block) AFTER all metadata fields (packet_id, fec_id,
 *   symbol_index, total_symbols, payload_len) are fully populated.
 *
 *   The rationale is that fec_encode_block() only sets fec_id and payload_len
 *   on repair symbols.  The caller assigns packet_id (block index),
 *   total_symbols, and symbol_index before pushing to the interleaver.  CRC
 *   must be stamped after that assignment — not inside fec_encode_block().
 */

#define _POSIX_C_SOURCE 200112L

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <wirehair/wirehair.h>

#include "fec_wrapper.h"
#include "logging.h"
#include "types.h"

struct fec_ctx {
    int            k;
    int            symbol_size;
    int            block_bytes;
    unsigned char *symbol_buf;  /* pre-allocated encode scratch, symbol_size bytes */
    WirehairCodec  encoder;     /* reused across fec_encode_block calls */
};

typedef struct fec_decode_telemetry_t {
    uint64_t block_id;
    int      number_of_symbols_received;
    int      number_of_missing_symbols;
    int      number_of_repair_symbols_used;
    int      decode_success;
} fec_decode_telemetry_t;

static uint64_t                g_current_decode_block_id = 0U;
static fec_decode_telemetry_t  g_last_decode_telemetry;

static void fec_clear_last_decode_telemetry(void)
{
    memset(&g_last_decode_telemetry, 0, sizeof(g_last_decode_telemetry));
    g_last_decode_telemetry.block_id = g_current_decode_block_id;
}

void fec_set_current_decode_block_id(uint64_t block_id)
{
    g_current_decode_block_id = block_id;
}

void fec_get_last_decode_telemetry(fec_decode_telemetry_t *out)
{
    if (out == NULL) {
        return;
    }

    *out = g_last_decode_telemetry;
}

static int fec_count_valid_symbols(const symbol_t *symbols, int scan_capacity)
{
    int i;
    int count = 0;

    for (i = 0; i < scan_capacity; ++i) {
        /* packet_id==0 means the slot is an unfilled hole (erasure).
         * Source symbols with payload_len=0 but packet_id!=0 are valid
         * block-builder zero-padding and must be counted. */
        if (symbols[i].packet_id != 0U) {
            count++;
        }
    }

    return count;
}

static int fec_build_feed_order(const symbol_t *symbols,
                                int             scan_capacity,
                                int             k,
                                int            *order_out,
                                int            *repair_available_out)
{
    int i;
    int pos = 0;
    int repair_available = 0;

    for (i = 0; i < scan_capacity; ++i) {
        if (symbols[i].packet_id == 0U) {
            continue;  /* unfilled hole — skip */
        }

        if ((int)symbols[i].fec_id < k) {
            order_out[pos++] = i;
        }
    }

    for (i = 0; i < scan_capacity; ++i) {
        if (symbols[i].packet_id == 0U) {
            continue;  /* unfilled hole — skip */
        }

        if ((int)symbols[i].fec_id >= k) {
            order_out[pos++] = i;
            repair_available++;
        }
    }

    if (repair_available_out != NULL) {
        *repair_available_out = repair_available;
    }

    return pos;
}

fec_handle_t fec_create(int k, int symbol_size)
{
    struct fec_ctx *ctx;

    if (k < 2 || symbol_size <= 0) {
        LOG_ERROR("[FEC] fec_create: invalid parameters k=%d symbol_size=%d",
                  k, symbol_size);
        return NULL;
    }

    ctx = (struct fec_ctx *)calloc(1U, sizeof(*ctx));
    if (ctx == NULL) {
        LOG_ERROR("[FEC] fec_create: calloc failed");
        return NULL;
    }

    ctx->k           = k;
    ctx->symbol_size = symbol_size;
    ctx->block_bytes = k * symbol_size;
    ctx->encoder     = NULL;

    ctx->symbol_buf = (unsigned char *)malloc((size_t)symbol_size);
    if (ctx->symbol_buf == NULL) {
        LOG_ERROR("[FEC] fec_create: malloc(symbol_buf, %d) failed", symbol_size);
        free(ctx);
        return NULL;
    }

    LOG_INFO("[FEC] Created: k=%d symbol_size=%d block_bytes=%d",
             ctx->k, ctx->symbol_size, ctx->block_bytes);

    return ctx;
}

void fec_destroy(fec_handle_t handle)
{
    struct fec_ctx *ctx = handle;
    if (ctx == NULL) {
        return;
    }
    if (ctx->encoder != NULL) {
        wirehair_free(ctx->encoder);
    }
    free(ctx->symbol_buf);
    free(ctx);
}

int fec_encode_block(fec_handle_t          handle,
                     const unsigned char  *source_data,
                     symbol_t             *out_repair_symbols,
                     int                   m)
{
    struct fec_ctx   *ctx = handle;
    WirehairCodec     new_enc;
    int               i;

    if (ctx == NULL || source_data == NULL || out_repair_symbols == NULL || m < 1) {
        LOG_ERROR("[FEC] fec_encode_block: invalid arguments");
        return FEC_DECODE_ERR;
    }

    /* Reuse encoder allocation across calls — wirehair_encoder_create with a
     * non-NULL first argument reinitialises the existing codec in place, avoiding
     * a heap alloc/free on every block.  The returned pointer replaces ctx->encoder
     * (it may be the same pointer or a newly allocated one if the codec had to grow). */
    new_enc = wirehair_encoder_create(ctx->encoder,
                                      source_data,
                                      (uint64_t)ctx->block_bytes,
                                      (uint32_t)ctx->symbol_size);
    if (new_enc == NULL) {
        LOG_ERROR("[FEC] fec_encode_block: wirehair_encoder_create() failed");
        return FEC_DECODE_ERR;
    }
    ctx->encoder = new_enc;

    for (i = 0; i < m; ++i) {
        uint32_t       bytes_out     = 0U;
        uint32_t       repair_fec_id = (uint32_t)(ctx->k + i);
        WirehairResult wr;

        wr = wirehair_encode(ctx->encoder,
                             repair_fec_id,
                             ctx->symbol_buf,
                             (uint32_t)ctx->symbol_size,
                             &bytes_out);

        if (wr != Wirehair_Success ||
            bytes_out != (uint32_t)ctx->symbol_size) {
            LOG_ERROR("[FEC] fec_encode_block: wirehair_encode() failed "
                      "fec_id=%u wr=%d bytes_out=%u",
                      (unsigned)repair_fec_id,
                      (int)wr,
                      (unsigned)bytes_out);
            return FEC_DECODE_ERR;
        }

        /*
         * Set only the fields that fec_encode_block owns.
         * packet_id, symbol_index, total_symbols are set to 0 here and
         * filled by the caller before the symbol is pushed to the interleaver.
         * crc32 = 0 so any accidental use of an unfinished symbol fails CRC.
         * data[] is written by memcpy below — no full-struct memset needed.
         */
        out_repair_symbols[i].packet_id     = 0;
        out_repair_symbols[i].fec_id        = repair_fec_id;
        out_repair_symbols[i].symbol_index  = 0;
        out_repair_symbols[i].total_symbols = 0;
        out_repair_symbols[i].payload_len   = (uint16_t)bytes_out;
        out_repair_symbols[i].crc32         = 0;
        memcpy(out_repair_symbols[i].data,
               ctx->symbol_buf,
               (size_t)ctx->symbol_size);
    }

    return FEC_DECODE_OK;
}

int fec_decode_block(fec_handle_t   handle,
                     symbol_t      *symbols,
                     int            symbol_count,
                     int            scan_capacity,
                     unsigned char *out_reconstructed)
{
    struct fec_ctx    *ctx = handle;
    WirehairCodec      decoder = NULL;
    int               *feed_order = NULL;
    int                valid_received;
    int                repair_available = 0;
    int                feed_count;
    int                effective_n;
    int                missing_count;
    int                m_limit;
    int                result = FEC_DECODE_ERR;
    int                repair_used = 0;
    int                fed;
    int                decode_success = 0;

    fec_clear_last_decode_telemetry();

    if (ctx == NULL || symbols == NULL || out_reconstructed == NULL ||
        symbol_count < 0 || scan_capacity < 0) {
        LOG_ERROR("[FEC] fec_decode_block: invalid arguments");
        return FEC_DECODE_ERR;
    }

    if (scan_capacity < symbol_count) {
        scan_capacity = symbol_count;
    }

    valid_received = fec_count_valid_symbols(symbols, scan_capacity);
    effective_n    = scan_capacity;
    missing_count  = (effective_n > valid_received) ? (effective_n - valid_received) : 0;
    m_limit        = (effective_n > ctx->k) ? (effective_n - ctx->k) : 0;

    g_last_decode_telemetry.block_id                    = g_current_decode_block_id;
    g_last_decode_telemetry.number_of_symbols_received  = valid_received;
    g_last_decode_telemetry.number_of_missing_symbols   = missing_count;
    g_last_decode_telemetry.number_of_repair_symbols_used = 0;
    g_last_decode_telemetry.decode_success              = 0;

    if (valid_received < ctx->k) {
        LOG_WARN("[FEC] fec_decode_block: block=%llu insufficient symbols "
                 "received=%d required=%d",
                 (unsigned long long)g_current_decode_block_id,
                 valid_received,
                 ctx->k);
        return FEC_DECODE_ERR;
    }

    if (effective_n > ctx->k && missing_count > m_limit) {
        LOG_WARN("[FEC] fec_decode_block: block=%llu too many holes "
                 "received=%d missing=%d m=%d",
                 (unsigned long long)g_current_decode_block_id,
                 valid_received,
                 missing_count,
                 m_limit);
        return FEC_DECODE_TOO_MANY_HOLES;
    }

    feed_order = (int *)calloc((size_t)scan_capacity, sizeof(int));
    if (feed_order == NULL) {
        LOG_ERROR("[FEC] fec_decode_block: feed_order allocation failed");
        return FEC_DECODE_ERR;
    }

    feed_count = fec_build_feed_order(symbols,
                                      scan_capacity,
                                      ctx->k,
                                      feed_order,
                                      &repair_available);

    decoder = wirehair_decoder_create(NULL,
                                      (uint64_t)ctx->block_bytes,
                                      (uint32_t)ctx->symbol_size);
    if (decoder == NULL) {
        LOG_ERROR("[FEC] fec_decode_block: wirehair_decoder_create() failed");
        goto cleanup;
    }

    for (fed = 0; fed < feed_count; ++fed) {
        const symbol_t   *sym = &symbols[feed_order[fed]];
        WirehairResult    wr;

        wr = wirehair_decode(decoder,
                             sym->fec_id,
                             sym->data,
                             (uint32_t)ctx->symbol_size);

        if ((int)sym->fec_id >= ctx->k) {
            repair_used++;
        }

        if (wr == Wirehair_Success) {
            decode_success = 1;
            break;
        }

        if (wr == Wirehair_NeedMore) {
            continue;
        }

        LOG_ERROR("[FEC] fec_decode_block: block=%llu wirehair_decode failed "
                  "fec_id=%u wr=%d after fed=%d",
                  (unsigned long long)g_current_decode_block_id,
                  (unsigned)sym->fec_id,
                  (int)wr,
                  fed + 1);
        goto cleanup;
    }

    if (!decode_success) {
        LOG_WARN("[FEC] fec_decode_block: block=%llu decode incomplete "
                 "received=%d missing=%d repair_avail=%d repair_used=%d",
                 (unsigned long long)g_current_decode_block_id,
                 valid_received,
                 missing_count,
                 repair_available,
                 repair_used);
        goto cleanup;
    }

    memset(out_reconstructed, 0, (size_t)ctx->block_bytes);

    {
        WirehairResult wr;

        wr = wirehair_recover(decoder,
                              out_reconstructed,
                              (uint64_t)ctx->block_bytes);
        if (wr != Wirehair_Success) {
            LOG_ERROR("[FEC] fec_decode_block: block=%llu wirehair_recover failed wr=%d",
                      (unsigned long long)g_current_decode_block_id,
                      (int)wr);
            goto cleanup;
        }
    }

    g_last_decode_telemetry.number_of_repair_symbols_used = repair_used;
    g_last_decode_telemetry.decode_success                = 1;

    LOG_INFO("[FEC] decode telemetry: block=%llu received=%d missing=%d repair=%d success=YES",
             (unsigned long long)g_last_decode_telemetry.block_id,
             g_last_decode_telemetry.number_of_symbols_received,
             g_last_decode_telemetry.number_of_missing_symbols,
             g_last_decode_telemetry.number_of_repair_symbols_used);

    result = FEC_DECODE_OK;

cleanup:
    if (result != FEC_DECODE_OK) {
        g_last_decode_telemetry.number_of_repair_symbols_used = repair_used;
        g_last_decode_telemetry.decode_success                = 0;

        LOG_INFO("[FEC] decode telemetry: block=%llu received=%d missing=%d repair=%d success=NO",
                 (unsigned long long)g_last_decode_telemetry.block_id,
                 g_last_decode_telemetry.number_of_symbols_received,
                 g_last_decode_telemetry.number_of_missing_symbols,
                 g_last_decode_telemetry.number_of_repair_symbols_used);
    }

    if (decoder != NULL) {
        wirehair_free(decoder);
    }
    free(feed_order);
    return result;
}

int fec_run_self_test(void)
{
    enum {
        TEST_K = 8,
        TEST_M = 2,
        TEST_SYMBOL_SIZE = 128
    };

    fec_handle_t   fec = NULL;
    unsigned char *source = NULL;
    unsigned char *recon  = NULL;
    symbol_t       repairs[TEST_M];
    symbol_t       received[TEST_K + TEST_M];
    int            i;
    int            rc = -1;

    fec = fec_create(TEST_K, TEST_SYMBOL_SIZE);
    if (fec == NULL) {
        return -1;
    }

    source = (unsigned char *)calloc((size_t)TEST_K * TEST_SYMBOL_SIZE, sizeof(unsigned char));
    recon  = (unsigned char *)calloc((size_t)TEST_K * TEST_SYMBOL_SIZE, sizeof(unsigned char));
    if (source == NULL || recon == NULL) {
        goto cleanup;
    }

    for (i = 0; i < TEST_K * TEST_SYMBOL_SIZE; ++i) {
        source[i] = (unsigned char)((i * 17 + 3) & 0xFF);
    }

    if (fec_encode_block(fec, source, repairs, TEST_M) != FEC_DECODE_OK) {
        goto cleanup;
    }

    memset(received, 0, sizeof(received));

    for (i = 0; i < TEST_K; ++i) {
        received[i].fec_id      = (uint32_t)i;
        received[i].payload_len = TEST_SYMBOL_SIZE;
        memcpy(received[i].data,
               source + (size_t)i * TEST_SYMBOL_SIZE,
               TEST_SYMBOL_SIZE);
    }

    for (i = 0; i < TEST_M; ++i) {
        received[TEST_K + i] = repairs[i];
    }

    memset(&received[3], 0, sizeof(symbol_t));

    fec_set_current_decode_block_id(0U);

    if (fec_decode_block(fec,
                         received,
                         TEST_K + TEST_M - 1,
                         TEST_K + TEST_M,
                         recon) != FEC_DECODE_OK) {
        goto cleanup;
    }

    if (memcmp(source, recon, (size_t)TEST_K * TEST_SYMBOL_SIZE) != 0) {
        LOG_ERROR("[FEC] fec_run_self_test: reconstructed payload mismatch");
        goto cleanup;
    }

    rc = 0;

cleanup:
    free(source);
    free(recon);
    fec_destroy(fec);
    return rc;
}
