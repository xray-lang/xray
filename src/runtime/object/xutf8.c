/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xutf8.c - UTF-8 encoding/decoding utilities implementation
 */

#include "xutf8.h"
#include "../../base/xchecks.h"
#include <string.h>

/* ========================================================================
 * UTF-8 Decode
 * ======================================================================== */

int xr_utf8_decode(const char *str, size_t len, uint32_t *out_cp) {
    if (!str || len == 0) {
        if (out_cp)
            *out_cp = 0;
        return 0;
    }

    uint8_t b0 = (uint8_t) str[0];
    uint32_t cp;
    int size;

    // Single byte ASCII (0xxxxxxx)
    if ((b0 & 0x80) == 0) {
        cp = b0;
        size = 1;
    }
    // 2 bytes (110xxxxx 10xxxxxx)
    else if ((b0 & 0xE0) == 0xC0) {
        if (len < 2)
            goto invalid;
        uint8_t b1 = (uint8_t) str[1];
        if (!xr_utf8_is_continuation(b1))
            goto invalid;

        cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);

        // Check overlong encoding
        if (cp < 0x80)
            goto invalid;
        size = 2;
    }
    // 3 bytes (1110xxxx 10xxxxxx 10xxxxxx)
    else if ((b0 & 0xF0) == 0xE0) {
        if (len < 3)
            goto invalid;
        uint8_t b1 = (uint8_t) str[1];
        uint8_t b2 = (uint8_t) str[2];
        if (!xr_utf8_is_continuation(b1) || !xr_utf8_is_continuation(b2))
            goto invalid;

        cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);

        // Check overlong encoding
        if (cp < 0x800)
            goto invalid;
        // Check surrogate pair range (U+D800..U+DFFF)
        if (cp >= 0xD800 && cp <= 0xDFFF)
            goto invalid;
        size = 3;
    }
    // 4 bytes (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
    else if ((b0 & 0xF8) == 0xF0) {
        if (len < 4)
            goto invalid;
        uint8_t b1 = (uint8_t) str[1];
        uint8_t b2 = (uint8_t) str[2];
        uint8_t b3 = (uint8_t) str[3];
        if (!xr_utf8_is_continuation(b1) || !xr_utf8_is_continuation(b2) ||
            !xr_utf8_is_continuation(b3))
            goto invalid;

        cp = ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);

        // Check overlong encoding and range
        if (cp < 0x10000 || cp > XR_UNICODE_MAX)
            goto invalid;
        size = 4;
    } else {
        goto invalid;
    }

    if (out_cp)
        *out_cp = cp;
    return size;

invalid:
    if (out_cp)
        *out_cp = XR_UNICODE_INVALID;
    return 1;  // Skip one invalid byte
}

bool xr_utf8_char_at(const char *str, size_t len, size_t index, uint32_t *out_cp, size_t *out_pos) {
    if (!str)
        return false;

    size_t pos = 0;
    size_t char_idx = 0;

    while (pos < len && char_idx < index) {
        int size = xr_utf8_char_size((uint8_t) str[pos]);
        if (pos + size > len)
            break;
        pos += size;
        char_idx++;
    }

    if (char_idx != index || pos >= len) {
        return false;  // Index out of bounds
    }

    if (out_pos)
        *out_pos = pos;

    if (out_cp) {
        xr_utf8_decode(str + pos, len - pos, out_cp);
    }

    return true;
}

/* ========================================================================
 * UTF-8 Encode
 * ======================================================================== */

int xr_utf8_encode(uint32_t cp, char *out) {
    if (!out)
        return 0;

    // Single byte ASCII
    if (cp <= 0x7F) {
        out[0] = (char) cp;
        return 1;
    }
    // 2 bytes
    else if (cp <= 0x7FF) {
        out[0] = (char) (0xC0 | (cp >> 6));
        out[1] = (char) (0x80 | (cp & 0x3F));
        return 2;
    }
    // 3 bytes
    else if (cp <= 0xFFFF) {
        // Exclude surrogate pair range
        if (cp >= 0xD800 && cp <= 0xDFFF)
            return 0;

        out[0] = (char) (0xE0 | (cp >> 12));
        out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char) (0x80 | (cp & 0x3F));
        return 3;
    }
    // 4 bytes
    else if (cp <= XR_UNICODE_MAX) {
        out[0] = (char) (0xF0 | (cp >> 18));
        out[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char) (0x80 | (cp & 0x3F));
        return 4;
    }

    return 0;  // Invalid codepoint
}

