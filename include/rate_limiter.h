#ifndef RATE_LIMITER_H
#define RATE_LIMITER_H

#include <stdint.h>
#include <time.h>

/*
 * Rate Limiter - Token bucket per IP address
 *
 * Each worker has its own rate limiter (no locking needed).
 * Uses token bucket algorithm:
 * - Tokens replenish at 'rate' tokens per second
 * - Bucket holds max 'burst' tokens
 * - Each request consumes 1 token
 * - Request denied if no tokens available
 *
 * Total system rate = num_workers × rate_per_worker
 */

/* Maximum number of tracked IPs per worker */
#define RATE_LIMITER_MAX_ENTRIES 10000

/* Entry expiration time in seconds (cleanup stale entries) */
#define RATE_LIMITER_ENTRY_TTL 60

/* IP address key (supports both IPv4 and IPv6) */
typedef struct {
    uint8_t addr[16];   /* IPv6 or IPv4-mapped */
    uint8_t is_ipv6;
} RateLimitKey;

/* Per-IP bucket entry */
typedef struct RateLimitEntry {
    RateLimitKey key;
    double tokens;              /* Current token count */
    double last_update;         /* Last token replenishment (seconds with microsecond precision) */
    time_t last_request;        /* For TTL expiration (seconds) */
    struct RateLimitEntry *next; /* Hash chain */
} RateLimitEntry;

/* Rate limiter state */
typedef struct RateLimiter {
    RateLimitEntry **buckets;   /* Hash table */
    int num_buckets;            /* Hash table size */
    int num_entries;            /* Current entry count */
    double rate;                /* Tokens per second */
    double burst;               /* Max tokens (bucket size) */
    int enabled;                /* 0 = disabled (allow all) */
} RateLimiter;

/*
 * Initialize rate limiter.
 * rate: requests per second allowed
 * burst: maximum burst size (bucket capacity)
 * Returns 0 on success, -1 on error.
 */
int rate_limiter_init(RateLimiter *rl, double rate, double burst);

/*
 * Free rate limiter resources.
 */
void rate_limiter_free(RateLimiter *rl);

/*
 * Check if request from IP is allowed.
 * Consumes a token if allowed.
 * Returns 1 if allowed, 0 if rate limited.
 */
int rate_limiter_allow(RateLimiter *rl, const char *ip_str);

/*
 * Get current stats.
 */
int rate_limiter_get_entry_count(RateLimiter *rl);

/*
 * Clean up expired entries.
 * Called periodically to prevent memory growth.
 */
void rate_limiter_cleanup(RateLimiter *rl);

#endif /* RATE_LIMITER_H */
