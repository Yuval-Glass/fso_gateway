/*
 * src/packet_io_dpdk.c — DPDK-based packet I/O implementation.
 *
 * Drop-in replacement for packet_io.c, selected at build time via
 * -DUSE_DPDK_BUILD.  Implements the same packet_io.h API on top of DPDK's
 * Poll Mode Driver (PMD), bypassing the kernel entirely for line-rate
 * 10 Gbps operation.
 *
 * Architecture
 * ------------
 * One `port_state` is created per physical NIC (shared between the two
 * packet_io_ctx_t handles that gateway.c opens on each interface).  The
 * port is configured with 1 RX queue and 1 TX queue; calls from both
 * the TX and RX pipeline threads are to different queues and are safe.
 *
 * EAL initialisation
 * ------------------
 * Called once on the first packet_io_open().  Uses --no-pci so that only
 * the explicitly probed interfaces are attached.  Each unique interface is
 * probed via rte_dev_probe() using the PCI address read from sysfs.
 *
 * Environment variables
 * ---------------------
 *   FSO_LAN_PCI   — override sysfs PCI lookup for the LAN interface
 *   FSO_FSO_PCI   — override sysfs PCI lookup for the FSO interface
 *   DPDK_CORES    — CPU list passed to EAL -l flag (default "0-3")
 *
 * Loop prevention
 * ---------------
 * With pcap we used PACKET_IGNORE_OUTGOING to prevent tx_pipeline from
 * re-capturing frames injected by rx_pipeline into the LAN.  With DPDK on
 * physical NICs the TX ring does not feed back into the RX ring, but we
 * add a software dedup hash table as belt-and-suspenders.  When
 * packet_io_ignore_outgoing() is called on a context, all subsequent
 * packet_io_send() calls on the SAME PORT record a fingerprint in the
 * port's dedup table.  packet_io_receive() checks and silently drops
 * matching frames.
 *
 * Hugepages
 * ---------
 * DPDK requires hugepages.  Before running:
 *   echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
 */

#ifdef USE_DPDK_BUILD

#define _POSIX_C_SOURCE 200112L
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <net/if.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_dev.h>
#include <rte_bus_pci.h>
#include <rte_version.h>
#include <rte_flow.h>

#include "logging.h"
#include "packet_io.h"

/* =========================================================================
 * Compile-time constants
 * ========================================================================= */

/* mbuf pool: 8191 entries, cache of 250 per lcore */
#define DPDK_NUM_MBUFS        8191U
#define DPDK_MBUF_CACHE_SIZE   250U

/* Each mbuf data area: headroom + max jumbo frame */
#define DPDK_MBUF_DATA_SIZE   (RTE_PKTMBUF_HEADROOM + 9216U)

/* Descriptor ring sizes */
#define DPDK_RX_RING_SIZE     1024U
#define DPDK_TX_RING_SIZE     1024U

/* RX burst size: number of mbufs fetched in one rte_eth_rx_burst call */
#define DPDK_RX_BURST_SZ       32U

/* Maximum simultaneous ports (LAN + FSO = 2 in practice) */
#define DPDK_MAX_PORTS          4U

/* Dedup table: power-of-2 open-addressing hash table.
 * At 10 Gbps (833 K fps for 1500-byte frames) and 5 ms TTL we have at most
 * ~4200 concurrent injected frames.  A 16384-entry table gives <26% load. */
#define DEDUP_TABLE_SIZE       16384U   /* must be power of 2 */
#define DEDUP_TTL_NS           5000000LL  /* 5 ms */

/* =========================================================================
 * Internal types
 * ========================================================================= */

struct dedup_table {
    uint32_t hashes[DEDUP_TABLE_SIZE];
    int64_t  expires[DEDUP_TABLE_SIZE];  /* CLOCK_MONOTONIC nanoseconds */
    int      enabled;
    pthread_spinlock_t lock;
};

struct port_state {
    char                iface_name[IF_NAMESIZE];
    uint16_t            port_id;
    struct rte_mempool *mbuf_pool;
    struct dedup_table  dedup;
    int                 configured;   /* configure+start done */
    int                 ref_count;
    pthread_mutex_t     lock;
};

struct packet_io_ctx {
    struct port_state  *port;
    int                 promiscuous;
    char                last_error[512];

