/*
 * MOSTLY DEAD CODE — only tier_name() and size_to_tier() are still used.
 *
 * read_request_line() and its helpers (approaching_threshold, calculate_read_size,
 * wait_for_read, find_newline) were the synchronous select()+read() reader,
 * replaced by libevent bufferevent async I/O in connection.c (v6 rewrite).
 *
 * LIVE functions (called from connection.c and http2.c):
 *   - tier_name()     — returns human-readable tier name
 *   - size_to_tier()  — maps request size to tier enum
 * These two should be factored into their own file if reader.c is removed.
 */

#include "reader.h"
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>
#include <string.h>

#define NORMAL_CHUNK_SIZE 4096
#define THRESHOLD_APPROACH_ZONE 4096

const char *tier_name(RequestTier tier)
{
    switch (tier) {
        case TIER_NORMAL: return "NORMAL";
        case TIER_LARGE:  return "LARGE";
        case TIER_HUGE:   return "HUGE";
        default:          return "UNKNOWN";
    }
}

RequestTier size_to_tier(size_t size, Config *cfg)
{
    if (size >= cfg->tier_huge_threshold) {
        return TIER_HUGE;
    } else if (size >= cfg->tier_large_threshold) {
        return TIER_LARGE;
    }
    return TIER_NORMAL;
}

/* DEAD CODE below: approaching_threshold, calculate_read_size, wait_for_read,
 * find_newline, and read_request_line are all unused since the v6 rewrite.
 * Only called internally by read_request_line(), which has no callers. */

/*
 * Check if we're approaching a threshold.
 * Returns the threshold if we're within THRESHOLD_APPROACH_ZONE bytes,
 * or 0 if not approaching any threshold.
 */
static size_t approaching_threshold(size_t current_size, Config *cfg)
{
    /* Check if we're approaching large threshold */
    if (current_size < cfg->tier_large_threshold &&
        current_size + THRESHOLD_APPROACH_ZONE >= cfg->tier_large_threshold) {
        return cfg->tier_large_threshold;
    }

    /* Check if we're approaching huge threshold */
    if (current_size < cfg->tier_huge_threshold &&
        current_size + THRESHOLD_APPROACH_ZONE >= cfg->tier_huge_threshold) {
        return cfg->tier_huge_threshold;
    }

    return 0;
}

/*
 * Calculate how much to read in this iteration.
 */
static size_t calculate_read_size(size_t current_size, Config *cfg)
{
    size_t threshold = approaching_threshold(current_size, cfg);

    if (threshold > 0) {
        /* Read byte-by-byte when close to threshold to hit it exactly */
        size_t bytes_to_threshold = threshold - current_size;
        if (bytes_to_threshold <= THRESHOLD_APPROACH_ZONE) {
            /* Return small chunk to get close, then 1 byte to cross */
            if (bytes_to_threshold > 1) {
                return bytes_to_threshold - 1;
            }
            return 1;
        }
    }

    return NORMAL_CHUNK_SIZE;
}

/*
 * Wait for fd to be readable with timeout.
 * Returns 1 if readable, 0 on timeout, -1 on error.
 */
static int wait_for_read(int fd, int timeout_sec)
{
    fd_set readfds;
    struct timeval tv;
    int ret;

    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);

    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    ret = select(fd + 1, &readfds, NULL, NULL, &tv);

    if (ret < 0) {
        return -1;
    } else if (ret == 0) {
        return 0; /* Timeout */
    }

    return 1;
}

/*
 * Find newline in buffer, returns position or -1 if not found.
 */
static ssize_t find_newline(const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            return (ssize_t)i;
        }
    }
    return -1;
}

ReadResult read_request_line(
    int fd,
    Buffer *buf,
    Config *cfg,
    TierCallback on_tier_change,
    void *userdata,
    RequestTier *final_tier)
{
    RequestTier current_tier = TIER_NORMAL;
    char chunk[NORMAL_CHUNK_SIZE];
    ssize_t newline_pos;

    if (final_tier) {
        *final_tier = TIER_NORMAL;
    }

    while (1) {
        size_t to_read;
        ssize_t bytes_read;
        int wait_ret;
        RequestTier new_tier;

        /* Check if we already have a newline */
        newline_pos = find_newline(buf->data, buf->len);
        if (newline_pos >= 0) {
            if (final_tier) {
                *final_tier = current_tier;
            }
            return READ_OK;
        }

        /* Check max size */
        if (buf->len >= cfg->max_buffer_size) {
            return READ_TOO_LARGE;
        }

        /* Calculate how much to read */
        to_read = calculate_read_size(buf->len, cfg);
        if (to_read > sizeof(chunk)) {
            to_read = sizeof(chunk);
        }

        /* Don't exceed max buffer size */
        if (buf->len + to_read > cfg->max_buffer_size) {
            to_read = cfg->max_buffer_size - buf->len;
        }

        /* Wait for data with timeout */
        wait_ret = wait_for_read(fd, cfg->read_timeout_sec);
        if (wait_ret == 0) {
            return READ_TIMEOUT;
        } else if (wait_ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return READ_ERROR;
        }

        /* Read data */
        bytes_read = read(fd, chunk, to_read);

        if (bytes_read < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            return READ_ERROR;
        }

        if (bytes_read == 0) {
            /* EOF - incomplete request */
            return READ_INCOMPLETE;
        }

        /* Append to buffer */
        if (buffer_append(buf, chunk, (size_t)bytes_read) != 0) {
            return READ_TOO_LARGE;
        }

        /* Check for tier transition */
        new_tier = size_to_tier(buf->len, cfg);
        if (new_tier != current_tier) {
            /* Tier changed, invoke callback */
            if (on_tier_change) {
                int cb_ret = on_tier_change(new_tier, buf->len, userdata);
                if (cb_ret != 0) {
                    return READ_TIER_EXCEEDED;
                }
            }
            current_tier = new_tier;
        }
    }
}
