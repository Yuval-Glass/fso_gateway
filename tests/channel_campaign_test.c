/*
 * tests/channel_campaign_test.c — FSO Channel Campaign Test (Task 21).
 *
 * Runs many channel scenarios over the real TX→channel→RX pipeline via
 * sim_run_campaign_case(). All results are from real pipeline execution.
 *
 * Build:  make ctest
 * Output: build/channel_campaign_results.csv
 */

#define _POSIX_C_SOURCE 200112L

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "stats.h"
#include "sim_runner.h"

/* =========================================================================
 * Campaign geometry
 * =========================================================================*/

#define CAMP_K            16
#define CAMP_M             8
#define CAMP_DEPTH         8
#define CAMP_SYMBOL_SIZE 1500
#define CAMP_NUM_WINDOWS   32
#define CAMP_BASE_SEED     0x12345678U

#define CAMP_N          (CAMP_K + CAMP_M)
#define CAMP_TOTAL_SYMS (CAMP_NUM_WINDOWS * CAMP_DEPTH * CAMP_N)

#define CAMP_FEC_SPAN   (CAMP_DEPTH * CAMP_M)

/* 10 Gbps symbol time in microseconds. */
#define SYM_TIME_US     ((double)CAMP_SYMBOL_SIZE * 8.0 / (10.0 * 1000.0))

#define CAMP_BURST_START_DEFAULT 30
#define DEFAULT_REPEATS 8

/* =========================================================================
 * Scenario descriptor
 * =========================================================================*/

typedef struct {
    const char *group;
    const char *name;

    sim_config_t config;

    channel_event_t events[16];
    int             num_events;

    int      repeats;
    uint32_t base_seed;

    int contributes_to_contiguous_boundary;
} scenario_t;

/* =========================================================================
 * Scenario aggregate
 * =========================================================================*/

typedef struct {
    uint64_t runs;
    uint64_t pass_runs;
    uint64_t failed_runs;
    uint64_t pipeline_errors;

    uint64_t blocks_total_expected;
    uint64_t blocks_attempted_for_decode;
    uint64_t blocks_decode_success;
    uint64_t blocks_decode_failed;
    uint64_t blocks_discarded_before_decode;

    uint64_t blocks_discarded_timeout_before_decode;
    uint64_t blocks_discarded_too_many_holes_before_decode;
    uint64_t blocks_discarded_evicted_before_decode;
    uint64_t blocks_discarded_ready_evicted_before_mark;

    uint64_t packet_fail_missing;
    uint64_t packet_fail_corrupted;
    uint64_t packet_fail_due_to_missing_blocks;
    uint64_t packet_fail_after_successful_block_decode;

    uint64_t oracle_valid_runs;
    uint64_t oracle_invalid_runs;
    uint64_t oracle_blocks_theoretically_recoverable;
    uint64_t oracle_blocks_theoretically_unrecoverable;
    uint64_t oracle_recoverable_but_not_decoded_successfully;
    uint64_t oracle_unrecoverable_and_not_decoded_successfully;
    uint64_t oracle_recoverable_but_discarded_before_decode;
    uint64_t oracle_recoverable_but_decode_failed;

    uint64_t suspicious_runs_count;
    uint64_t suspicious_blocks_total;
    uint64_t suspicious_discarded_timeout_before_decode;
    uint64_t suspicious_discarded_too_many_holes_before_decode;
    uint64_t suspicious_discarded_evicted_before_decode;
    uint64_t suspicious_discarded_ready_evicted_before_mark;

    uint64_t worst_missing_symbols_seen;
    double   failed_blocks_sum_per_run;
    double   recovery_rate_sum;
    double   exact_match_rate_sum;

    int      contributes_to_contiguous_boundary;
    uint64_t characterization_burst_symbols;
} scenario_agg_t;

/* =========================================================================
 * Global scenario list
 * =========================================================================*/

#define MAX_SCENARIOS 512
static int        g_num_scenarios = 0;
static scenario_t g_scenarios[MAX_SCENARIOS];

/* =========================================================================
 * Helpers
 * =========================================================================*/

static int us_to_symbols(double us)
{
    double syms = us / SYM_TIME_US;
    int    n    = (int)ceil(syms);

    return (n < 1) ? 1 : n;
}

static sim_config_t default_cfg(void)
{
    sim_config_t c;

    memset(&c, 0, sizeof(c));
    c.k           = CAMP_K;
    c.m           = CAMP_M;
    c.depth       = CAMP_DEPTH;
    c.symbol_size = CAMP_SYMBOL_SIZE;
    c.num_windows = CAMP_NUM_WINDOWS;
    c.seed        = CAMP_BASE_SEED;

    return c;
}

static void add_scenario(const scenario_t *s)
{
    if (g_num_scenarios >= MAX_SCENARIOS) {
        return;
    }

    g_scenarios[g_num_scenarios++] = *s;
}

static int clamp_len(int start, int len)
{
    int available = CAMP_TOTAL_SYMS - start;

    if (available <= 0) {
        return 0;
    }

    return (len > available) ? available : len;
}

static uint64_t symbols_to_equiv_1500_packets(uint64_t symbols)
{
    uint64_t bytes = symbols * (uint64_t)CAMP_SYMBOL_SIZE;
    return (bytes + 1499U) / 1500U;
}

static int scenario_is_exactly_one_contiguous_erasure_event(const scenario_t *sc)
{
    return (sc->contributes_to_contiguous_boundary &&
            sc->num_events == 1 &&
            sc->events[0].type == CHANNEL_EVENT_ERASURE &&
            sc->events[0].length_symbols > 0);
}

static const char *log_level_name(log_level level)
{
    switch (level) {
        case DEBUG: return "DEBUG";
        case INFO:  return "INFO";
        case WARN:  return "WARN";
        case ERROR: return "ERROR";
        default:    return "UNKNOWN";
    }
}

static const char *snapshot_state_tag(int state)
{
    switch (state) {
        case 0: return "E";
        case 1: return "F";
        case 2: return "R";
        default: return "?";
    }
}

static const char *dominant_failure_reason_name(const scenario_agg_t *agg)
{
    uint64_t block_failures =
        agg->blocks_decode_failed + agg->blocks_discarded_before_decode;
    uint64_t packet_missing_blocks = agg->packet_fail_due_to_missing_blocks;
    uint64_t packet_after = agg->packet_fail_after_successful_block_decode;

    if (block_failures == 0U && packet_missing_blocks == 0U && packet_after == 0U) {
        return "none";
    }

    if (agg->blocks_discarded_before_decode >= agg->blocks_decode_failed &&
        agg->blocks_discarded_before_decode >= packet_missing_blocks &&
        agg->blocks_discarded_before_decode >= packet_after) {
        return "discarded_before_decode";
    }

    if (agg->blocks_decode_failed >= agg->blocks_discarded_before_decode &&
        agg->blocks_decode_failed >= packet_missing_blocks &&
        agg->blocks_decode_failed >= packet_after) {
        return "decode_failed";
    }

    if (packet_missing_blocks >= packet_after) {
        return "packet_fail_due_to_missing_blocks";
    }

    return "packet_fail_after_block_decode";
}

