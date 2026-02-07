/*
 * DEAD CODE — entire file is unused.
 *
 * Replaced by: libevent's evbuffer (v6 rewrite). connection.c notes:
 *   "v6: No more request_buffer allocation - we use evbuffer directly."
 *   "v6: No more request_buffer to free."
 *
 * The only caller was reader.c's read_request_line(), which is also dead code.
 * No production or test code calls any function in this file.
 * Still compiled via Makefile but nothing links to it.
 *
 * Safe to remove this file and include/buffer.h.
 */

#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Global memory tracking */
size_t g_total_allocated = 0;

/* Maximum buffer size (default 16MB) */
static size_t s_max_buffer_size = 16 * 1024 * 1024;

/* Threshold for switching growth strategies */
#define GROWTH_THRESHOLD (64 * 1024)

size_t buffer_set_max_size(size_t max_size)
{
    size_t old = s_max_buffer_size;
    s_max_buffer_size = max_size;
    return old;
}

size_t buffer_get_max_size(void)
{
    return s_max_buffer_size;
}

Buffer *buffer_new(size_t initial_cap)
{
    Buffer *b;

    if (initial_cap == 0) {
        initial_cap = 4096;
    }

    /* Check against max size */
    if (initial_cap > s_max_buffer_size) {
        return NULL;
    }

    b = malloc(sizeof(Buffer));
    if (!b) {
        return NULL;
    }

    b->data = malloc(initial_cap);
    if (!b->data) {
        free(b);
        return NULL;
    }

    b->len = 0;
    b->cap = initial_cap;
    g_total_allocated += initial_cap;

    return b;
}

int buffer_grow(Buffer *b, size_t min_cap)
{
    size_t new_cap;
    char *new_data;

    if (!b) {
        return -1;
    }

    /* Already large enough */
    if (b->cap >= min_cap) {
        return 0;
    }

    /* Check for overflow and max size */
    if (min_cap > s_max_buffer_size) {
        return -1;
    }

    /* Calculate new capacity with growth strategy */
    new_cap = b->cap;

    while (new_cap < min_cap) {
        size_t growth;

        if (new_cap < GROWTH_THRESHOLD) {
            /* 2x growth under 64KB */
            /* Check for overflow */
            if (new_cap > SIZE_MAX / 2) {
                return -1;
            }
            growth = new_cap;
        } else {
            /* 1.5x growth above 64KB */
            /* Check for overflow */
            if (new_cap > SIZE_MAX / 3 * 2) {
                return -1;
            }
            growth = new_cap / 2;
        }

        /* Check for overflow in addition */
        if (new_cap > SIZE_MAX - growth) {
            return -1;
        }
        new_cap += growth;

        /* Cap at max size */
        if (new_cap > s_max_buffer_size) {
            new_cap = s_max_buffer_size;
            if (new_cap < min_cap) {
                return -1;
            }
            break;
        }
    }

    new_data = realloc(b->data, new_cap);
    if (!new_data) {
        return -1;
    }

    g_total_allocated += (new_cap - b->cap);
    b->data = new_data;
    b->cap = new_cap;

    return 0;
}

int buffer_append(Buffer *b, const char *data, size_t len)
{
    size_t required;

    if (!b || (!data && len > 0)) {
        return -1;
    }

    if (len == 0) {
        return 0;
    }

    /* Check for overflow */
    if (b->len > SIZE_MAX - len) {
        return -1;
    }

    required = b->len + len;

    if (required > b->cap) {
        if (buffer_grow(b, required) != 0) {
            return -1;
        }
    }

    memcpy(b->data + b->len, data, len);
    b->len += len;

    return 0;
}

void buffer_free(Buffer *b)
{
    if (!b) {
        return;
    }

    if (b->data) {
        g_total_allocated -= b->cap;
        free(b->data);
    }

    free(b);
}

void buffer_reset(Buffer *b)
{
    if (b) {
        b->len = 0;
    }
}
