/*
 * include/fec_wrapper.h
 */

#ifndef FSO_FEC_WRAPPER_H
#define FSO_FEC_WRAPPER_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct fec_ctx;
typedef struct fec_ctx *fec_handle_t;

fec_handle_t fec_create(int k, int symbol_size);
void         fec_destroy(fec_handle_t handle);

int fec_encode_block(fec_handle_t          handle,
                     const unsigned char  *source_data,
                     symbol_t             *out_repair_symbols,
                     int                   m);

/*
 * fec_decode_block() — recover the source block from received symbols.
 *
 * symbols        : array of symbol_t to decode from.
 * symbol_count   : number of symbols actually received (used for logging
 *                  and the compact-array fast path).
 * scan_capacity  : number of array slots to examine for valid symbols.
 *
 *   COMPACT array (stress test, raw received[] with no holes):
 *     Pass scan_capacity = symbol_count.
 *     Every slot 0..symbol_count-1 is a valid symbol.
 *
 *   SPARSE array (deinterleaver block_t, indexed by fec_id):
 *     Pass scan_capacity = block.symbols_per_block  (K+M, e.g. 96).
 *     Slots at lost fec_id positions have payload_len=0 and are skipped.
 *
 * out_reconstructed : caller-allocated, zeroed buffer of k*symbol_size bytes.
 *
 * Returns 0 on success, -1 on failure.
 */
int fec_decode_block(fec_handle_t   handle,
                     symbol_t      *symbols,
                     int            symbol_count,
                     int            scan_capacity,
                     unsigned char *out_reconstructed);

int fec_run_self_test(void);

#ifdef __cplusplus
}
#endif

#endif /* FSO_FEC_WRAPPER_H */