    /* RX burst buffer — mbufs returned by rte_eth_rx_burst, served one at
     * a time to the caller via packet_io_receive()                        */
    struct rte_mbuf    *rx_burst[DPDK_RX_BURST_SZ];
    uint16_t            rx_burst_count;
    uint16_t            rx_burst_idx;
};

/* =========================================================================
 * Global state
 * ========================================================================= */

static int              g_eal_initialized = 0;
static pthread_mutex_t  g_eal_lock        = PTHREAD_MUTEX_INITIALIZER;

static struct port_state g_ports[DPDK_MAX_PORTS];
static int               g_num_ports = 0;
static pthread_mutex_t   g_ports_lock = PTHREAD_MUTEX_INITIALIZER;

/* =========================================================================
 * Helpers — time
 * ========================================================================= */

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* =========================================================================
 * Helpers — FNV-1a hash (fast, good distribution for dedup)
 * ========================================================================= */

static uint32_t fnv1a(const unsigned char *buf, size_t len)
{
    uint32_t h = 2166136261u;
    size_t   n = (len < 64) ? len : 64;
    size_t   i;

    for (i = 0; i < n; ++i) {
        h ^= (uint32_t)buf[i];
        h *= 16777619u;
    }
    return h;
}

/* =========================================================================
 * Helpers — dedup table
 * ========================================================================= */

static void dedup_init(struct dedup_table *dt)
{
    memset(dt, 0, sizeof(*dt));
    pthread_spin_init(&dt->lock, PTHREAD_PROCESS_PRIVATE);
}

/* Record a frame fingerprint so it can be suppressed on receive. */
static void dedup_add(struct dedup_table *dt,
                      const unsigned char *buf, size_t len)
{
    uint32_t hash;
    uint32_t slot;
    int64_t  expire;

    if (!dt->enabled) {
        return;
    }

    hash   = fnv1a(buf, len);
    expire = mono_ns() + DEDUP_TTL_NS;
    slot   = hash & (DEDUP_TABLE_SIZE - 1U);

    pthread_spin_lock(&dt->lock);
    dt->hashes[slot]  = hash;
    dt->expires[slot] = expire;
    pthread_spin_unlock(&dt->lock);
}

/* Returns 1 if frame looks self-sent (should be suppressed), 0 otherwise. */
static int dedup_check(struct dedup_table *dt,
                       const unsigned char *buf, size_t len)
{
    uint32_t hash;
    uint32_t slot;
    int      found = 0;
    int64_t  now;

    if (!dt->enabled) {
        return 0;
    }

    hash = fnv1a(buf, len);
    slot = hash & (DEDUP_TABLE_SIZE - 1U);
    now  = mono_ns();

    pthread_spin_lock(&dt->lock);
    if (dt->hashes[slot] == hash && dt->expires[slot] > now) {
        found = 1;
        /* Consume the entry so a legitimate identical frame can pass. */
        dt->expires[slot] = 0;
    }
    pthread_spin_unlock(&dt->lock);

    return found;
}

/* =========================================================================
 * Helpers — sysfs PCI address lookup
 * ========================================================================= */

static int iface_to_pci(const char *iface, char *pci_out, size_t pci_len)
{
    char    path[256];
    char    target[512];
    char   *slash;
    ssize_t n;

    /* Check environment overrides first */
    {
        char env_key[64];
        const char *env_val;
        snprintf(env_key, sizeof(env_key), "FSO_%s_PCI", iface);
        /* Uppercase the iface name in the key */
        for (size_t i = 4; env_key[i]; ++i) {
            if (env_key[i] >= 'a' && env_key[i] <= 'z') {
                env_key[i] = (char)(env_key[i] - 32);
            }
        }
        env_val = getenv(env_key);
        if (env_val != NULL) {
            strncpy(pci_out, env_val, pci_len - 1);
            pci_out[pci_len - 1] = '\0';
            return 0;
        }
        /* Also check FSO_LAN_PCI / FSO_FSO_PCI generic names */
    }

    snprintf(path, sizeof(path), "/sys/class/net/%s/device", iface);
    n = readlink(path, target, sizeof(target) - 1);
    if (n < 0) {
        return -1;
    }
    target[n] = '\0';

    slash = strrchr(target, '/');
    if (slash == NULL) {
        return -1;
    }

    strncpy(pci_out, slash + 1, pci_len - 1);
    pci_out[pci_len - 1] = '\0';
    return 0;
}

/* =========================================================================
 * EAL initialisation (called once from packet_io_open, before any use)
 * ========================================================================= */

