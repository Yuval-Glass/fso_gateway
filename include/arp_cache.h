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

#ifdef __cplusplus
}
#endif

#endif /* ARP_CACHE_H */
