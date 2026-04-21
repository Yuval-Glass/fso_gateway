#define _POSIX_C_SOURCE 200112L

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arp_cache.h"
#include "logging.h"

#define ARP_CACHE_MAX_ENTRIES 256
#define ARP_CACHE_TTL_SECS    300   /* 5 minutes */

struct arp_entry {
    uint32_t ip_nbo;
    uint8_t  mac[6];
    int      valid;
    time_t   last_seen;
};

struct arp_cache {
    struct arp_entry entries[ARP_CACHE_MAX_ENTRIES];
    int              count;
    pthread_mutex_t  lock;
};

arp_cache_t *arp_cache_create(void)
{
    arp_cache_t *c = (arp_cache_t *)malloc(sizeof(arp_cache_t));
    if (c == NULL) {
        LOG_ERROR("[arp_cache] create: malloc failed");
        return NULL;
    }
    memset(c, 0, sizeof(arp_cache_t));
    pthread_mutex_init(&c->lock, NULL);
    return c;
}

void arp_cache_destroy(arp_cache_t *c)
{
    if (c == NULL) {
        return;
    }
    pthread_mutex_destroy(&c->lock);
    free(c);
}

void arp_cache_learn(arp_cache_t *c, uint32_t ip_nbo, const uint8_t *mac)
{
    int    i;
    time_t now;

    if (c == NULL || mac == NULL) {
        return;
    }

    now = time(NULL);

    pthread_mutex_lock(&c->lock);

    /* Update existing entry if IP already known */
    for (i = 0; i < c->count; ++i) {
        if (c->entries[i].valid && c->entries[i].ip_nbo == ip_nbo) {
            memcpy(c->entries[i].mac, mac, 6);
            c->entries[i].last_seen = now;
            pthread_mutex_unlock(&c->lock);
            LOG_DEBUG("[arp_cache] updated %u.%u.%u.%u",
                      (ip_nbo >> 0) & 0xff, (ip_nbo >> 8) & 0xff,
                      (ip_nbo >> 16) & 0xff, (ip_nbo >> 24) & 0xff);
            return;
        }
    }

    /* Reuse an expired slot before appending */
    for (i = 0; i < c->count; ++i) {
        if (c->entries[i].valid &&
            (now - c->entries[i].last_seen) > ARP_CACHE_TTL_SECS) {
            c->entries[i].ip_nbo    = ip_nbo;
            memcpy(c->entries[i].mac, mac, 6);
            c->entries[i].last_seen = now;
            pthread_mutex_unlock(&c->lock);
            LOG_INFO("[arp_cache] learned (reused slot) %u.%u.%u.%u -> "
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     (ip_nbo >> 0) & 0xff, (ip_nbo >> 8) & 0xff,
                     (ip_nbo >> 16) & 0xff, (ip_nbo >> 24) & 0xff,
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            return;
        }
    }

    /* Append new entry */
    if (c->count < ARP_CACHE_MAX_ENTRIES) {
        c->entries[c->count].ip_nbo    = ip_nbo;
        memcpy(c->entries[c->count].mac, mac, 6);
        c->entries[c->count].valid     = 1;
        c->entries[c->count].last_seen = now;
        c->count++;
        pthread_mutex_unlock(&c->lock);
        LOG_INFO("[arp_cache] learned %u.%u.%u.%u -> "
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 (ip_nbo >> 0) & 0xff, (ip_nbo >> 8) & 0xff,
                 (ip_nbo >> 16) & 0xff, (ip_nbo >> 24) & 0xff,
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        pthread_mutex_unlock(&c->lock);
        LOG_WARN("[arp_cache] table full (%d entries), ignoring new entry",
                 ARP_CACHE_MAX_ENTRIES);
    }
}

int arp_cache_lookup(arp_cache_t *c, uint32_t ip_nbo, uint8_t *mac_out)
{
    int    i;
    int    found = 0;
    time_t now;

    if (c == NULL || mac_out == NULL) {
        return 0;
    }

    now = time(NULL);

    pthread_mutex_lock(&c->lock);

    for (i = 0; i < c->count; ++i) {
        if (!c->entries[i].valid || c->entries[i].ip_nbo != ip_nbo) {
            continue;
        }
        if ((now - c->entries[i].last_seen) > ARP_CACHE_TTL_SECS) {
            /* Expired — invalidate and fall through to FSO */
            c->entries[i].valid = 0;
            LOG_INFO("[arp_cache] entry expired for %u.%u.%u.%u",
                     (ip_nbo >> 0) & 0xff, (ip_nbo >> 8) & 0xff,
                     (ip_nbo >> 16) & 0xff, (ip_nbo >> 24) & 0xff);
            break;
        }
        memcpy(mac_out, c->entries[i].mac, 6);
        found = 1;
        break;
    }

    pthread_mutex_unlock(&c->lock);
    return found;
}