static int eal_init_once(void)
{
    const char *env_cores;
    char        cores_buf[32];
    const char *argv[16];
    int         argc = 0;
    int         ret;

    if (g_eal_initialized) {
        return 0;
    }

    env_cores = getenv("DPDK_CORES");
    strncpy(cores_buf, (env_cores != NULL) ? env_cores : "0-3",
            sizeof(cores_buf) - 1);
    cores_buf[sizeof(cores_buf) - 1] = '\0';

    argv[argc++] = "fso_gateway";
    argv[argc++] = "-l";
    argv[argc++] = cores_buf;
    argv[argc++] = "--proc-type";
    argv[argc++] = "primary";
    argv[argc++] = "--in-memory";
    argv[argc++] = "--log-level";
    argv[argc++] = "4";              /* ERR level, keep DPDK logs quiet */

    LOG_INFO("[packet_io_dpdk] EAL init: cores=%s", cores_buf);

    ret = rte_eal_init(argc, (char **)argv);
    if (ret < 0) {
        LOG_ERROR("[packet_io_dpdk] rte_eal_init failed (ret=%d). "
                  "Ensure hugepages are configured: "
                  "echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages",
                  ret);
        return -1;
    }

    g_eal_initialized = 1;
    LOG_INFO("[packet_io_dpdk] EAL initialised (DPDK %s)",
             rte_version());
    return 0;
}

/* =========================================================================
 * Port management
 * ========================================================================= */

/* Find existing port_state for iface_name, or NULL if not yet opened. */
static struct port_state *find_port(const char *iface)
{
    int i;
    for (i = 0; i < g_num_ports; ++i) {
        if (strcmp(g_ports[i].iface_name, iface) == 0) {
            return &g_ports[i];
        }
    }
    return NULL;
}

