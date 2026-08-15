/* Shared parser helpers for the kitty-keyboard library layers.
 * Internal: not installed; include only from library translation units. */
#ifndef KITTY_KEYBOARD_INTERNAL_H
#define KITTY_KEYBOARD_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool
cancels_sequence(unsigned char byte)
{
    return byte == 0x18u || byte == 0x1au;
}

static inline bool
parse_decimal(
    const char *text,
    size_t begin,
    size_t end,
    uint32_t *value)
{
    uint32_t result = 0u;
    size_t index;

    if (text == NULL || value == NULL || begin >= end) {
        return false;
    }
    for (index = begin; index < end; ++index) {
        const unsigned char character = (unsigned char)text[index];
        const uint32_t digit = (uint32_t)(character - (unsigned char)'0');

        if (character < (unsigned char)'0' ||
            character > (unsigned char)'9' ||
            result > (UINT32_MAX - digit) / 10u) {
            return false;
        }
        result = result * 10u + digit;
    }
    *value = result;
    return true;
}

#endif
