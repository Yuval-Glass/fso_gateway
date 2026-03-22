/*
 * tests/fec_stress_test.c — FEC Recovery Threshold Stress Test
 *
 * Validates the Wirehair encode/decode round-trip under increasing symbol
 * loss, from 1 lost symbol up to M+5, and reports the exact recovery
 * threshold for this build and geometry.
 *
 * fec_decode_block() calling convention used here — COMPACT array:
 *
 *   received_symbols[] is a tightly packed array with no holes.
 *   scan_capacity is passed as received_count so the decoder never
 *   reads past the end of the allocation (fixes ASan heap-buffer-overflow).
 *
 *   payload_len must be set on every source symbol constructed manually.
 *   fec_decode_block() uses payload_len > 0 as the occupancy sentinel to
 *   distinguish valid symbols from holes in sparse arrays.  Without this,
 *   source symbols would be silently skipped, causing decode failures even
 *   when enough symbols are present.
 *
 *   fec_encode_block() sets payload_len on repair symbols automatically.
 */

#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <wirehair/wirehair.h>

#include "fec_wrapper.h"
#include "logging.h"
#include "types.h"

/* -------------------------------------------------------------------------- */
/* Test geometry                                                               */
/* -------------------------------------------------------------------------- */

#define TEST_K           64
#define TEST_M           32
#define TEST_SYMBOL_SIZE 1500
#define TEST_N           (TEST_K + TEST_M)   /* 96 */

/* -------------------------------------------------------------------------- */
/* Private helpers                                                             */
/* -------------------------------------------------------------------------- */

/*
 * shuffle() — Fisher-Yates in-place shuffle.
 * Used to select which symbols are "lost" randomly each iteration.
 */
static void shuffle(int *array, int n)
{
    int i;

    for (i = n - 1; i > 0; --i) {
        int j   = (int)(rand() % (i + 1));
        int tmp = array[i];
        array[i] = array[j];
        array[j] = tmp;
    }
}

/* -------------------------------------------------------------------------- */
/* Core stress test                                                            */
/* -------------------------------------------------------------------------- */