/* Probe iface, configure it, and return its port_state. */
static struct port_state *probe_and_configure_port(const char *iface,
                                                   int         promiscuous,
                                                   char       *errbuf,
                                                   size_t      errbuf_sz)
{
    char              pci_addr[64];
    uint16_t          port_id;
    char              pool_name[64];
    struct rte_eth_conf         port_conf;
    struct rte_eth_dev_info     dev_info;
    struct rte_eth_rxconf        rx_conf;
    struct rte_eth_txconf        tx_conf;
    struct port_state            *ps;
    int               ret;

    /* ---- Resolve PCI address ------------------------------------------ */
    if (iface_to_pci(iface, pci_addr, sizeof(pci_addr)) != 0) {
        snprintf(errbuf, errbuf_sz,
                 "cannot resolve PCI address for interface '%s' via sysfs; "
                 "set FSO_%s_PCI env var", iface, iface);
        return NULL;
    }

    /* ---- Probe the device --------------------------------------------- */
    ret = rte_dev_probe(pci_addr);
    if (ret != 0 && ret != -EEXIST) {
        snprintf(errbuf, errbuf_sz,
                 "rte_dev_probe('%s') failed (ret=%d). "
                 "Verify libibverbs and mlx5 RDMA providers are installed.",
                 pci_addr, ret);
        return NULL;
    }

    /* ---- Find the DPDK port_id ---------------------------------------- */
    {
        struct rte_eth_dev_info di;
        uint16_t pid;
        int found = 0;

        RTE_ETH_FOREACH_DEV(pid) {
            if (rte_eth_dev_info_get(pid, &di) != 0) continue;
            if (di.device == NULL) continue;
            if (strstr(rte_dev_name(di.device), pci_addr) != NULL) {
                port_id = pid;
                found   = 1;
                break;
            }
        }
        if (!found) {
            snprintf(errbuf, errbuf_sz,
                     "no DPDK port found for PCI '%s' after probe", pci_addr);
            return NULL;
        }
    }

    /* ---- Allocate port_state ------------------------------------------ */
    pthread_mutex_lock(&g_ports_lock);

    if (g_num_ports >= (int)DPDK_MAX_PORTS) {
        pthread_mutex_unlock(&g_ports_lock);
        snprintf(errbuf, errbuf_sz,
                 "too many ports (max=%u)", DPDK_MAX_PORTS);
        return NULL;
    }

    ps = &g_ports[g_num_ports++];
    memset(ps, 0, sizeof(*ps));
    strncpy(ps->iface_name, iface, IF_NAMESIZE - 1);
    ps->port_id   = port_id;
    ps->ref_count = 1;
    pthread_mutex_init(&ps->lock, NULL);
    dedup_init(&ps->dedup);

    pthread_mutex_unlock(&g_ports_lock);

    /* ---- Configure port ----------------------------------------------- */
    ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret != 0) {
        snprintf(errbuf, errbuf_sz,
                 "rte_eth_dev_info_get port=%u failed (ret=%d)", port_id, ret);
        return NULL;
    }

    memset(&port_conf, 0, sizeof(port_conf));
    port_conf.rxmode.mtu = 9000;   /* enable jumbo frames */

    ret = rte_eth_dev_configure(port_id, 1, 1, &port_conf);
    if (ret != 0) {
        snprintf(errbuf, errbuf_sz,
                 "rte_eth_dev_configure port=%u failed (ret=%d)", port_id, ret);
        return NULL;
    }

    /* ---- Memory pool -------------------------------------------------- */
    snprintf(pool_name, sizeof(pool_name), "fso_pool_%u", port_id);
    ps->mbuf_pool = rte_pktmbuf_pool_create(pool_name,
                                             DPDK_NUM_MBUFS,
                                             DPDK_MBUF_CACHE_SIZE,
                                             0,
                                             DPDK_MBUF_DATA_SIZE,
                                             rte_eth_dev_socket_id(port_id));
    if (ps->mbuf_pool == NULL) {
        snprintf(errbuf, errbuf_sz,
                 "rte_pktmbuf_pool_create for port=%u failed (rte_errno=%d)",
                 port_id, rte_errno);
        return NULL;
    }

    /* ---- RX queue ----------------------------------------------------- */
    memset(&rx_conf, 0, sizeof(rx_conf));
    rx_conf = dev_info.default_rxconf;
    rx_conf.offloads = port_conf.rxmode.offloads;

    ret = rte_eth_rx_queue_setup(port_id, 0, DPDK_RX_RING_SIZE,
                                 rte_eth_dev_socket_id(port_id),
                                 &rx_conf, ps->mbuf_pool);
    if (ret != 0) {
        snprintf(errbuf, errbuf_sz,
                 "rte_eth_rx_queue_setup port=%u failed (ret=%d)",
                 port_id, ret);
        return NULL;
    }

    /* ---- TX queue ----------------------------------------------------- */
    memset(&tx_conf, 0, sizeof(tx_conf));
    tx_conf = dev_info.default_txconf;
    tx_conf.offloads = port_conf.txmode.offloads;

    ret = rte_eth_tx_queue_setup(port_id, 0, DPDK_TX_RING_SIZE,
                                 rte_eth_dev_socket_id(port_id),
                                 &tx_conf);
    if (ret != 0) {
        snprintf(errbuf, errbuf_sz,
                 "rte_eth_tx_queue_setup port=%u failed (ret=%d)",
                 port_id, ret);
        return NULL;
    }

    /* ---- Start device ------------------------------------------------- */
    ret = rte_eth_dev_start(port_id);
    if (ret != 0) {
        snprintf(errbuf, errbuf_sz,
                 "rte_eth_dev_start port=%u failed (ret=%d)", port_id, ret);
        return NULL;
    }

    if (promiscuous) {
        rte_eth_promiscuous_enable(port_id);
    }

    /* Steer all ingress traffic to DPDK queue 0.
     * In mlx5 bifurcated mode RSS distributes packets across kernel and DPDK
     * queues by default.  This catch-all flow rule ensures DPDK receives
     * every ingress frame on this port. */
    {
        struct rte_flow_attr          attr  = { .ingress = 1, .priority = 0 };
        struct rte_flow_item          pattern[] = {
            { .type = RTE_FLOW_ITEM_TYPE_ETH },
            { .type = RTE_FLOW_ITEM_TYPE_END }
        };
        struct rte_flow_action_queue  q     = { .index = 0 };
        struct rte_flow_action        actions[] = {
            { .type = RTE_FLOW_ACTION_TYPE_QUEUE, .conf = &q },
            { .type = RTE_FLOW_ACTION_TYPE_END }
        };
        struct rte_flow_error         ferr;
        struct rte_flow              *flow;

        flow = rte_flow_create(port_id, &attr, pattern, actions, &ferr);
        if (flow == NULL) {
            LOG_WARN("[packet_io_dpdk] port %u: rte_flow_create failed (%s) — "
                     "RSS may split traffic between kernel and DPDK queues",
                     port_id, ferr.message ? ferr.message : "unknown");
        } else {
            LOG_INFO("[packet_io_dpdk] port %u: catch-all flow rule installed "
                     "(all ingress → DPDK queue 0)", port_id);
        }
    }

    ps->configured = 1;

    LOG_INFO("[packet_io_dpdk] port %u (%s, PCI=%s) started "
             "(promiscuous=%d, socket=%d)",
             port_id, iface, pci_addr, promiscuous,
             rte_eth_dev_socket_id(port_id));
    return ps;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

