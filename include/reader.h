#ifndef READER_H
#define READER_H

#include "buffer.h"
#include "config.h"

typedef enum {
    TIER_NORMAL = 0,
    TIER_LARGE = 1,
    TIER_HUGE = 2
} RequestTier;

typedef enum {
    READ_OK = 0,
    READ_ERROR = -1,
    READ_TOO_LARGE = -2,
    READ_TIMEOUT = -3,
    READ_TIER_EXCEEDED = -4,
    READ_INCOMPLETE = -5
} ReadResult;

/* Tier callback - called when crossing threshold */
typedef int (*TierCallback)(RequestTier new_tier, size_t bytes_read, void *userdata);

/*
 * Read HTTP request line from file descriptor into buffer.
 *
 * Implements adaptive reading strategy:
 * - 4KB chunks normally
 * - Byte-by-byte 4KB before thresholds
 * - Tier callback at each threshold
 *
 * Returns:
 *   READ_OK on success (found \r\n or \n)
 *   READ_ERROR on read error
 *   READ_TOO_LARGE if max buffer size exceeded
 *   READ_TIMEOUT if read times out
 *   READ_TIER_EXCEEDED if callback rejects tier change
 */
ReadResult read_request_line(
    int fd,
    Buffer *buf,
    Config *cfg,
    TierCallback on_tier_change,
    void *userdata,
    RequestTier *final_tier
);

/*
 * Get tier name as string.
 */
const char *tier_name(RequestTier tier);

/*
 * Determine tier based on size and config.
 */
RequestTier size_to_tier(size_t size, Config *cfg);

#endif /* READER_H */
