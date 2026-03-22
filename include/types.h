/*
 * include/types.h — Core data types for the FSO Gateway pipeline.
 *
 * All modules in the pipeline (fragmenter, FEC wrapper, interleaver,
 * deinterleaver, block builder) share these definitions.  Keep this
 * header dependency-free except for the C standard library.
 *
 * Layout of a received Ethernet packet through the pipeline:
 *
 *   Ethernet frame
 *       └─> fragment_packet()     →  N × symbol_t  (packet_id + fec_id set)
 *           └─> fec_encode_block() →  M repair symbol_t  (fec_id = K..K+M-1)
 *               └─> interleaver   →  column-major symbol stream
 *                   └─> [wire / FSO link]
 *                       └─> deinterleaver  →  block_t  (all N symbols)
 *                           └─> fec_decode_block()  →  recovered source bytes
 */

#ifndef FSO_TYPES_H
#define FSO_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Symbol                                                                      */
/* -------------------------------------------------------------------------- */

/*
 * Maximum payload bytes stored inside a single symbol's data array.
 *
 * Sized for jumbo-frame fragmentation (symbol_size up to 9000 bytes).
 * In practice the gateway runs at symbol_size = 1500, so only the first
 * 1500 bytes of this array are used — the rest is never touched.
 */
#define MAX_SYMBOL_DATA_SIZE 9000U

/*
 * symbol_t — the atomic unit of the FSO processing pipeline.
 *
 * Field ownership by stage:
 *
 *   packet_id      set by fragment_packet(); identifies the original Ethernet
 *                  frame.  The deinterleaver uses it as the block identifier
 *                  (block_id mod depth → interleave slot).
 *
 *   fec_id         set by fragment_packet() for source symbols (0..K-1) and
 *                  by fec_encode_block() for repair symbols (K..K+M-1).
 *                  Used by Wirehair as the symbol index.
 *
 *   symbol_index   fragmentation index within the original packet (0-based).
 *                  Redundant with fec_id for source symbols; kept for
 *                  protocol compatibility with the wire format.
 *
 *   total_symbols  total number of symbols this packet was fragmented into.
 *                  Allows the receiver to know when a packet is complete
 *                  independently of the FEC block boundary.
 *
 *   payload_len    valid bytes in data[].  Always == symbol_size for source
 *                  and repair symbols; may be less for the last fragment of
 *                  a short packet.
 *
 *   data[]         raw payload bytes, up to MAX_SYMBOL_DATA_SIZE.
 */
typedef struct symbol_t {
    uint32_t      packet_id;
    uint32_t      fec_id;
    uint16_t      symbol_index;
    uint16_t      total_symbols;
    uint16_t      payload_len;
    unsigned char data[MAX_SYMBOL_DATA_SIZE];
} symbol_t;

/* -------------------------------------------------------------------------- */
/* Block                                                                       */
/* -------------------------------------------------------------------------- */

/*
 * Maximum number of symbols that can be stored inline in a block_t.
 *
 * Must be >= K + M for the largest geometry the gateway will use.
 * Reference geometry: K=64, M=32 → N=96.  We use 256 as the ceiling
 * to give headroom for future parameter changes without a recompile.
 *
 * The deinterleaver stores symbols at index [fec_id], so this value
 * also bounds the maximum fec_id the deinterleaver will accept.
 */
#define MAX_SYMBOLS_PER_BLOCK 256

/*
 * block_t — a complete or partially complete FEC block.
 *
 * Two subsystems populate block_t in different ways:
 *
 *   block_builder  (transmit path)
 *     Fills symbols[0..symbol_count-1] in arrival order.
 *     k_limit is set to K; the block is "complete" when
 *     symbol_count == k_limit.
 *
 *   deinterleaver  (receive path)
 *     Fills symbols[fec_id] directly so that symbols[i].fec_id == i
 *     after reassembly.  symbol_count tracks how many distinct fec_id
 *     positions have been filled.  symbols_per_block is K + M.
 *
 * Callers that only need to check whether FEC decoding is possible
 * should compare symbol_count against the K value they were configured
 * with (not symbols_per_block, which is K+M on the receive path).
 *
 * Memory model:
 *   symbols[] is an inline array — no heap allocation required.
 *   sizeof(block_t) ≈ MAX_SYMBOLS_PER_BLOCK × sizeof(symbol_t)
 *                   = 256 × ~9018 bytes ≈ 2.3 MiB
 *
 *   This is intentionally large so that a single block_t can be stack-
 *   or heap-allocated without pointer chasing.  In the deinterleaver,
 *   each slot holds one block_t, giving a total working set of
 *   depth × sizeof(block_t) ≈ 4 × 2.3 MiB ≈ 9.2 MiB — fits in L3.
 *
 * Fields:
 *   block_id         monotonically increasing sequence number assigned
 *                    by the transmitter.
 *
 *   symbol_count     number of symbols currently stored (filled positions).
 *
 *   symbols_per_block  K + M — total capacity of this block.
 *                     0 means "not yet initialised" (block_builder path
 *                     does not use this field).
 *
 *   k_limit          K — the minimum symbols needed for FEC recovery.
 *                    Set by block_builder on the transmit path.
 *                    On the receive path, the FEC wrapper uses the
 *                    fec_handle_t's k value instead.
 *
 *   symbols[]        inline symbol storage, indexed by fec_id on the
 *                    receive path and by arrival order on the transmit path.
 */
typedef struct block_t {
    uint64_t block_id;
    int      symbol_count;
    int      symbols_per_block;   /* K + M  (receive path)     */
    int      k_limit;             /* K only (transmit path)    */
    symbol_t symbols[MAX_SYMBOLS_PER_BLOCK];
} block_t;

#ifdef __cplusplus
}
#endif

#endif /* FSO_TYPES_H */