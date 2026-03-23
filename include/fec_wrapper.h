/*
 * include/fec_wrapper.h — Forward Error Correction API for the FSO Gateway.
 *
 * Return codes
 * ------------
 * All three codes are part of the public API.  Callers must test against
 * these named constants; do NOT compare against raw integer literals.
 *
 *   FEC_DECODE_OK             (0)   Reconstruction succeeded.
 *   FEC_DECODE_ERR           (-1)   Generic failure (NULL args, Wirehair
 *                                   error, valid_symbols < K).
 *   FEC_DECODE_TOO_MANY_HOLES (-2)  holes > M — block is irrecoverable.
 *                                   The deinterleaver layer already enforces
 *                                   this before calling fec_decode_block(),
 *                                   so callers that use the deinterleaver
 *                                   pipeline should never see -2 in normal
 *                                   operation.  Tests and direct callers
 *                                   (e.g. burst_sim_test compact mode) must
 *                                   handle it explicitly.
 *
 * fec_decode_block() scan modes
 * ------------------------------
 *   COMPACT (stress test — raw received[] with no holes):
 *     Pass scan_capacity = symbol_count.
 *     Every slot 0..symbol_count-1 is a valid symbol.
 *
 *   SPARSE (deinterleaver block_t — indexed by fec_id):
 *     Pass scan_capacity = block.symbols_per_block  (K + M).
 *     Slots at lost fec_id positions have payload_len == 0 and are skipped.
 *
 * Source-First Feeding
 * --------------------
 *   fec_decode_block() feeds source symbols (fec_id < K) to Wirehair before
 *   repair symbols (fec_id >= K).  This minimises decode iterations in the
 *   common no-loss or low-loss case.
 */

#ifndef FSO_FEC_WRAPPER_H
#define FSO_FEC_WRAPPER_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Public return codes for fec_decode_block()                                 */
/* -------------------------------------------------------------------------- */

#define FEC_DECODE_OK             0   /* reconstruction successful            */
#define FEC_DECODE_ERR           (-1) /* generic failure                      */
#define FEC_DECODE_TOO_MANY_HOLES (-2) /* holes > M — irrecoverable           */

/* -------------------------------------------------------------------------- */
/* Opaque context handle                                                       */
/* -------------------------------------------------------------------------- */

struct fec_ctx;
typedef struct fec_ctx *fec_handle_t;

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

/*
 * fec_create() — allocate a FEC context.
 *
 *   k           : number of source symbols per block  (>= 2).
 *   symbol_size : bytes per symbol  (> 0).
 *
 * Returns non-NULL on success, NULL on failure.
 * Must be released with fec_destroy().
 */
fec_handle_t fec_create(int k, int symbol_size);

/*
 * fec_destroy() — free context.  Safe to call with NULL.
 */
void fec_destroy(fec_handle_t handle);

/* -------------------------------------------------------------------------- */
/* Encoding                                                                    */
/* -------------------------------------------------------------------------- */

/*
 * fec_encode_block() — generate m repair symbols from source data.
 *
 *   handle             : FEC context.
 *   source_data        : k * symbol_size bytes of source data (64-byte aligned
 *                        recommended).
 *   out_repair_symbols : array of at least m symbol_t to receive repair data.
 *                        fec_id and payload_len are set by this function.
 *   m                  : number of repair symbols to generate  (>= 1).
 *
 * Returns FEC_DECODE_OK (0) on success, FEC_DECODE_ERR (-1) on failure.
 */
int fec_encode_block(fec_handle_t          handle,
                     const unsigned char  *source_data,
                     symbol_t             *out_repair_symbols,
                     int                   m);

/* -------------------------------------------------------------------------- */
/* Decoding                                                                    */
/* -------------------------------------------------------------------------- */

/*
 * fec_decode_block() — recover the source block from received symbols.
 *
 *   handle           : FEC context (must have been created with the same K and
 *                      symbol_size used during encoding).
 *   symbols          : array of symbol_t to decode from (may be sparse).
 *   symbol_count     : number of symbols actually received.  Used for the
 *                      compact fast-path and for logging.
 *   scan_capacity    : number of array slots to examine.
 *                        COMPACT: pass scan_capacity = symbol_count.
 *                        SPARSE:  pass scan_capacity = block.symbols_per_block.
 *   out_reconstructed: caller-allocated buffer of k * symbol_size bytes.
 *                      Should be 64-byte aligned and zeroed before the call.
 *
 * Returns:
 *   FEC_DECODE_OK (0)             success.
 *   FEC_DECODE_ERR (-1)           generic failure.
 *   FEC_DECODE_TOO_MANY_HOLES(-2) holes > M; block is irrecoverable.
 */
int fec_decode_block(fec_handle_t   handle,
                     symbol_t      *symbols,
                     int            symbol_count,
                     int            scan_capacity,
                     unsigned char *out_reconstructed);

/* -------------------------------------------------------------------------- */
/* Self-test                                                                   */
/* -------------------------------------------------------------------------- */

/*
 * fec_run_self_test() — quick encode/decode sanity check.
 * Returns 0 on success, -1 on failure.
 * Called from main() at startup to gate-keep a broken Wirehair build.
 */
int fec_run_self_test(void);

#ifdef __cplusplus
}
#endif

#endif /* FSO_FEC_WRAPPER_H */
