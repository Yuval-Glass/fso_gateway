/*
 * include/arp_cache.h — Thread-safe proxy-ARP IP→MAC table.
 *
 * The TX pipeline looks up target IPs here to answer ARP requests locally.
 * The RX pipeline populates the table whenever a decoded packet reveals a
 * remote host's MAC address.
 */

#ifndef ARP_CACHE_H
#define ARP_CACHE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct arp_cache arp_cache_t;

/*
 * Public entry shape for arp_cache_dump().
 *
 * ip_nbo       — IPv4 address in network byte order (same value that was
 *                passed to arp_cache_learn()).
 * mac[6]       — the MAC address.
 * last_seen_ms — wall-clock time of the last learn for this entry, in
 *                milliseconds since the epoch.
 */
struct arp_entry {
    uint32_t ip_nbo;
    uint8_t  mac[6];
    int64_t  last_seen_ms;
};

arp_cache_t *arp_cache_create(void);
void         arp_cache_destroy(arp_cache_t *cache);

/*
 * arp_cache_learn() — store ip (network byte order) → mac mapping.
 * Overwrites any existing entry for the same IP.
 */
void arp_cache_learn(arp_cache_t *cache, uint32_t ip_nbo,
                     const uint8_t *mac);

/*
 * arp_cache_lookup() — fill mac_out[6] for ip_nbo.
 * Returns 1 if found, 0 if not found.
 */
int  arp_cache_lookup(arp_cache_t *cache, uint32_t ip_nbo,
                      uint8_t *mac_out);

/*
 * arp_cache_dump() — copy up to `max` entries into `out`.
 *
 * Returns the number of entries written. Thread-safe. Intended for the
 * control_server to expose ARP state to diagnostics UIs.
 */
int arp_cache_dump(arp_cache_t *cache, struct arp_entry *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* ARP_CACHE_H */