packet_io_ctx_t *packet_io_open(const char *iface,
                                int         promiscuous,
                                char       *errbuf,
                                size_t      errbuf_sz)
{
    packet_io_ctx_t  *ctx;
    struct port_state *ps;

    if (iface == NULL || errbuf == NULL || errbuf_sz == 0) {
        if (errbuf && errbuf_sz) {
            snprintf(errbuf, errbuf_sz, "invalid argument");
        }
        return NULL;
    }

    /* ---- EAL init (once) --------------------------------------------- */
    pthread_mutex_lock(&g_eal_lock);
    if (!g_eal_initialized) {
        if (eal_init_once() != 0) {
            pthread_mutex_unlock(&g_eal_lock);
            snprintf(errbuf, errbuf_sz, "DPDK EAL initialisation failed");
            return NULL;
        }
    }
    pthread_mutex_unlock(&g_eal_lock);

    /* ---- Find or probe port ------------------------------------------- */
    pthread_mutex_lock(&g_ports_lock);
    ps = find_port(iface);
    if (ps != NULL) {
        ps->ref_count++;
        /* Enable promiscuous if any ctx on this port requests it */
        if (promiscuous && ps->configured) {
            rte_eth_promiscuous_enable(ps->port_id);
        }
        pthread_mutex_unlock(&g_ports_lock);
    } else {
        pthread_mutex_unlock(&g_ports_lock);
        ps = probe_and_configure_port(iface, promiscuous, errbuf, errbuf_sz);
        if (ps == NULL) {
            return NULL;
        }
    }

    /* ---- Allocate context -------------------------------------------- */
    ctx = (packet_io_ctx_t *)malloc(sizeof(packet_io_ctx_t));
    if (ctx == NULL) {
        snprintf(errbuf, errbuf_sz, "malloc(packet_io_ctx_t) failed");
        return NULL;
    }
    memset(ctx, 0, sizeof(packet_io_ctx_t));
    ctx->port        = ps;
    ctx->promiscuous = promiscuous;

    LOG_INFO("[packet_io_dpdk] opened '%s' (port_id=%u promiscuous=%d)",
             iface, ps->port_id, promiscuous);
    return ctx;
}

void packet_io_close(packet_io_ctx_t *ctx)
{
    struct port_state *ps;
    int                i;

    if (ctx == NULL) {
        return;
    }

    ps = ctx->port;

    /* Free any buffered mbufs */
    for (i = ctx->rx_burst_idx; i < ctx->rx_burst_count; ++i) {
        rte_pktmbuf_free(ctx->rx_burst[i]);
    }

    pthread_mutex_lock(&g_ports_lock);
    ps->ref_count--;
    if (ps->ref_count <= 0 && ps->configured) {
        rte_eth_dev_stop(ps->port_id);
        rte_eth_dev_close(ps->port_id);
        rte_mempool_free(ps->mbuf_pool);
        ps->configured = 0;
        LOG_INFO("[packet_io_dpdk] port %u stopped and closed", ps->port_id);
    }
    pthread_mutex_unlock(&g_ports_lock);

    free(ctx);
}