static void format_event_description(const sim_run_request_t *req,
                                     char                    *buf,
                                     size_t                   buf_sz)
{
    int    e;
    size_t used = 0U;

    if (buf == NULL || buf_sz == 0U) {
        return;
    }

    buf[0] = '\0';

    for (e = 0; e < req->num_events; ++e) {
        const channel_event_t *ev = &req->events[e];
        int written;

        written = snprintf(buf + used,
                           (used < buf_sz) ? (buf_sz - used) : 0U,
                           "%s%s:%d-%d(len=%d)",
                           (e == 0) ? "" : "|",
                           (ev->type == CHANNEL_EVENT_ERASURE) ? "E" : "C",
                           ev->start_symbol,
                           ev->start_symbol + ev->length_symbols - 1,
                           ev->length_symbols);

        if (written < 0) {
            break;
        }

        if ((size_t)written >= buf_sz - used) {
            used = buf_sz - 1U;
            break;
        }

        used += (size_t)written;
    }
}

static int parse_campaign_log_level(int argc, char **argv, log_level *level_out)
{
    int i;

    if (level_out == NULL) {
        return -1;
    }

    *level_out = WARN;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--quiet") == 0) {
            *level_out = WARN;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            *level_out = INFO;
        } else if (strcmp(argv[i], "--debug") == 0) {
            *level_out = DEBUG;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--quiet] [--verbose] [--debug]\n", argv[0]);
            printf("  --quiet   Default. Show campaign summaries plus warnings/errors.\n");
            printf("  --verbose Enable module INFO/WARN/ERROR logs.\n");
            printf("  --debug   Enable full DEBUG/INFO/WARN/ERROR logs.\n");
            return 1;
        } else {
            fprintf(stderr, "[campaign] ERROR: unknown argument '%s'\n", argv[i]);
            fprintf(stderr, "Usage: %s [--quiet] [--verbose] [--debug]\n", argv[0]);
            return -1;
        }
    }

    return 0;
}

static void print_suspicious_block_line(const char                   *scenario_group,
                                        const char                   *scenario_name,
                                        int                           run_idx,
                                        const sr_oracle_block_diag_t *diag)
{
    const char *final_reason;
    const char *evicted_before_decode;
    const char *timed_out;
    char        snapshot[512];
    size_t      used;
    uint32_t    i;

    if (diag == NULL) {
        return;
    }

    final_reason = sim_runner_block_final_reason_name(diag->final_reason);
    evicted_before_decode =
        (diag->discarded_evicted_before_decode ||
         diag->discarded_ready_evicted_before_mark) ? "YES" : "NO";
    timed_out =
        (diag->discarded_timeout_before_decode ||
         diag->discarded_too_many_holes_before_decode) ? "YES" : "NO";

    if (diag->has_eviction_trace) {
        used = (size_t)snprintf(snapshot, sizeof(snapshot), "[");
        if (used >= sizeof(snapshot)) {
            used = sizeof(snapshot) - 1U;
        }

        for (i = 0; i < diag->eviction_snapshot_count && used + 2U < sizeof(snapshot); ++i) {
            int written;

            written = snprintf(snapshot + used,
                               sizeof(snapshot) - used,
                               "%sidx%u:%c:block%" PRIu64 ":v%u",
                               (i == 0U) ? "" : "|",
                               diag->eviction_snapshot_indices[i],
                               diag->eviction_snapshot_states[i],
                               diag->eviction_snapshot_block_ids[i],
                               diag->eviction_snapshot_valid_symbols[i]);
            if (written < 0) {
                break;
            }
            if ((size_t)written >= (sizeof(snapshot) - used)) {
                used = sizeof(snapshot) - 1U;
                break;
            }
            used += (size_t)written;
        }

        if (used + 2U < sizeof(snapshot)) {
            snapshot[used++] = ']';
            snapshot[used] = '\0';
        } else {
            snapshot[sizeof(snapshot) - 2U] = ']';
            snapshot[sizeof(snapshot) - 1U] = '\0';
        }

        printf("[SUSPICIOUS BLOCK] "
               "scenario=%s/%s run=%d block=%" PRIu64
               " survived=%" PRIu64
               " missing=%" PRIu64
               " source=%" PRIu64
               " repair=%" PRIu64
               " oracle_recoverable=YES"
               " final_reason=%s"
               " evicted_before_decode=%s"
               " timed_out=%s"
               " eviction_incoming_block_id=%" PRIu64
               " eviction_slot_index=%u"
               " eviction_valid_symbols=%u"
               " eviction_holes=%u"
               " eviction_expected_symbols=%u"
               " eviction_active_blocks=%u"
               " eviction_max_active_blocks=%u"
               " snapshot=%s\n",
               scenario_group,
               scenario_name,
               run_idx,
               diag->block_id,
               diag->survived_symbols,
               diag->missing_symbols,
               diag->source_symbols_survived,
               diag->repair_symbols_survived,
               final_reason,
               evicted_before_decode,
               timed_out,
               diag->eviction_incoming_block_id,
               diag->eviction_slot_index,
               diag->eviction_valid_symbols,
               diag->eviction_holes,
               diag->eviction_expected_symbols,
               diag->eviction_active_blocks,
               diag->eviction_max_active_blocks,
               snapshot);
    } else {
        printf("[SUSPICIOUS BLOCK] "
               "scenario=%s/%s run=%d block=%" PRIu64
               " survived=%" PRIu64
               " missing=%" PRIu64
               " source=%" PRIu64
               " repair=%" PRIu64
               " oracle_recoverable=YES"
               " final_reason=%s"
               " evicted_before_decode=%s"
               " timed_out=%s"
               " eviction_trace=NONE\n",
               scenario_group,
               scenario_name,
               run_idx,
               diag->block_id,
               diag->survived_symbols,
               diag->missing_symbols,
               diag->source_symbols_survived,
               diag->repair_symbols_survived,
               final_reason,
               evicted_before_decode,
               timed_out);
    }
}

/* =========================================================================
 * Scenario builders
 * =========================================================================*/

