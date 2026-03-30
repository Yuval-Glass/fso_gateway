/**
 * @file symbol.c
 * @brief Symbol lifecycle, CRC-32C helpers, and debug utilities.
 *
 * CRC algorithm: CRC-32C (Castagnoli)
 * ------------------------------------
 * Polynomial:  0x1EDC6F41  (reflected: 0x82F63B78)
 * Seed:        0xFFFFFFFF
 * Final XOR:   0xFFFFFFFF
 *
 * This is the same polynomial as iSCSI, SCTP, Btrfs, and the hardware
 * instructions SSE4.2 _mm_crc32_u8/u32/u64 and ARMv8 CRC32CB/CRC32CH/
 * CRC32CW/CRC32CX.
 *
 * Hardware acceleration path
 * --------------------------
 * The software implementation is factored so that a hardware backend can be
 * substituted by replacing crc32c_sw_update() with a CPU-specific version:
 *
 *   // SSE4.2 example (x86-64):
 *   static uint32_t crc32c_hw_update(uint32_t crc,
 *                                    const unsigned char *buf, size_t len)
 *   {
 *       while (len >= 8) {
 *           crc = (uint32_t)_mm_crc32_u64(crc, *(const uint64_t *)buf);
 *           buf += 8; len -= 8;
 *       }
 *       while (len--) { crc = _mm_crc32_u8(crc, *buf++); }
 *       return crc;
 *   }
 *
 * Protected fields and serialisation order
 * -----------------------------------------
 * The 14-byte big-endian header covers:
 *   packet_id     [0..3]    4 bytes BE
 *   fec_id        [4..7]    4 bytes BE
 *   symbol_index  [8..9]    2 bytes BE
 *   total_symbols [10..11]  2 bytes BE
 *   payload_len   [12..13]  2 bytes BE
 * Followed by data[0..payload_len-1].
 * Padding beyond payload_len is excluded.
 * sym->crc32 is never included in the CRC input.
 */

#include <string.h>

#include "symbol.h"
#include "logging.h"

/* -------------------------------------------------------------------------- */
/* CRC-32C table (Castagnoli, reflected polynomial 0x82F63B78)               */
/* -------------------------------------------------------------------------- */

static uint32_t g_crc32c_table[256];
static int      g_crc32c_table_ready = 0;

static void crc32c_build_table(void)
{
    uint32_t i;
    uint32_t j;
    uint32_t crc;

    for (i = 0U; i < 256U; ++i) {
        crc = i;
        for (j = 0U; j < 8U; ++j) {
            if (crc & 1U) {
                crc = (crc >> 1) ^ 0x82F63B78U;   /* Castagnoli reflected */
            } else {
                crc >>= 1;
            }
        }
        g_crc32c_table[i] = crc;
    }

    g_crc32c_table_ready = 1;
}

/*
 * Software CRC-32C update over a byte buffer.
 * Can be replaced by a hardware-accelerated version (SSE4.2, ARMv8 CRC32C)
 * without changing any of the callers.
 */
static uint32_t crc32c_sw_update(uint32_t crc,
                                  const unsigned char *buf,
                                  size_t               len)
{
    size_t i;

    for (i = 0U; i < len; ++i) {
        crc = g_crc32c_table[(crc ^ (uint32_t)buf[i]) & 0xFFU] ^ (crc >> 8);
    }

    return crc;
}

/* -------------------------------------------------------------------------- */
/* Public CRC-32C over arbitrary buffer                                       */
/* -------------------------------------------------------------------------- */

uint32_t symbol_crc32c(const unsigned char *data, size_t length)
{
    uint32_t crc;

    if (!g_crc32c_table_ready) {
        crc32c_build_table();
    }

    crc = 0xFFFFFFFFU;
    crc = crc32c_sw_update(crc, data, length);
    return crc ^ 0xFFFFFFFFU;
}

/* -------------------------------------------------------------------------- */
/* Header serialisation (14 bytes, big-endian)                                */
/* -------------------------------------------------------------------------- */

#define SYMBOL_HDR_SIZE 14U

static void symbol_serialise_header(const symbol_t *sym,
                                     unsigned char   hdr[SYMBOL_HDR_SIZE])
{
    /* packet_id — 4 bytes big-endian */
    hdr[0] = (unsigned char)((sym->packet_id >> 24) & 0xFFU);
    hdr[1] = (unsigned char)((sym->packet_id >> 16) & 0xFFU);
    hdr[2] = (unsigned char)((sym->packet_id >>  8) & 0xFFU);
    hdr[3] = (unsigned char)( sym->packet_id        & 0xFFU);

    /* fec_id — 4 bytes big-endian */
    hdr[4] = (unsigned char)((sym->fec_id >> 24) & 0xFFU);
    hdr[5] = (unsigned char)((sym->fec_id >> 16) & 0xFFU);
    hdr[6] = (unsigned char)((sym->fec_id >>  8) & 0xFFU);
    hdr[7] = (unsigned char)( sym->fec_id        & 0xFFU);

    /* symbol_index — 2 bytes big-endian */
    hdr[8] = (unsigned char)((sym->symbol_index >> 8) & 0xFFU);
    hdr[9] = (unsigned char)( sym->symbol_index       & 0xFFU);

    /* total_symbols — 2 bytes big-endian */
    hdr[10] = (unsigned char)((sym->total_symbols >> 8) & 0xFFU);
    hdr[11] = (unsigned char)( sym->total_symbols       & 0xFFU);

    /* payload_len — 2 bytes big-endian */
    hdr[12] = (unsigned char)((sym->payload_len >> 8) & 0xFFU);
    hdr[13] = (unsigned char)( sym->payload_len       & 0xFFU);
}

/* Compute the raw CRC-32C value without storing it. */
static uint32_t symbol_compute_crc_value(const symbol_t *sym)
{
    unsigned char hdr[SYMBOL_HDR_SIZE];
    uint32_t      crc;
    size_t        pl;

    if (!g_crc32c_table_ready) {
        crc32c_build_table();
    }

    symbol_serialise_header(sym, hdr);

    crc = 0xFFFFFFFFU;
    crc = crc32c_sw_update(crc, hdr, SYMBOL_HDR_SIZE);

    pl = (size_t)sym->payload_len;
    if (pl > 0U && pl <= MAX_SYMBOL_DATA_SIZE) {
        crc = crc32c_sw_update(crc, sym->data, pl);
    }

    return crc ^ 0xFFFFFFFFU;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

void symbol_init(symbol_t *sym)
{
    if (sym == NULL) {
        return;
    }
    memset(sym, 0, sizeof(*sym));
}

void symbol_dump(const symbol_t *sym)
{
    if (sym == NULL) {
        LOG_DEBUG("symbol_dump called with NULL symbol");
        return;
    }

    LOG_DEBUG(
        "Symbol dump: packet_id=%u fec_id=%u symbol_index=%u "
        "total_symbols=%u payload_len=%u crc32=0x%08X",
        sym->packet_id,
        sym->fec_id,
        sym->symbol_index,
        sym->total_symbols,
        sym->payload_len,
        sym->crc32
    );
}

void symbol_compute_crc(symbol_t *sym)
{
    if (sym == NULL) {
        return;
    }
    sym->crc32 = symbol_compute_crc_value(sym);
}

int symbol_verify_crc(const symbol_t *sym)
{
    if (sym == NULL) {
        return 0;
    }
    return (sym->crc32 == symbol_compute_crc_value(sym)) ? 1 : 0;
}