int packet_io_receive(packet_io_ctx_t *ctx,
                      unsigned char   *buf,
                      size_t           buf_size,
                      size_t          *out_len)
{
    uint16_t            n;
    struct rte_mbuf    *m;
    size_t              pkt_len;
    size_t              copy_len;

    if (out_len != NULL) {
        *out_len = 0;
    }

    if (ctx == NULL || buf == NULL || out_len == NULL) {
        LOG_ERROR("[packet_io_dpdk] receive: invalid argument");
        return -1;
    }

    for (;;) {
        /* Drain existing burst buffer first */
        while (ctx->rx_burst_idx < ctx->rx_burst_count) {
            m = ctx->rx_burst[ctx->rx_burst_idx++];

            pkt_len  = rte_pktmbuf_pkt_len(m);
            copy_len = (pkt_len < buf_size) ? pkt_len : buf_size;

            if (copy_len < pkt_len) {
                LOG_WARN("[packet_io_dpdk] receive: frame truncated "
                         "(%zu > buf_size=%zu)", pkt_len, buf_size);
            }

            rte_memcpy(buf, rte_pktmbuf_mtod(m, const void *), copy_len);
            rte_pktmbuf_free(m);

            /* Dedup check: drop self-sent frames */
            if (dedup_check(&ctx->port->dedup, buf, copy_len)) {
                LOG_DEBUG("[packet_io_dpdk] receive: dedup drop "
                          "(self-sent frame suppressed)");
                continue;
            }

            *out_len = copy_len;
            return 1;
        }

        /* Burst buffer empty — try to refill */
        ctx->rx_burst_idx   = 0;
        ctx->rx_burst_count = 0;

        n = rte_eth_rx_burst(ctx->port->port_id, 0,
                             ctx->rx_burst, DPDK_RX_BURST_SZ);
        if (n == 0) {
            return 0;   /* no packets available */
        }

        ctx->rx_burst_count = n;
        /* Loop back to drain the newly filled burst */
    }
}

int packet_io_send(packet_io_ctx_t     *ctx,
                   const unsigned char *buf,
                   size_t               len)
{
    struct rte_mbuf *m;
    char            *dst;
    uint16_t         sent;

    if (ctx == NULL || buf == NULL || len == 0) {
        LOG_ERROR("[packet_io_dpdk] send: invalid argument");
        return -1;
    }

    /* Allocate mbuf and copy frame data */
    m = rte_pktmbuf_alloc(ctx->port->mbuf_pool);
    if (m == NULL) {
        snprintf(ctx->last_error, sizeof(ctx->last_error),
                 "rte_pktmbuf_alloc failed (pool empty)");
        LOG_WARN("[packet_io_dpdk] send: %s", ctx->last_error);
        return -1;
    }

    if (len > rte_pktmbuf_tailroom(m)) {
        snprintf(ctx->last_error, sizeof(ctx->last_error),
                 "frame too large for mbuf (%zu > %u)",
                 len, rte_pktmbuf_tailroom(m));
        LOG_WARN("[packet_io_dpdk] send: %s", ctx->last_error);
        rte_pktmbuf_free(m);
        return -1;
    }

    dst = rte_pktmbuf_append(m, (uint16_t)len);
    if (dst == NULL) {
        snprintf(ctx->last_error, sizeof(ctx->last_error),
                 "rte_pktmbuf_append failed");
        rte_pktmbuf_free(m);
        return -1;
    }

    rte_memcpy(dst, buf, len);

    /* Record frame in dedup table before sending */
    dedup_add(&ctx->port->dedup, buf, len);

    /* Transmit */
    sent = rte_eth_tx_burst(ctx->port->port_id, 0, &m, 1);
    if (sent == 0) {
        snprintf(ctx->last_error, sizeof(ctx->last_error),
                 "rte_eth_tx_burst returned 0 (TX ring full?)");
        LOG_WARN("[packet_io_dpdk] send: %s", ctx->last_error);
        rte_pktmbuf_free(m);
        return -1;
    }

    return 0;
}

const char *packet_io_last_error(const packet_io_ctx_t *ctx)
{
    if (ctx == NULL) {
        return "";
    }
    return ctx->last_error;
}

int packet_io_ignore_outgoing(packet_io_ctx_t *ctx)
{
    if (ctx == NULL) {
        return -1;
    }

    /* Enable the software dedup table on this port.  All subsequent
     * packet_io_send() calls on ANY context sharing this port will record
     * fingerprints, which packet_io_receive() will then suppress.       */
    ctx->port->dedup.enabled = 1;

    LOG_INFO("[packet_io_dpdk] ignore_outgoing: port %u dedup enabled "
             "(software PACKET_IGNORE_OUTGOING equivalent)",
             ctx->port->port_id);
    return 0;
}

int packet_io_set_direction_in(packet_io_ctx_t *ctx)
{
    /* No pcap direction filter in DPDK — physical NICs only deliver ingress
     * frames to the RX ring; the dedup table handles self-sent suppression. */
    (void)ctx;
    LOG_DEBUG("[packet_io_dpdk] set_direction_in: no-op in DPDK mode "
              "(ingress-only is the default for physical NIC RX queues)");
    return 0;
}

#endif /* USE_DPDK_BUILD */
