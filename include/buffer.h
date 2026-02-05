#ifndef BUFFER_H
#define BUFFER_H

#include <stddef.h>

/* Global memory tracking */
extern size_t g_total_allocated;

typedef struct {
    char *data;
    size_t len;      /* Current data length */
    size_t cap;      /* Allocated capacity */
} Buffer;

/*
 * Create a new buffer with the specified initial capacity.
 * Returns NULL on allocation failure.
 */
Buffer *buffer_new(size_t initial_cap);

/*
 * Grow buffer to at least min_cap bytes.
 * Growth strategy: 2x under 64KB, 1.5x above.
 * Returns 0 on success, -1 on failure.
 */
int buffer_grow(Buffer *b, size_t min_cap);

/*
 * Append data to the buffer, growing if necessary.
 * Returns 0 on success, -1 on failure.
 */
int buffer_append(Buffer *b, const char *data, size_t len);

/*
 * Free the buffer and its data.
 */
void buffer_free(Buffer *b);

/*
 * Reset buffer length to 0, keeping allocation.
 */
void buffer_reset(Buffer *b);

/*
 * Set the maximum allowed buffer size.
 * Returns the previous max size.
 */
size_t buffer_set_max_size(size_t max_size);

/*
 * Get the current maximum buffer size.
 */
size_t buffer_get_max_size(void);

#endif /* BUFFER_H */