/* ========================================================================
 * String-level Operations
 * ======================================================================== */

size_t xr_utf8_strlen(const char *str, size_t len) {
    if (!str || len == 0)
        return 0;

    size_t count = 0;
    size_t pos = 0;

    while (pos < len) {
        int size = xr_utf8_char_size((uint8_t) str[pos]);
        if (pos + size > len) {
            // Incomplete char, count as single
            count++;
            break;
        }
        pos += size;
        count++;
    }

    return count;
}

size_t xr_utf8_index_to_offset(const char *str, size_t len, size_t index) {
    if (!str || len == 0)
        return 0;

    size_t pos = 0;
    size_t char_idx = 0;

    while (pos < len && char_idx < index) {
        int size = xr_utf8_char_size((uint8_t) str[pos]);
        if (pos + size > len)
            break;
        pos += size;
        char_idx++;
    }

    return pos;
}

size_t xr_utf8_offset_to_index(const char *str, size_t len, size_t offset) {
    if (!str || len == 0)
        return 0;
    if (offset > len)
        offset = len;

    size_t pos = 0;
    size_t char_idx = 0;

    while (pos < offset) {
        int size = xr_utf8_char_size((uint8_t) str[pos]);
        if (pos + size > len)
            break;
        pos += size;
        char_idx++;
    }

    return char_idx;
}

bool xr_utf8_char_range(const char *str, size_t len, size_t start_idx, size_t end_idx,
                        size_t *out_start, size_t *out_end) {
    if (!str || start_idx > end_idx)
        return false;

    size_t start_offset = xr_utf8_index_to_offset(str, len, start_idx);
    size_t end_offset = xr_utf8_index_to_offset(str, len, end_idx);

    if (out_start)
        *out_start = start_offset;
    if (out_end)
        *out_end = end_offset;

    return true;
}

/* ========================================================================
 * UTF-16 Code Unit Conversion (for LSP)
 * ======================================================================== */

size_t xr_utf8_utf16_to_byte_offset(const char *str, size_t len, size_t utf16_offset) {
    if (!str || len == 0)
        return 0;

    size_t byte_pos = 0;
    size_t utf16_pos = 0;

    while (byte_pos < len && utf16_pos < utf16_offset) {
        uint32_t cp;
        int utf8_size = xr_utf8_decode(str + byte_pos, len - byte_pos, &cp);

        // Count UTF-16 code units for this codepoint
        int utf16_size = (cp > 0xFFFF) ? 2 : 1;

        // Check if we would overshoot
        if (utf16_pos + utf16_size > utf16_offset) {
            // For surrogate pairs, landing in the middle means stay at current byte
            break;
        }

        utf16_pos += utf16_size;
        byte_pos += utf8_size;
    }

    return byte_pos;
}

size_t xr_utf8_byte_to_utf16_offset(const char *str, size_t len, size_t byte_offset) {
    if (!str || len == 0)
        return 0;
    if (byte_offset > len)
        byte_offset = len;

    size_t byte_pos = 0;
    size_t utf16_pos = 0;

    while (byte_pos < byte_offset) {
        uint32_t cp;
        int utf8_size = xr_utf8_decode(str + byte_pos, len - byte_pos, &cp);

        // Don't go past the target byte offset
        if (byte_pos + utf8_size > byte_offset)
            break;

        // Count UTF-16 code units for this codepoint
        utf16_pos += (cp > 0xFFFF) ? 2 : 1;
        byte_pos += utf8_size;
    }

    return utf16_pos;
}

/* ========================================================================
 * Validation
 * ======================================================================== */

bool xr_utf8_validate(const char *str, size_t len) {
    if (!str)
        return len == 0;

    size_t pos = 0;

    while (pos < len) {
        uint32_t cp;
        int size = xr_utf8_decode(str + pos, len - pos, &cp);

        // If decode returns INVALID, there's an error
        if (cp == XR_UNICODE_INVALID && size == 1) {
            uint8_t b = (uint8_t) str[pos];
            // If continuation byte or invalid first byte, validation fails
            if (xr_utf8_is_continuation(b) || (b & 0xC0) == 0xC0) {
                return false;
            }
        }

        pos += size;
    }

    return true;
}
