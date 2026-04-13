/* tools/fso_gw_runner.c */

#define _POSIX_C_SOURCE 200112L

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "gateway.h"
#include "hw_stats.h"
#include "logging.h"
#include <wirehair/wirehair.h>

#define DEFAULT_DURATION    0
#define DEFAULT_K           2
#define DEFAULT_M           1
#define DEFAULT_DEPTH       2
#define DEFAULT_SYMBOL_SIZE 750

static gateway_t              *g_gw      = NULL;
static volatile sig_atomic_t   g_running = 1;

typedef struct { int duration; } timer_args_t;
typedef struct { hw_stats_t *stats; int duration; } live_args_t;

static void print_usage(void)
{
    fprintf(stderr,
            "Usage: fso_gw_runner --lan-iface <i> --fso-iface <i>\n"
            " [--duration <sec>] [--k N] [--m N] [--depth N]\n"
            " [--symbol-size N]\n");
}

static int parse_int_arg(const char *name, const char *value, int *out_value)
{
    char *endptr;
    long  parsed;

    if (name == NULL || value == NULL || out_value == NULL) {
        LOG_ERROR("[fso_gw_runner] parse_int_arg: NULL argument");
        return -1;
    }

    errno = 0;
    parsed = strtol(value, &endptr, 10);

    if (errno != 0 || endptr == value || *endptr != '\0') {
        LOG_ERROR("[fso_gw_runner] invalid numeric value for %s: \"%s\"", name, value);
        return -1;
    }

    if (parsed < 0 || parsed > 2147483647L) {
        LOG_ERROR("[fso_gw_runner] out-of-range value for %s: \"%s\"", name, value);
        return -1;
    }

    *out_value = (int)parsed;
    return 0;
}

static void sig_handler(int signum)
{
    (void)signum;
    g_running = 0;
    if (g_gw != NULL) gateway_stop(g_gw);
}

static void *timer_thread(void *arg)
{
    timer_args_t *a = (timer_args_t *)arg;
    int remaining = a->duration;

    while (g_running && remaining > 0) {
        sleep(1);
        remaining--;
    }

    if (g_running) {
        g_running = 0;
        if (g_gw != NULL) gateway_stop(g_gw);
    }
    return NULL;
}

static void *live_stats_thread(void *arg)
{
    live_args_t *a = (live_args_t *)arg;
    int elapsed = 0;

    if (a->duration > 0) {
        while (g_running && elapsed < a->duration) {
            int step = 5;
            int remaining = a->duration - elapsed;
            if (remaining < step) step = remaining;
            if (step <= 0) break;

            sleep(step);
            elapsed += step;
            if (!g_running) break;
            hw_stats_print_live(a->stats, elapsed);
        }
    } else {
        while (g_running) {
            sleep(5);
            elapsed += 5;
            if (!g_running) break;
            hw_stats_print_live(a->stats, elapsed);
        }
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    static const struct option long_opts[] = {
        { "lan-iface", required_argument, NULL, 'l' },
        { "fso-iface", required_argument, NULL, 'f' },
        { "duration", required_argument, NULL, 'd' },
        { "k", required_argument, NULL, 'k' },
        { "m", required_argument, NULL, 'm' },
        { "depth", required_argument, NULL, 'e' },
        { "symbol-size", required_argument, NULL, 's' },
        { NULL, 0, NULL, 0 }
    };

    const char *lan_iface = NULL;
    const char *fso_iface = NULL;
    int duration = DEFAULT_DURATION;
    int k = DEFAULT_K;
    int m = DEFAULT_M;
    int depth = DEFAULT_DEPTH;
    int symbol_size = DEFAULT_SYMBOL_SIZE;

    struct config cfg;
    struct sigaction sa;

    hw_stats_t *stats = NULL;
    gateway_t *gw = NULL;

    pthread_t timer_tid, live_tid;
    timer_args_t timer_args;
    live_args_t live_args;

    int opt;
    int gw_rc = -1;
    int timer_started = 0;
    int live_started = 0;

    log_init();

    if (wirehair_init() != Wirehair_Success) {
        fprintf(stderr, "wirehair_init() failed\n");
        return 1;
    }

    mkdir("build/stats", 0755);

    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'l': lan_iface = optarg; break;
        case 'f': fso_iface = optarg; break;
        case 'd': parse_int_arg("--duration", optarg, &duration); break;
        case 'k': parse_int_arg("--k", optarg, &k); break;
        case 'm': parse_int_arg("--m", optarg, &m); break;
        case 'e': parse_int_arg("--depth", optarg, &depth); break;
        case 's': parse_int_arg("--symbol-size", optarg, &symbol_size); break;
        default: print_usage(); return 1;
        }
    }

    if (!lan_iface || !fso_iface) {
        print_usage();
        return 1;
    }

    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.lan_iface, lan_iface, sizeof(cfg.lan_iface)-1);
    strncpy(cfg.fso_iface, fso_iface, sizeof(cfg.fso_iface)-1);
    cfg.k = k;
    cfg.m = m;
    cfg.depth = depth;
    cfg.symbol_size = symbol_size;
    cfg.internal_symbol_crc_enabled = 1;

    printf("fso_gw_runner: starting\n");
    printf("  lan-iface:   %s\n", cfg.lan_iface);
    printf("  fso-iface:   %s\n", cfg.fso_iface);
    printf("  k=%d m=%d depth=%d symbol_size=%d\n", k,m,depth,symbol_size);
    printf("  duration:    %ds (0=forever)\n", duration);

    memset(&sa,0,sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT,&sa,NULL);
    sigaction(SIGTERM,&sa,NULL);

    stats = hw_stats_create();
    gw = gateway_create(&cfg);
    g_gw = gw;

    if (duration > 0) {
        timer_args.duration = duration;
        pthread_create(&timer_tid,NULL,timer_thread,&timer_args);
        timer_started = 1;
    }

    live_args.stats = stats;
    live_args.duration = duration;
    pthread_create(&live_tid,NULL,live_stats_thread,&live_args);
    live_started = 1;

    gw_rc = gateway_run(gw);

    g_running = 0;

    if (timer_started) pthread_join(timer_tid,NULL);
    if (live_started) pthread_join(live_tid,NULL);

    hw_stats_set_packets_injected(stats,0);
    hw_stats_print_report(stats);
    hw_stats_save_csv(stats,"build/stats");
    hw_stats_destroy(stats);

    gateway_destroy(gw);

    printf("fso_gw_runner: done\n");
    return (gw_rc==0)?0:1;
}
