/*
 * Optimized hex character validation using lookup table.
 *
 * This provides O(1) per-character validation instead of
 * multiple comparisons, improving performance for large
 * Bitcoin transaction hex strings.
 */

#include "hex.h"

/*
 * Lookup table for hex character validation.
 * Index by character value, value is 1 if valid hex.
 *
 * Valid hex characters:
 *   '0'-'9' (0x30-0x39)
 *   'A'-'F' (0x41-0x46)
 *   'a'-'f' (0x61-0x66)
 */
const uint8_t hex_char_valid[256] = {
    /* 0x00-0x0F: control characters */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x10-0x1F: control characters */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x20-0x2F: space, punctuation */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x30-0x3F: '0'-'9', then punctuation */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,
    /* 0x40-0x4F: '@', 'A'-'F', 'G'-'O' */
    0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x50-0x5F: 'P'-'Z', punctuation */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x60-0x6F: '`', 'a'-'f', 'g'-'o' */
    0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x70-0x7F: 'p'-'z', punctuation, DEL */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x80-0xFF: high bytes (non-ASCII) - all invalid */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

int is_all_hex(const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (!hex_char_valid[(unsigned char)data[i]]) {
            return 0;
        }
    }
    return 1;
}
