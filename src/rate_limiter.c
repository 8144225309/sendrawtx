#include "rate_limiter.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <arpa/inet.h>

/*
 * Get current time as double (seconds with microsecond precision).
 * This allows sub-second rate limiting accuracy.
 */
static double get_time_precise(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

/* Hash table size (prime for better distribution) */
#define HASH_SIZE 4099

/*
 * Parse IP string to key (handles both IPv4 and IPv6).
 */
static int parse_ip(const char *ip_str, RateLimitKey *key)
{
    memset(key, 0, sizeof(*key));

    /* Try IPv4 first */
    struct in_addr addr4;
    if (inet_pton(AF_INET, ip_str, &addr4) == 1) {
        /* Store as IPv4-mapped IPv6: ::ffff:x.x.x.x */
        key->addr[10] = 0xff;
        key->addr[11] = 0xff;
        memcpy(&key->addr[12], &addr4, 4);
        key->is_ipv6 = 0;
        return 0;
    }

    /* Try IPv6 */
    struct in6_addr addr6;
    if (inet_pton(AF_INET6, ip_str, &addr6) == 1) {
        memcpy(key->addr, &addr6, 16);
        key->is_ipv6 = 1;
        return 0;
    }

    return -1;
}

/*
 * Hash function for IP key.
 */
static uint32_t hash_key(const RateLimitKey *key)
{
    /* FNV-1a hash */
    uint32_t hash = 2166136261u;
    for (int i = 0; i < 16; i++) {
        hash ^= key->addr[i];
        hash *= 16777619u;
    }
    return hash;
}

/*
 * Compare two keys for equality.
 */
static int keys_equal(const RateLimitKey *a, const RateLimitKey *b)
{
    return memcmp(a->addr, b->addr, 16) == 0;
}

int rate_limiter_init(RateLimiter *rl, double rate, double burst)
{
    memset(rl, 0, sizeof(*rl));

    /* Rate of 0 means disabled */
    if (rate <= 0) {
        rl->enabled = 0;
        return 0;
    }

    rl->buckets = calloc(HASH_SIZE, sizeof(RateLimitEntry *));
    if (!rl->buckets) {
        return -1;
    }

    rl->num_buckets = HASH_SIZE;
    rl->num_entries = 0;
    rl->rate = rate;
    rl->burst = burst > 0 ? burst : rate;  /* Default burst = rate */
    rl->enabled = 1;

    return 0;
}

void rate_limiter_free(RateLimiter *rl)
{
    if (!rl->buckets) {
        return;
    }

    /* Free all entries */
    for (int i = 0; i < rl->num_buckets; i++) {
        RateLimitEntry *entry = rl->buckets[i];
        while (entry) {
            RateLimitEntry *next = entry->next;
            free(entry);
            entry = next;
        }
    }

    free(rl->buckets);
    rl->buckets = NULL;
}

/*
 * Find or create entry for IP.
 */
static RateLimitEntry *get_entry(RateLimiter *rl, const RateLimitKey *key, double now)
{
    uint32_t hash = hash_key(key);
    int bucket = hash % rl->num_buckets;

    /* Search existing entries */
    RateLimitEntry *entry = rl->buckets[bucket];
    while (entry) {
        if (keys_equal(&entry->key, key)) {
            return entry;
        }
        entry = entry->next;
    }

    /* Create new entry if under limit */
    if (rl->num_entries >= RATE_LIMITER_MAX_ENTRIES) {
        /* Table full - run cleanup and try again */
        rate_limiter_cleanup(rl);
        if (rl->num_entries >= RATE_LIMITER_MAX_ENTRIES) {
            /* Still full - deny request (fail safe) */
            return NULL;
        }
    }

    entry = malloc(sizeof(RateLimitEntry));
    if (!entry) {
        return NULL;
    }

    memcpy(&entry->key, key, sizeof(*key));
    entry->tokens = rl->burst;  /* Start with full bucket */
    entry->last_update = now;
    entry->last_request = (time_t)now;  /* Integer seconds for TTL */
    entry->next = rl->buckets[bucket];
    rl->buckets[bucket] = entry;
    rl->num_entries++;

    return entry;
}

/*
 * Replenish tokens based on elapsed time.
 * Uses sub-second precision for accurate rate limiting.
 */
static void replenish_tokens(RateLimiter *rl, RateLimitEntry *entry, double now)
{
    if (now <= entry->last_update) {
        return;
    }

    double elapsed = now - entry->last_update;
    double new_tokens = entry->tokens + (elapsed * rl->rate);

    /* Cap at burst limit */
    if (new_tokens > rl->burst) {
        new_tokens = rl->burst;
    }

    entry->tokens = new_tokens;
    entry->last_update = now;
}

int rate_limiter_allow(RateLimiter *rl, const char *ip_str)
{
    /* Disabled = allow all */
    if (!rl->enabled || !rl->buckets) {
        return 1;
    }

    RateLimitKey key;
    if (parse_ip(ip_str, &key) < 0) {
        /* Can't parse IP - allow (fail open) */
        return 1;
    }

    double now = get_time_precise();

    RateLimitEntry *entry = get_entry(rl, &key, now);
    if (!entry) {
        /* Can't track - deny (fail safe when table full) */
        return 0;
    }

    /* Update last request time for TTL (integer seconds) */
    entry->last_request = (time_t)now;

    /* Replenish tokens */
    replenish_tokens(rl, entry, now);

    /* Check if we have a token */
    if (entry->tokens >= 1.0) {
        entry->tokens -= 1.0;
        return 1;  /* Allowed */
    }

    return 0;  /* Rate limited */
}

int rate_limiter_get_entry_count(RateLimiter *rl)
{
    return rl->num_entries;
}

void rate_limiter_cleanup(RateLimiter *rl)
{
    if (!rl->buckets) {
        return;
    }

    time_t now = time(NULL);
    time_t expiry = now - RATE_LIMITER_ENTRY_TTL;

    for (int i = 0; i < rl->num_buckets; i++) {
        RateLimitEntry **pp = &rl->buckets[i];
        while (*pp) {
            RateLimitEntry *entry = *pp;
            if (entry->last_request < expiry) {
                /* Entry expired - remove */
                *pp = entry->next;
                free(entry);
                rl->num_entries--;
            } else {
                pp = &entry->next;
            }
        }
    }
}