static void build_very_short_bursts(void)
{
    static const int lens[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int n = (int)(sizeof(lens) / sizeof(lens[0]));
    int i;

    for (i = 0; i < n; ++i) {
        scenario_t s;
        static char names[10][32];

        memset(&s, 0, sizeof(s));
        s.group   = "A_VeryShort";
        snprintf(names[i], sizeof(names[i]), "burst_%d", lens[i]);
        s.name    = names[i];
        s.config  = default_cfg();
        s.repeats = DEFAULT_REPEATS;
        s.base_seed = CAMP_BASE_SEED + (uint32_t)(i * 7);
        s.contributes_to_contiguous_boundary = 1;

        s.events[0].type           = CHANNEL_EVENT_ERASURE;
        s.events[0].start_symbol   = CAMP_BURST_START_DEFAULT;
        s.events[0].length_symbols = clamp_len(CAMP_BURST_START_DEFAULT, lens[i]);
        s.num_events = 1;

        add_scenario(&s);
    }
}

static void build_single_burst_sweep(void)
{
    static const int lens[] = {
        16, 32, 48, 56, 60, 63, 64, 65,
        72, 80, 96, 128, 192, 256, 384, 512, 768, 1024
    };
    int n = (int)(sizeof(lens) / sizeof(lens[0]));
    int i;

    for (i = 0; i < n; ++i) {
        scenario_t s;
        static char names[18][32];

        memset(&s, 0, sizeof(s));
        s.group   = "B_SingleBurst";
        snprintf(names[i], sizeof(names[i]), "burst_%d", lens[i]);
        s.name    = names[i];
        s.config  = default_cfg();
        s.repeats = DEFAULT_REPEATS;
        s.base_seed = CAMP_BASE_SEED + 1000U + (uint32_t)(i * 13);
        s.contributes_to_contiguous_boundary = 1;

        s.events[0].type           = CHANNEL_EVENT_ERASURE;
        s.events[0].start_symbol   = CAMP_BURST_START_DEFAULT;
        s.events[0].length_symbols = clamp_len(CAMP_BURST_START_DEFAULT, lens[i]);
        s.num_events = 1;

        add_scenario(&s);
    }
}

static void build_boundary_bursts(void)
{
    static const int offsets[] = { -4, -2, -1, 0, 1, 2, 4, 8, 16 };
    int n = (int)(sizeof(offsets) / sizeof(offsets[0]));
    int i;

    for (i = 0; i < n; ++i) {
        scenario_t s;
        static char names[9][40];
        int len = CAMP_FEC_SPAN + offsets[i];

        if (len < 1) {
            len = 1;
        }

        memset(&s, 0, sizeof(s));
        s.group = "C_Boundary";
        snprintf(names[i], sizeof(names[i]), "span%+d_%d", offsets[i], len);
        s.name  = names[i];
        s.config = default_cfg();
        s.repeats = DEFAULT_REPEATS;
        s.base_seed = CAMP_BASE_SEED + 2000U + (uint32_t)(i * 11);
        s.contributes_to_contiguous_boundary = 1;

        s.events[0].type           = CHANNEL_EVENT_ERASURE;
        s.events[0].start_symbol   = CAMP_BURST_START_DEFAULT;
        s.events[0].length_symbols = clamp_len(CAMP_BURST_START_DEFAULT, len);
        s.num_events = 1;

        add_scenario(&s);
    }
}

static void build_time_fades(void)
{
    static const double fade_us[] = { 100.0, 250.0, 500.0, 1000.0, 2000.0 };
    static const char  *fade_names[] = { "100us", "250us", "500us", "1ms", "2ms" };
    int n = (int)(sizeof(fade_us) / sizeof(fade_us[0]));
    int i;

    for (i = 0; i < n; ++i) {
        scenario_t s;
        int sym_len = us_to_symbols(fade_us[i]);

        memset(&s, 0, sizeof(s));
        s.group   = "D_TimeFade";
        s.name    = fade_names[i];
        s.config  = default_cfg();
        s.repeats = DEFAULT_REPEATS;
        s.base_seed = CAMP_BASE_SEED + 3000U + (uint32_t)(i * 17);
        s.contributes_to_contiguous_boundary = 1;

        s.events[0].type           = CHANNEL_EVENT_ERASURE;
        s.events[0].start_symbol   = CAMP_BURST_START_DEFAULT;
        s.events[0].length_symbols = clamp_len(CAMP_BURST_START_DEFAULT, sym_len);
        s.num_events = 1;

        add_scenario(&s);
    }
}

static void build_multi_burst(void)
{
    {
        scenario_t s;
        memset(&s, 0, sizeof(s));
        s.group = "E_MultiBurst";
        s.name  = "2x16_sep100";
        s.config = default_cfg();
        s.repeats = DEFAULT_REPEATS;
        s.base_seed = CAMP_BASE_SEED + 4000U;
        s.events[0] = (channel_event_t){ CHANNEL_EVENT_ERASURE, 20, 16 };
        s.events[1] = (channel_event_t){ CHANNEL_EVENT_ERASURE, 140, 16 };
        s.num_events = 2;
        add_scenario(&s);
    }

    {
        scenario_t s;
        memset(&s, 0, sizeof(s));
        s.group = "E_MultiBurst";
        s.name  = "4x8_sep50";
        s.config = default_cfg();
        s.repeats = DEFAULT_REPEATS;
        s.base_seed = CAMP_BASE_SEED + 4100U;
        s.events[0] = (channel_event_t){ CHANNEL_EVENT_ERASURE, 20, 8 };
        s.events[1] = (channel_event_t){ CHANNEL_EVENT_ERASURE, 78, 8 };
        s.events[2] = (channel_event_t){ CHANNEL_EVENT_ERASURE, 136, 8 };
        s.events[3] = (channel_event_t){ CHANNEL_EVENT_ERASURE, 194, 8 };
        s.num_events = 4;
        add_scenario(&s);
    }

    {
        scenario_t s;
        int ei;

        memset(&s, 0, sizeof(s));
        s.group = "E_MultiBurst";
        s.name  = "10x4_sep30";
        s.config = default_cfg();
        s.repeats = DEFAULT_REPEATS;
        s.base_seed = CAMP_BASE_SEED + 4200U;
        s.num_events = 10;

        for (ei = 0; ei < 10; ++ei) {
            s.events[ei].type           = CHANNEL_EVENT_ERASURE;
            s.events[ei].start_symbol   = 20 + ei * 34;
            s.events[ei].length_symbols = 4;
        }

        add_scenario(&s);
    }

    {
        scenario_t s;
        memset(&s, 0, sizeof(s));
        s.group = "E_MultiBurst";
        s.name  = "2x32_nearoverlap";
        s.config = default_cfg();
        s.repeats = DEFAULT_REPEATS;
        s.base_seed = CAMP_BASE_SEED + 4300U;
        s.events[0] = (channel_event_t){ CHANNEL_EVENT_ERASURE, 20, 32 };
        s.events[1] = (channel_event_t){ CHANNEL_EVENT_ERASURE, 60, 32 };
        s.num_events = 2;
        add_scenario(&s);
    }

    {
        scenario_t s;
        int ei;

        memset(&s, 0, sizeof(s));
        s.group = "E_MultiBurst";
        s.name  = "8x16_spread";
        s.config = default_cfg();
        s.repeats = DEFAULT_REPEATS;
        s.base_seed = CAMP_BASE_SEED + 4400U;
        s.num_events = 8;

        for (ei = 0; ei < 8; ++ei) {
            s.events[ei].type           = CHANNEL_EVENT_ERASURE;
            s.events[ei].start_symbol   = 20 + ei * 700;
            s.events[ei].length_symbols = clamp_len(20 + ei * 700, 16);
        }

        add_scenario(&s);
    }
}

static void build_blackout(void)
{
    static const int lens[] = { 192, 384, 768, 1536, 3072 };
    static const char *names[] = {
        "blackout_192", "blackout_384", "blackout_768",
        "blackout_1536", "blackout_3072"
    };
    int n = (int)(sizeof(lens) / sizeof(lens[0]));
    int i;

    for (i = 0; i < n; ++i) {
        scenario_t s;

        memset(&s, 0, sizeof(s));
        s.group = "F_Blackout";
        s.name  = names[i];
        s.config = default_cfg();
        s.repeats = 5;
        s.base_seed = CAMP_BASE_SEED + 5000U + (uint32_t)(i * 23);

        s.events[0].type           = CHANNEL_EVENT_ERASURE;
        s.events[0].start_symbol   = CAMP_BURST_START_DEFAULT;
        s.events[0].length_symbols = clamp_len(CAMP_BURST_START_DEFAULT, lens[i]);
        s.num_events = 1;

        add_scenario(&s);
    }
}

static void build_corruption_only(void)
{
    static const int lens[] = { 4, 8, 16, 32, 64 };
    static const char *names[] = {
        "corrupt_4", "corrupt_8", "corrupt_16", "corrupt_32", "corrupt_64"
    };
    int n = (int)(sizeof(lens) / sizeof(lens[0]));
    int i;

    for (i = 0; i < n; ++i) {
        scenario_t s;

        memset(&s, 0, sizeof(s));
        s.group = "G_Corruption";
        s.name  = names[i];
        s.config = default_cfg();
        s.repeats = DEFAULT_REPEATS;
        s.base_seed = CAMP_BASE_SEED + 6000U + (uint32_t)(i * 19);

        s.events[0].type           = CHANNEL_EVENT_CORRUPTION;
        s.events[0].start_symbol   = CAMP_BURST_START_DEFAULT;
        s.events[0].length_symbols = clamp_len(CAMP_BURST_START_DEFAULT, lens[i]);
        s.num_events = 1;

        add_scenario(&s);
    }

    {
        scenario_t s;

        memset(&s, 0, sizeof(s));
        s.group = "G_Corruption";
        s.name  = "2x16_corrupt_sep100";
        s.config = default_cfg();
        s.repeats = DEFAULT_REPEATS;
        s.base_seed = CAMP_BASE_SEED + 6500U;
        s.events[0] = (channel_event_t){ CHANNEL_EVENT_CORRUPTION, 20, 16 };
        s.events[1] = (channel_event_t){ CHANNEL_EVENT_CORRUPTION, 140, 16 };
        s.num_events = 2;

        add_scenario(&s);
    }
}

static void build_mixed(void)
{
    {
        scenario_t s;

        memset(&s, 0, sizeof(s));
        s.group = "H_Mixed";
        s.name  = "erase8_corrupt8_same_win";
        s.config = default_cfg();
        s.repeats = DEFAULT_REPEATS;
        s.base_seed = CAMP_BASE_SEED + 7000U;
        s.events[0] = (channel_event_t){ CHANNEL_EVENT_ERASURE, 20, 8 };
        s.events[1] = (channel_event_t){ CHANNEL_EVENT_CORRUPTION, 30, 8 };
        s.num_events = 2;

        add_scenario(&s);
    }

    {
        scenario_t s;

        memset(&s, 0, sizeof(s));
        s.group = "H_Mixed";
        s.name  = "erase64_corrupt4";
        s.config = default_cfg();
        s.repeats = DEFAULT_REPEATS;
        s.base_seed = CAMP_BASE_SEED + 7100U;
        s.events[0] = (channel_event_t){ CHANNEL_EVENT_ERASURE, 20, 64 };
        s.events[1] = (channel_event_t){ CHANNEL_EVENT_CORRUPTION, 200, 4 };
        s.num_events = 2;

        add_scenario(&s);
    }

    {
        scenario_t s;

        memset(&s, 0, sizeof(s));
        s.group = "H_Mixed";
        s.name  = "erase16_corrupt16_erase16";
        s.config = default_cfg();
        s.repeats = DEFAULT_REPEATS;
        s.base_seed = CAMP_BASE_SEED + 7200U;
        s.events[0] = (channel_event_t){ CHANNEL_EVENT_ERASURE, 20, 16 };
        s.events[1] = (channel_event_t){ CHANNEL_EVENT_CORRUPTION, 80, 16 };
        s.events[2] = (channel_event_t){ CHANNEL_EVENT_ERASURE, 150, 16 };
        s.num_events = 3;

        add_scenario(&s);
    }

    {
        scenario_t s;

        memset(&s, 0, sizeof(s));
        s.group = "H_Mixed";
        s.name  = "heavy_mixed";
        s.config = default_cfg();
        s.repeats = 5;
        s.base_seed = CAMP_BASE_SEED + 7300U;
        s.events[0] = (channel_event_t){ CHANNEL_EVENT_ERASURE, 20, 32 };
        s.events[1] = (channel_event_t){ CHANNEL_EVENT_CORRUPTION, 70, 16 };
        s.events[2] = (channel_event_t){ CHANNEL_EVENT_ERASURE, 200, 32 };
        s.events[3] = (channel_event_t){ CHANNEL_EVENT_CORRUPTION, 400, 16 };
        s.num_events = 4;

        add_scenario(&s);
    }
}

static void build_failure_edge(void)
{
    static const int overshoot[] = { 1, 2, 4, 8, 16, 32, 64 };
    int n = (int)(sizeof(overshoot) / sizeof(overshoot[0]));
    int i;

    for (i = 0; i < n; ++i) {
        scenario_t s;
        static char names[7][40];
        int len = CAMP_FEC_SPAN + overshoot[i];

        memset(&s, 0, sizeof(s));
        s.group = "I_FailureEdge";
        snprintf(names[i], sizeof(names[i]), "span_plus%d_%d", overshoot[i], len);
        s.name  = names[i];
        s.config = default_cfg();
        s.repeats = DEFAULT_REPEATS;
        s.base_seed = CAMP_BASE_SEED + 8000U + (uint32_t)(i * 29);
        s.contributes_to_contiguous_boundary = 1;

        s.events[0].type           = CHANNEL_EVENT_ERASURE;
        s.events[0].start_symbol   = CAMP_BURST_START_DEFAULT;
        s.events[0].length_symbols = clamp_len(CAMP_BURST_START_DEFAULT, len);
        s.num_events = 1;

        add_scenario(&s);
    }
}

/* =========================================================================
 * CSV output
 * =========================================================================*/

static void csv_write_header(FILE *f)
{
    fprintf(f,
            "group,name,repeat,seed,"
            "event_description,"
            "transmitted_packets,recovered_packets,exact_match_packets,"
            "blocks_total_expected,blocks_attempted_for_decode,blocks_decode_success,blocks_decode_failed,"
            "blocks_discarded_before_decode,blocks_discarded_timeout_before_decode,"
            "blocks_discarded_too_many_holes_before_decode,blocks_discarded_evicted_before_decode,"
            "blocks_discarded_ready_evicted_before_mark,"
            "packet_fail_missing,packet_fail_corrupted,packet_fail_due_to_missing_blocks,"
            "packet_fail_after_successful_block_decode,"
            "oracle_valid_for_run,"
            "oracle_blocks_theoretically_recoverable,oracle_blocks_theoretically_unrecoverable,"
            "oracle_recoverable_but_not_decoded_successfully,"
            "oracle_unrecoverable_and_not_decoded_successfully,"
            "oracle_recoverable_but_discarded_before_decode,oracle_recoverable_but_decode_failed,"
            "suspicious_blocks_in_run,"
            "worst_missing_symbols_seen,pass_fail\n");
}

static void csv_write_row(FILE                     *f,
                          const char               *group,
                          const char               *name,
                          int                       repeat,
                          uint32_t                  seed,
                          const sim_run_request_t  *req,
                          const sim_result_t       *r,
                          const sr_run_report_t    *rr,
                          uint64_t                  suspicious_blocks_in_run)
{
    char event_desc[256];
    int  pass;

    format_event_description(req, event_desc, sizeof(event_desc));
    pass = (r->missing_packets == 0U && r->corrupted_packets == 0U) ? 1 : 0;

    fprintf(f,
            "%s,%s,%d,%u,"
            "\"%s\","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%s,"
            "%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ",%s\n",
            group,
            name,
            repeat,
            seed,
            event_desc,
            r->transmitted_packets,
            r->recovered_packets,
            r->exact_match_packets,
            rr->blocks_total_expected,
            rr->blocks_attempted_for_decode,
            rr->blocks_decode_success,
            rr->blocks_decode_failed,
            rr->blocks_discarded_before_decode,
            rr->blocks_discarded_timeout_before_decode,
            rr->blocks_discarded_too_many_holes_before_decode,
            rr->blocks_discarded_evicted_before_decode,
            rr->blocks_discarded_ready_evicted_before_mark,
            rr->packet_fail_missing,
            rr->packet_fail_corrupted,
            rr->packet_fail_due_to_missing_blocks,
            rr->packet_fail_after_successful_block_decode,
            rr->oracle_valid_for_run ? "true" : "false",
            rr->oracle_blocks_theoretically_recoverable,
            rr->oracle_blocks_theoretically_unrecoverable,
            rr->oracle_recoverable_but_not_decoded_successfully,
            rr->oracle_unrecoverable_and_not_decoded_successfully,
            rr->oracle_recoverable_but_discarded_before_decode,
            rr->oracle_recoverable_but_decode_failed,
            suspicious_blocks_in_run,
            rr->max_missing_symbols_in_block,
            pass ? "PASS" : "FAIL");
}

/* =========================================================================
 * Reporting
 * =========================================================================*/

static void print_run_line(const char               *group,
                           const char               *name,
                           int                       repeat,
                           const sim_result_t       *r,
                           const sr_run_report_t    *rr,
                           uint64_t                  suspicious_blocks_in_run)
{
    const char *status =
        (r->missing_packets == 0U && r->corrupted_packets == 0U) ? "PASS" : "FAIL";

    printf("  [%s/%s] rep=%d status=%s "
           "pkts=%" PRIu64 "/%" PRIu64 " "
           "decode=%" PRIu64 "/%" PRIu64 " "
           "discarded=%" PRIu64 " packet_missing_blocks=%" PRIu64 " packet_after=%" PRIu64
           " oracle_valid=%s suspicious_blocks=%" PRIu64
           " oracle_recov_discarded=%" PRIu64
           " worst_missing=%" PRIu64 "\n",
           group,
           name,
           repeat,
           status,
           r->recovered_packets,
           r->transmitted_packets,
           rr->blocks_decode_success,
           rr->blocks_attempted_for_decode,
           rr->blocks_discarded_before_decode,
           rr->packet_fail_due_to_missing_blocks,
           rr->packet_fail_after_successful_block_decode,
           rr->oracle_valid_for_run ? "true" : "false",
           suspicious_blocks_in_run,
           rr->oracle_recoverable_but_discarded_before_decode,
           rr->max_missing_symbols_in_block);
}

static void print_run_summary(const sr_run_report_t *rr,
                              uint64_t               suspicious_blocks_in_run)
{
    printf("    [RUN SUMMARY]\n");
    printf("      blocks_total_expected=%" PRIu64 "\n", rr->blocks_total_expected);
    printf("      blocks_attempted_for_decode=%" PRIu64 "\n", rr->blocks_attempted_for_decode);
    printf("      blocks_decode_success=%" PRIu64 "\n", rr->blocks_decode_success);
    printf("      blocks_decode_failed=%" PRIu64 "\n", rr->blocks_decode_failed);
    printf("      blocks_discarded_before_decode=%" PRIu64 "\n", rr->blocks_discarded_before_decode);
    printf("      blocks_discarded_timeout_before_decode=%" PRIu64 "\n",
           rr->blocks_discarded_timeout_before_decode);
    printf("      blocks_discarded_too_many_holes_before_decode=%" PRIu64 "\n",
           rr->blocks_discarded_too_many_holes_before_decode);
    printf("      blocks_discarded_evicted_before_decode=%" PRIu64 "\n",
           rr->blocks_discarded_evicted_before_decode);
    printf("      blocks_discarded_ready_evicted_before_mark=%" PRIu64 "\n",
           rr->blocks_discarded_ready_evicted_before_mark);
    printf("      packet_fail_missing=%" PRIu64 "\n", rr->packet_fail_missing);
    printf("      packet_fail_corrupted=%" PRIu64 "\n", rr->packet_fail_corrupted);
    printf("      packet_fail_due_to_missing_blocks=%" PRIu64 "\n",
           rr->packet_fail_due_to_missing_blocks);
    printf("      packet_fail_after_successful_block_decode=%" PRIu64 "\n",
           rr->packet_fail_after_successful_block_decode);
    printf("      max_missing_symbols_in_block=%" PRIu64 "\n",
           rr->max_missing_symbols_in_block);
    printf("      avg_missing_symbols_in_failed_blocks=%.2f\n",
           rr->avg_missing_symbols_in_failed_blocks);
    printf("      oracle_valid_for_run=%s\n",
           rr->oracle_valid_for_run ? "true" : "false");
    printf("      oracle_blocks_theoretically_recoverable=%" PRIu64 "\n",
           rr->oracle_blocks_theoretically_recoverable);
    printf("      oracle_blocks_theoretically_unrecoverable=%" PRIu64 "\n",
           rr->oracle_blocks_theoretically_unrecoverable);
    printf("      oracle_recoverable_but_not_decoded_successfully=%" PRIu64 "\n",
           rr->oracle_recoverable_but_not_decoded_successfully);
    printf("      oracle_unrecoverable_and_not_decoded_successfully=%" PRIu64 "\n",
           rr->oracle_unrecoverable_and_not_decoded_successfully);
    printf("      oracle_recoverable_but_discarded_before_decode=%" PRIu64 "\n",
           rr->oracle_recoverable_but_discarded_before_decode);
    printf("      oracle_recoverable_but_decode_failed=%" PRIu64 "\n",
           rr->oracle_recoverable_but_decode_failed);
    printf("      suspicious_blocks_in_run=%" PRIu64 "\n", suspicious_blocks_in_run);
}

static void print_scenario_summary(const scenario_t     *sc,
                                   const scenario_agg_t *agg)
{
    printf("  [SCENARIO SUMMARY] %s/%s\n", sc->group, sc->name);
    printf("    runs=%" PRIu64 "\n", agg->runs);
    printf("    pass_runs=%" PRIu64 "\n", agg->pass_runs);
    printf("    failed_runs=%" PRIu64 "\n", agg->failed_runs);
    printf("    total_decode_failed_blocks=%" PRIu64 "\n", agg->blocks_decode_failed);
    printf("    total_discarded_before_decode=%" PRIu64 "\n", agg->blocks_discarded_before_decode);
    printf("    avg_failed_blocks_per_run=%.2f\n",
           (agg->runs > 0U) ? (agg->failed_blocks_sum_per_run / (double)agg->runs) : 0.0);
    printf("    dominant_failure_reason=%s\n", dominant_failure_reason_name(agg));
    printf("    oracle_valid_runs=%" PRIu64 "\n", agg->oracle_valid_runs);
    printf("    oracle_invalid_runs=%" PRIu64 "\n", agg->oracle_invalid_runs);
    printf("    oracle_blocks_theoretically_recoverable=%" PRIu64 "\n",
           agg->oracle_blocks_theoretically_recoverable);
    printf("    oracle_blocks_theoretically_unrecoverable=%" PRIu64 "\n",
           agg->oracle_blocks_theoretically_unrecoverable);
    printf("    oracle_recoverable_but_not_decoded_successfully=%" PRIu64 "\n",
           agg->oracle_recoverable_but_not_decoded_successfully);
    printf("    oracle_unrecoverable_and_not_decoded_successfully=%" PRIu64 "\n",
           agg->oracle_unrecoverable_and_not_decoded_successfully);
    printf("    oracle_recoverable_but_discarded_before_decode=%" PRIu64 "\n",
           agg->oracle_recoverable_but_discarded_before_decode);
    printf("    oracle_recoverable_but_decode_failed=%" PRIu64 "\n",
           agg->oracle_recoverable_but_decode_failed);
    printf("    suspicious_runs_count=%" PRIu64 "\n", agg->suspicious_runs_count);
    printf("    suspicious_blocks_total=%" PRIu64 "\n", agg->suspicious_blocks_total);
    printf("    suspicious_discarded_timeout_before_decode=%" PRIu64 "\n",
           agg->suspicious_discarded_timeout_before_decode);
    printf("    suspicious_discarded_too_many_holes_before_decode=%" PRIu64 "\n",
           agg->suspicious_discarded_too_many_holes_before_decode);
    printf("    suspicious_discarded_evicted_before_decode=%" PRIu64 "\n",
           agg->suspicious_discarded_evicted_before_decode);
    printf("    suspicious_discarded_ready_evicted_before_mark=%" PRIu64 "\n",
           agg->suspicious_discarded_ready_evicted_before_mark);
    printf("    worst_missing_symbols_seen=%" PRIu64 "\n", agg->worst_missing_symbols_seen);
}

static void print_campaign_summary(const scenario_agg_t *aggs, int n)
{
    uint64_t total_scenarios                     = (uint64_t)n;
    uint64_t total_runs                          = 0U;
    uint64_t run_pass                            = 0U;
    uint64_t run_fail                            = 0U;
    uint64_t blocks_total_expected               = 0U;
    uint64_t blocks_attempted_for_decode         = 0U;
    uint64_t blocks_decode_success               = 0U;
    uint64_t blocks_decode_failed                = 0U;
    uint64_t blocks_discarded_before_decode      = 0U;
    uint64_t blocks_discarded_timeout_before_decode = 0U;
    uint64_t blocks_discarded_too_many_holes_before_decode = 0U;
    uint64_t blocks_discarded_evicted_before_decode = 0U;
    uint64_t blocks_discarded_ready_evicted_before_mark = 0U;
    uint64_t packet_fail_due_to_missing_blocks   = 0U;
    uint64_t packet_fail_after_successful_decode = 0U;
    uint64_t oracle_valid_runs                   = 0U;
    uint64_t oracle_invalid_runs                 = 0U;
    uint64_t oracle_blocks_theoretically_recoverable = 0U;
    uint64_t oracle_blocks_theoretically_unrecoverable = 0U;
    uint64_t oracle_recoverable_but_not_decoded_successfully = 0U;
    uint64_t oracle_unrecoverable_and_not_decoded_successfully = 0U;
    uint64_t oracle_recoverable_but_discarded_before_decode = 0U;
    uint64_t oracle_recoverable_but_decode_failed = 0U;
    uint64_t suspicious_runs_count               = 0U;
    uint64_t suspicious_blocks_total             = 0U;
    uint64_t suspicious_discarded_timeout_before_decode = 0U;
    uint64_t suspicious_discarded_too_many_holes_before_decode = 0U;
    uint64_t suspicious_discarded_evicted_before_decode = 0U;
    uint64_t suspicious_discarded_ready_evicted_before_mark = 0U;
    uint64_t longest_recoverable_burst           = 0U;
    uint64_t first_failing_burst                 = 0U;
    uint64_t worst_missing_symbols_seen          = 0U;
    double   failed_blocks_weighted_sum          = 0.0;
    uint64_t failed_blocks_weighted_count        = 0U;
    int      have_longest                        = 0;
    int      have_first_fail                     = 0;
    int      i;

    for (i = 0; i < n; ++i) {
        const scenario_agg_t *a = &aggs[i];

        total_runs                          += a->runs;
        run_pass                            += a->pass_runs;
        run_fail                            += a->failed_runs;
        blocks_total_expected               += a->blocks_total_expected;
        blocks_attempted_for_decode         += a->blocks_attempted_for_decode;
        blocks_decode_success               += a->blocks_decode_success;
        blocks_decode_failed                += a->blocks_decode_failed;
        blocks_discarded_before_decode      += a->blocks_discarded_before_decode;
        blocks_discarded_timeout_before_decode += a->blocks_discarded_timeout_before_decode;
        blocks_discarded_too_many_holes_before_decode += a->blocks_discarded_too_many_holes_before_decode;
        blocks_discarded_evicted_before_decode += a->blocks_discarded_evicted_before_decode;
        blocks_discarded_ready_evicted_before_mark += a->blocks_discarded_ready_evicted_before_mark;
        packet_fail_due_to_missing_blocks   += a->packet_fail_due_to_missing_blocks;
        packet_fail_after_successful_decode += a->packet_fail_after_successful_block_decode;
        oracle_valid_runs                   += a->oracle_valid_runs;
        oracle_invalid_runs                 += a->oracle_invalid_runs;
        oracle_blocks_theoretically_recoverable += a->oracle_blocks_theoretically_recoverable;
        oracle_blocks_theoretically_unrecoverable += a->oracle_blocks_theoretically_unrecoverable;
        oracle_recoverable_but_not_decoded_successfully +=
            a->oracle_recoverable_but_not_decoded_successfully;
        oracle_unrecoverable_and_not_decoded_successfully +=
            a->oracle_unrecoverable_and_not_decoded_successfully;
        oracle_recoverable_but_discarded_before_decode +=
            a->oracle_recoverable_but_discarded_before_decode;
        oracle_recoverable_but_decode_failed +=
            a->oracle_recoverable_but_decode_failed;
        suspicious_runs_count += a->suspicious_runs_count;
        suspicious_blocks_total += a->suspicious_blocks_total;
        suspicious_discarded_timeout_before_decode +=
            a->suspicious_discarded_timeout_before_decode;
        suspicious_discarded_too_many_holes_before_decode +=
            a->suspicious_discarded_too_many_holes_before_decode;
        suspicious_discarded_evicted_before_decode +=
            a->suspicious_discarded_evicted_before_decode;
        suspicious_discarded_ready_evicted_before_mark +=
            a->suspicious_discarded_ready_evicted_before_mark;

        if (a->worst_missing_symbols_seen > worst_missing_symbols_seen) {
            worst_missing_symbols_seen = a->worst_missing_symbols_seen;
        }

        failed_blocks_weighted_sum   += a->failed_blocks_sum_per_run;
        failed_blocks_weighted_count += a->runs;

        if (a->contributes_to_contiguous_boundary && a->runs > 0U) {
            if (a->failed_runs == 0U) {
                if (!have_longest || a->characterization_burst_symbols > longest_recoverable_burst) {
                    longest_recoverable_burst = a->characterization_burst_symbols;
                    have_longest = 1;
                }
            } else {
                if (!have_first_fail || a->characterization_burst_symbols < first_failing_burst) {
                    first_failing_burst = a->characterization_burst_symbols;
                    have_first_fail = 1;
                }
            }
        }
    }

    printf("\n=== CAMPAIGN ANALYSIS SUMMARY ===\n");
    printf("- total_scenarios: %" PRIu64 "\n", total_scenarios);
    printf("- total_runs: %" PRIu64 "\n", total_runs);
    printf("- run_pass: %" PRIu64 "\n", run_pass);
    printf("- run_fail: %" PRIu64 "\n", run_fail);
    printf("- blocks_total_expected: %" PRIu64 "\n", blocks_total_expected);
    printf("- blocks_attempted_for_decode: %" PRIu64 "\n", blocks_attempted_for_decode);
    printf("- blocks_decode_success: %" PRIu64 "\n", blocks_decode_success);
    printf("- blocks_decode_failed: %" PRIu64 "\n", blocks_decode_failed);
    printf("- blocks_discarded_before_decode: %" PRIu64 "\n", blocks_discarded_before_decode);
    printf("Failure breakdown:\n");
    printf("Discarded-before-decode:\n");
    printf("- timeout_before_decode: %" PRIu64 "\n", blocks_discarded_timeout_before_decode);
    printf("- too_many_holes_before_decode: %" PRIu64 "\n", blocks_discarded_too_many_holes_before_decode);
    printf("- evicted_before_decode: %" PRIu64 "\n", blocks_discarded_evicted_before_decode);
    printf("- ready_evicted_before_mark: %" PRIu64 "\n", blocks_discarded_ready_evicted_before_mark);
    printf("Decode-level:\n");
    printf("- decode_failed: %" PRIu64 "\n", blocks_decode_failed);
    printf("Packet-level:\n");
    printf("- packet_fail_due_to_missing_blocks: %" PRIu64 "\n",
           packet_fail_due_to_missing_blocks);
    printf("- packet_fail_after_successful_block_decode: %" PRIu64 "\n",
           packet_fail_after_successful_decode);
    printf("Erasure oracle diagnostics:\n");
    printf("- oracle_valid_runs: %" PRIu64 "\n", oracle_valid_runs);
    printf("- oracle_invalid_runs: %" PRIu64 "\n", oracle_invalid_runs);
    printf("- oracle_blocks_theoretically_recoverable: %" PRIu64 "\n",
           oracle_blocks_theoretically_recoverable);
    printf("- oracle_blocks_theoretically_unrecoverable: %" PRIu64 "\n",
           oracle_blocks_theoretically_unrecoverable);
    printf("- oracle_recoverable_but_not_decoded_successfully: %" PRIu64 "\n",
           oracle_recoverable_but_not_decoded_successfully);
    printf("- oracle_unrecoverable_and_not_decoded_successfully: %" PRIu64 "\n",
           oracle_unrecoverable_and_not_decoded_successfully);
    printf("- oracle_recoverable_but_discarded_before_decode: %" PRIu64 "\n",
           oracle_recoverable_but_discarded_before_decode);
    printf("- oracle_recoverable_but_decode_failed: %" PRIu64 "\n",
           oracle_recoverable_but_decode_failed);
    printf("Suspicious discard pinpoint summary:\n");
    printf("- suspicious_runs_count: %" PRIu64 "\n", suspicious_runs_count);
    printf("- suspicious_blocks_total: %" PRIu64 "\n", suspicious_blocks_total);
    printf("- suspicious_discarded_timeout_before_decode: %" PRIu64 "\n",
           suspicious_discarded_timeout_before_decode);
    printf("- suspicious_discarded_too_many_holes_before_decode: %" PRIu64 "\n",
           suspicious_discarded_too_many_holes_before_decode);
    printf("- suspicious_discarded_evicted_before_decode: %" PRIu64 "\n",
           suspicious_discarded_evicted_before_decode);
    printf("- suspicious_discarded_ready_evicted_before_mark: %" PRIu64 "\n",
           suspicious_discarded_ready_evicted_before_mark);
    printf("Burst characterization:\n");
    printf("- longest_recoverable_burst: ");
    if (have_longest) {
        printf("%" PRIu64 " symbols | %.2f us | %" PRIu64 " x 1500B-packets\n",
               longest_recoverable_burst,
               (double)longest_recoverable_burst * SYM_TIME_US,
               symbols_to_equiv_1500_packets(longest_recoverable_burst));
    } else {
        printf("none\n");
    }
    printf("- first_failing_burst: ");
    if (have_first_fail) {
        printf("%" PRIu64 " symbols | %.2f us | %" PRIu64 " x 1500B-packets\n",
               first_failing_burst,
               (double)first_failing_burst * SYM_TIME_US,
               symbols_to_equiv_1500_packets(first_failing_burst));
    } else {
        printf("none\n");
    }
    printf("Decoder stress:\n");
    printf("- worst_missing_symbols_seen: %" PRIu64 "\n", worst_missing_symbols_seen);
    printf("- average_missing_symbols_on_failed_blocks: %.2f\n",
           (failed_blocks_weighted_count > 0U)
               ? (failed_blocks_weighted_sum / (double)failed_blocks_weighted_count)
               : 0.0);
}

/* =========================================================================
 * Run one scenario
 * =========================================================================*/

static void run_scenario(const scenario_t *sc,
                         FILE             *csv,
                         scenario_agg_t   *agg)
{
    int r;

    memset(agg, 0, sizeof(*agg));
    agg->contributes_to_contiguous_boundary = scenario_is_exactly_one_contiguous_erasure_event(sc);
    agg->characterization_burst_symbols =
        (agg->contributes_to_contiguous_boundary != 0)
            ? (uint64_t)sc->events[0].length_symbols
            : 0U;

    printf("\n[%s/%s] %d repeat(s)\n", sc->group, sc->name, sc->repeats);

    for (r = 0; r < sc->repeats; ++r) {
        sim_run_request_t               req;
        sim_result_t                    result;
        channel_apply_result_t          ch;
        sr_run_report_t                 report;
        const sr_oracle_block_diag_t   *oracle_diags = NULL;
        size_t                          oracle_diag_count = 0U;
        uint64_t                        suspicious_blocks_in_run = 0U;
        uint32_t                        seed = sc->base_seed + (uint32_t)r;
        size_t                          bi;

        memset(&req, 0, sizeof(req));
        memcpy(req.events,
               sc->events,
               sizeof(channel_event_t) * (size_t)sc->num_events);
        req.num_events    = sc->num_events;
        req.seed_override = seed;

        if (sim_run_campaign_case(&sc->config, &req, &result, &ch) != 0) {
            fprintf(stderr,
                    "  [ERROR] pipeline failure: %s/%s rep=%d\n",
                    sc->group,
                    sc->name,
                    r);
            agg->pipeline_errors++;
            continue;
        }

        memset(&report, 0, sizeof(report));
        sim_runner_get_last_run_report(&report);
        sim_runner_get_last_oracle_block_diags(&oracle_diags, &oracle_diag_count);

        if (oracle_diags != NULL) {
            for (bi = 0; bi < oracle_diag_count; ++bi) {
                const sr_oracle_block_diag_t *diag = &oracle_diags[bi];

                if (!diag->suspicious_oracle_recoverable_but_discarded_before_decode ||
                    !diag->discarded_evicted_before_decode) {
                    continue;
                }

                suspicious_blocks_in_run++;
                print_suspicious_block_line(sc->group, sc->name, r, diag);

                if (diag->discarded_timeout_before_decode) {
                    agg->suspicious_discarded_timeout_before_decode++;
                }
                if (diag->discarded_too_many_holes_before_decode) {
                    agg->suspicious_discarded_too_many_holes_before_decode++;
                }
                if (diag->discarded_evicted_before_decode) {
                    agg->suspicious_discarded_evicted_before_decode++;
                }
                if (diag->discarded_ready_evicted_before_mark) {
                    agg->suspicious_discarded_ready_evicted_before_mark++;
                }
            }
        }

        print_run_line(sc->group, sc->name, r, &result, &report, suspicious_blocks_in_run);
        print_run_summary(&report, suspicious_blocks_in_run);
        csv_write_row(csv,
                      sc->group,
                      sc->name,
                      r,
                      seed,
                      &req,
                      &result,
                      &report,
                      suspicious_blocks_in_run);

        agg->runs++;
        agg->blocks_total_expected               += report.blocks_total_expected;
        agg->blocks_attempted_for_decode         += report.blocks_attempted_for_decode;
        agg->blocks_decode_success               += report.blocks_decode_success;
        agg->blocks_decode_failed                += report.blocks_decode_failed;
        agg->blocks_discarded_before_decode      += report.blocks_discarded_before_decode;
        agg->blocks_discarded_timeout_before_decode += report.blocks_discarded_timeout_before_decode;
        agg->blocks_discarded_too_many_holes_before_decode += report.blocks_discarded_too_many_holes_before_decode;
        agg->blocks_discarded_evicted_before_decode += report.blocks_discarded_evicted_before_decode;
        agg->blocks_discarded_ready_evicted_before_mark += report.blocks_discarded_ready_evicted_before_mark;
        agg->packet_fail_missing                 += report.packet_fail_missing;
        agg->packet_fail_corrupted               += report.packet_fail_corrupted;
        agg->packet_fail_due_to_missing_blocks   += report.packet_fail_due_to_missing_blocks;
        agg->packet_fail_after_successful_block_decode += report.packet_fail_after_successful_block_decode;

        if (report.oracle_valid_for_run) {
            agg->oracle_valid_runs++;
        } else {
            agg->oracle_invalid_runs++;
        }

        agg->oracle_blocks_theoretically_recoverable +=
            report.oracle_blocks_theoretically_recoverable;
        agg->oracle_blocks_theoretically_unrecoverable +=
            report.oracle_blocks_theoretically_unrecoverable;
        agg->oracle_recoverable_but_not_decoded_successfully +=
            report.oracle_recoverable_but_not_decoded_successfully;
        agg->oracle_unrecoverable_and_not_decoded_successfully +=
            report.oracle_unrecoverable_and_not_decoded_successfully;
        agg->oracle_recoverable_but_discarded_before_decode +=
            report.oracle_recoverable_but_discarded_before_decode;
        agg->oracle_recoverable_but_decode_failed +=
            report.oracle_recoverable_but_decode_failed;
        agg->failed_blocks_sum_per_run           += (double)report.blocks_decode_failed;
        agg->recovery_rate_sum                   += result.recovery_rate;
        agg->exact_match_rate_sum                += result.exact_match_rate;

        if (report.max_missing_symbols_in_block > agg->worst_missing_symbols_seen) {
            agg->worst_missing_symbols_seen = report.max_missing_symbols_in_block;
        }

        if (suspicious_blocks_in_run > 0U) {
            agg->suspicious_runs_count++;
            agg->suspicious_blocks_total += suspicious_blocks_in_run;
        }

        if (result.missing_packets == 0U && result.corrupted_packets == 0U) {
            agg->pass_runs++;
        } else {
            agg->failed_runs++;
        }
    }

    print_scenario_summary(sc, agg);
}

/* =========================================================================
 * main
 * =========================================================================*/

int main(int argc, char **argv)
{
    FILE           *csv  = NULL;
    scenario_agg_t *aggs = NULL;
    int             i;
    int             exit_code = 0;
    int             parse_rc;
    log_level       campaign_log_level = WARN;
    const char     *csv_path  = "build/channel_campaign_results.csv";

    parse_rc = parse_campaign_log_level(argc, argv, &campaign_log_level);
    if (parse_rc != 0) {
        return (parse_rc > 0) ? 0 : 1;
    }

    if (log_init() != 0) {
        fprintf(stderr, "[campaign] FATAL: log_init failed\n");
        return 1;
    }

    if (log_set_level(campaign_log_level) != 0) {
        fprintf(stderr, "[campaign] FATAL: log_set_level failed\n");
        return 1;
    }

    if (sim_runner_global_init() != 0) {
        fprintf(stderr, "[campaign] FATAL: sim_runner_global_init failed\n");
        return 1;
    }

    build_very_short_bursts();
    build_single_burst_sweep();
    build_boundary_bursts();
    build_time_fades();
    build_multi_burst();
    build_blackout();
    build_corruption_only();
    build_mixed();
    build_failure_edge();

    printf("================================================================\n");
    printf("  FSO Channel Campaign Test\n");
    printf("  Scenarios : %d\n", g_num_scenarios);
    printf("  K=%d M=%d Depth=%d SymSize=%d Windows=%d\n",
           CAMP_K, CAMP_M, CAMP_DEPTH, CAMP_SYMBOL_SIZE, CAMP_NUM_WINDOWS);
    printf("  Total TX symbols per run: %d\n", CAMP_TOTAL_SYMS);
    printf("  FEC recovery span: %d symbols (%.1f us at 10 Gbps)\n",
           CAMP_FEC_SPAN, (double)CAMP_FEC_SPAN * SYM_TIME_US);
    printf("  Symbol time: %.4f us\n", SYM_TIME_US);
    printf("  Log mode : %s\n", log_level_name(campaign_log_level));
    printf("================================================================\n");

    csv = fopen(csv_path, "w");
    if (!csv) {
        fprintf(stderr, "[campaign] FATAL: cannot open CSV '%s': ", csv_path);
        perror(NULL);
        return 1;
    }

    csv_write_header(csv);
    printf("  CSV output : %s\n", csv_path);
    printf("================================================================\n");

    aggs = (scenario_agg_t *)calloc((size_t)g_num_scenarios, sizeof(scenario_agg_t));
    if (!aggs) {
        fprintf(stderr, "[campaign] FATAL: calloc aggs failed\n");
        fclose(csv);
        return 1;
    }

    for (i = 0; i < g_num_scenarios; ++i) {
        run_scenario(&g_scenarios[i], csv, &aggs[i]);
        fflush(csv);
    }

    print_campaign_summary(aggs, g_num_scenarios);

    for (i = 0; i < g_num_scenarios; ++i) {
        if (aggs[i].pipeline_errors > 0U) {
            exit_code = 1;
            break;
        }
    }

    free(aggs);
    fclose(csv);
    return exit_code;
}