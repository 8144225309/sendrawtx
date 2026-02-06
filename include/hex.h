#ifndef HEX_H
#define HEX_H

#include <stddef.h>
#include <stdint.h>

/*
 * Optimized hex character validation using lookup table.
 *
 * Performance improvement:
 *   - Old: 3 range comparisons per character
 *   - New: 1 table lookup per character
 *
 * For large Bitcoin transaction hex (up to 4MB), this provides
 * measurable speedup on the hot validation path.
 */

/* Lookup table: 1 = valid hex char, 0 = invalid */
extern const uint8_t hex_char_valid[256];

/*
 * Check if single character is valid hex (0-9, a-f, A-F).
 * Inline for best performance on hot paths.
 */
static inline int is_hex_char(unsigned char c)
{
    return hex_char_valid[c];
}

/*
 * Check if entire string contains only valid hex characters.
 *
 * @param data Pointer to string data
 * @param len  Length of string
 * @return 1 if all characters are hex, 0 otherwise
 */
int is_all_hex(const char *data, size_t len);

#endif /* HEX_H */