int run_stress_test(void)
{
    fec_handle_t   handle             = NULL;
    unsigned char *original_data      = NULL;
    unsigned char *reconstructed_data = NULL;
    symbol_t      *all_symbols        = NULL;
    int            failures_below_m   = 0;
    int            first_failure      = -1;
    int            lost_count;
    int            i;

    LOG_INFO("=== Starting FEC Stress Test ===");
    LOG_INFO("Config: K=%d  M=%d  SymbolSize=%d  N=%d",
             TEST_K, TEST_M, TEST_SYMBOL_SIZE, TEST_N);

    /* ------------------------------------------------------------------ */
    /* Allocate                                                            */
    /* ------------------------------------------------------------------ */
    handle = fec_create(TEST_K, TEST_SYMBOL_SIZE);
    if (handle == NULL) {
        LOG_ERROR("[STRESS] fec_create() failed");
        return -1;
    }

    original_data = (unsigned char *)malloc(
                        (size_t)TEST_K * TEST_SYMBOL_SIZE);
    reconstructed_data = (unsigned char *)malloc(
                             (size_t)TEST_K * TEST_SYMBOL_SIZE);
    all_symbols = (symbol_t *)malloc(
                      (size_t)TEST_N * sizeof(symbol_t));

    if (original_data == NULL || reconstructed_data == NULL ||
        all_symbols   == NULL)
    {
        LOG_ERROR("[STRESS] malloc failed");
        goto cleanup;
    }

    memset(all_symbols, 0, (size_t)TEST_N * sizeof(symbol_t));

    /* ------------------------------------------------------------------ */
    /* Generate deterministic source data                                 */
    /*                                                                     */
    /* Fixed seed so source bytes are identical across every run —        */
    /* a failing iteration is therefore fully reproducible.               */
    /* The loss-pattern seed (time-based) is set separately below.        */
    /* ------------------------------------------------------------------ */
    srand(0xFEC0FFEE);
    for (i = 0; i < TEST_K * TEST_SYMBOL_SIZE; ++i) {
        original_data[i] = (unsigned char)(rand() % 256);
    }

    /* Reseed with time for varied loss patterns each run.                */
    /* Remove this line for a fully deterministic sweep.                  */
    srand((unsigned int)time(NULL));

    /* ------------------------------------------------------------------ */
    /* Populate source symbols (fec_id 0 .. K-1)                         */
    /*                                                                     */
    /* payload_len MUST be set to TEST_SYMBOL_SIZE on every source symbol */
    /* we construct manually.  fec_decode_block() uses payload_len > 0   */
    /* as the occupancy sentinel to skip holes in sparse arrays.          */
    /* A zeroed payload_len causes the symbol to be silently skipped,     */
    /* leading to decode failure even when K valid symbols are present.   */
    /* ------------------------------------------------------------------ */
    for (i = 0; i < TEST_K; ++i) {
        all_symbols[i].fec_id      = (uint32_t)i;
        all_symbols[i].payload_len = (uint16_t)TEST_SYMBOL_SIZE;
        memcpy(all_symbols[i].data,
               original_data + (size_t)i * TEST_SYMBOL_SIZE,
               TEST_SYMBOL_SIZE);
    }

    /* ------------------------------------------------------------------ */
    /* Generate M repair symbols (fec_id K .. K+M-1)                     */
    /* fec_encode_block() sets fec_id and payload_len automatically.      */
    /* ------------------------------------------------------------------ */
    if (fec_encode_block(handle, original_data,
                         &all_symbols[TEST_K], TEST_M) != 0)
    {
        LOG_ERROR("[STRESS] fec_encode_block() failed");
        goto cleanup;
    }

    /* ------------------------------------------------------------------ */
    /* Iterative drop test: L = 1 .. M+5                                  */
    /* ------------------------------------------------------------------ */
    LOG_INFO("Running iterative drop test (loss 1 .. %d) ...", TEST_M + 5);

    printf("\n");
    printf("  %-12s  %-10s  %-16s  %s\n",
           "Lost (L)", "Status", "Data", "Note");
    printf("  %-12s  %-10s  %-16s  %s\n",
           "--------", "------", "----", "----");

    for (lost_count = 1; lost_count <= TEST_M + 5; ++lost_count) {
        int       indices[TEST_N];
        int       received_count = TEST_N - lost_count;
        symbol_t *received       = NULL;
        int       dec_rc;
        const char *status;
        const char *data_result;
        const char *note;

        /* Build and shuffle the full index pool */
        for (i = 0; i < TEST_N; ++i) {
            indices[i] = i;
        }

        shuffle(indices, TEST_N);

        /* The first received_count shuffled indices are "received" symbols */
        if (received_count > 0) {
            received = (symbol_t *)malloc(
                           (size_t)received_count * sizeof(symbol_t));
            if (received == NULL) {
                LOG_ERROR("[STRESS] malloc failed for received_symbols "
                          "(lost=%d)", lost_count);
                goto cleanup;
            }

            for (i = 0; i < received_count; ++i) {
                received[i] = all_symbols[indices[i]];
            }
        }

        memset(reconstructed_data, 0,
               (size_t)TEST_K * TEST_SYMBOL_SIZE);

        /*
         * COMPACT array call — no holes, every slot is a valid symbol.
         *
         *   symbol_count  = received_count
         *   scan_capacity = received_count   <-- bounds the scan to the
         *                                       actual allocation; prevents
         *                                       the ASan heap-buffer-overflow
         *                                       that occurred when the decoder
         *                                       scanned up to MAX_SYMBOLS_PER_BLOCK
         *                                       (256) on a smaller array.
         */
        dec_rc = fec_decode_block(handle,
                                  received,
                                  received_count,
                                  received_count,   /* scan_capacity */
                                  reconstructed_data);

        free(received);
        received = NULL;

        /* ---------------------------------------------------------- */
        /* Classify result                                             */
        /* ---------------------------------------------------------- */
        if (dec_rc == 0) {
            int matched = (memcmp(original_data,
                                  reconstructed_data,
                                  (size_t)TEST_K * TEST_SYMBOL_SIZE) == 0);
            status      = "SUCCESS";
            data_result = matched ? "MATCH" : "CORRUPT";

            if (!matched) {
                note = "*** DATA CORRUPTION ***";
                ++failures_below_m;
            } else {
                note = (lost_count <= TEST_M)
                       ? "within M — expected"
                       : "beyond M — Wirehair overhead";
            }
        } else {
            status      = "FAILED";
            data_result = "N/A";

            if (lost_count <= TEST_M) {
                note = "*** UNEXPECTED — loss within M must always recover";
                ++failures_below_m;
            } else {
                note = "expected beyond threshold";
            }

            if (first_failure < 0) {
                first_failure = lost_count;
            }
        }

        printf("  %-12d  %-10s  %-16s  %s\n",
               lost_count, status, data_result, note);
    }

    /* ------------------------------------------------------------------ */
    /* Summary                                                             */
    /* ------------------------------------------------------------------ */
    printf("\n");
    printf("  ========================================\n");
    printf("  Summary\n");
    printf("  ========================================\n");
    printf("  K=%d  M=%d  first failure at L=%s",
           TEST_K, TEST_M,
           (first_failure < 0) ? "none in range\n" : "");

    if (first_failure >= 0) {
        printf("%d  (overhead=%d)\n",
               first_failure,
               first_failure - TEST_M);
    }

    printf("  Unexpected failures (L <= M): %d  %s\n",
           failures_below_m,
           (failures_below_m == 0) ? "(GOOD)" : "(BAD — FEC CORE BROKEN)");

    if (failures_below_m > 0) {
        printf("  RESULT: FAIL\n");
        printf("  ========================================\n\n");
        LOG_ERROR("[STRESS] FAILED (%d unexpected failure(s))",
                  failures_below_m);
        fec_destroy(handle);
        free(original_data);
        free(reconstructed_data);
        free(all_symbols);
        return -1;
    }

    printf("  RESULT: PASS\n");
    printf("  ========================================\n\n");
    LOG_INFO("=== Stress Test PASSED ===");

    fec_destroy(handle);
    free(original_data);
    free(reconstructed_data);
    free(all_symbols);
    return 0;

cleanup:
    fec_destroy(handle);
    free(original_data);
    free(reconstructed_data);
    free(all_symbols);
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                                 */
/* -------------------------------------------------------------------------- */

int main(void)
{
    log_init();

    if (wirehair_init() != Wirehair_Success) {
        fprintf(stderr, "[STRESS] Wirehair initialisation FAILED\n");
        return 1;
    }

    LOG_INFO("[STRESS] Wirehair initialised");

    if (fec_run_self_test() != 0) {
        LOG_ERROR("[STRESS] FEC self-test FAILED — aborting stress test");
        return 1;
    }

    return (run_stress_test() == 0) ? 0 : 1;
}