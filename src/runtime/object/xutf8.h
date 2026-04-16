/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xutf8.h - UTF-8 encoding/decoding utilities
 *
 * KEY CONCEPT:
 *   - Decode UTF-8 byte sequence to Unicode codepoint
 *   - Encode Unicode codepoint to UTF-8 byte sequence
 *   - Character-level string operations (length, index, iterate)
 *
 * UTF-8 ENCODING RULES:
 *   U+0000..U+007F     0xxxxxxx                              (1 byte)
 *   U+0080..U+07FF     110xxxxx 10xxxxxx                     (2 bytes)
 *   U+0800..U+FFFF     1110xxxx 10xxxxxx 10xxxxxx            (3 bytes)
 *   U+10000..U+10FFFF  11110xxx 10xxxxxx 10xxxxxx 10xxxxxx   (4 bytes)
 */

#ifndef XUTF8_H
#define XUTF8_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../base/xdefs.h"

#define XR_UNICODE_MAX 0x10FFFF
#define XR_UNICODE_INVALID 0xFFFD
#define XR_UTF8_MAX_BYTES 4

/* ========== UTF-8 Decode ========== */

// Get UTF-8 character byte length from first byte
static inline int xr_utf8_char_size(uint8_t first_byte) {
    if ((first_byte & 0x80) == 0x00) return 1; // 0xxxxxxx
    if ((first_byte & 0xE0) == 0xC0) return 2; // 110xxxxx
    if ((first_byte & 0xF0) == 0xE0) return 3; // 1110xxxx
    if ((first_byte & 0xF8) == 0xF0) return 4; // 11110xxx
    return 1;  // invalid sequence, treat as single byte
}

// Decode one UTF-8 char to codepoint, returns bytes consumed (1 on error)
XR_FUNC int xr_utf8_decode(const char *str, size_t len, uint32_t *out_cp);

// Get n-th character's codepoint and byte position
XR_FUNC bool xr_utf8_char_at(const char *str, size_t len, size_t index, 
                     uint32_t *out_cp, size_t *out_pos);

/* ========== UTF-8 Encode ========== */

// Encode codepoint to UTF-8, returns bytes written (0 on invalid)
XR_FUNC int xr_utf8_encode(uint32_t cp, char *out);

// Get bytes needed to encode codepoint
static inline int xr_utf8_encode_size(uint32_t cp) {
    if (cp <= 0x7F) return 1;       // U+0000..U+007F
    if (cp <= 0x7FF) return 2;      // U+0080..U+07FF
    if (cp <= 0xFFFF) return 3;     // U+0800..U+FFFF
    if (cp <= 0x10FFFF) return 4;   // U+10000..U+10FFFF
    return 0;  // invalid codepoint
}

/* ========== String-level Operations ========== */

// Count UTF-8 characters in string
XR_FUNC size_t xr_utf8_strlen(const char *str, size_t len);

// Convert character index to byte offset
XR_FUNC size_t xr_utf8_index_to_offset(const char *str, size_t len, size_t index);

// Convert byte offset to character index
XR_FUNC size_t xr_utf8_offset_to_index(const char *str, size_t len, size_t offset);

// Get byte range for character range [start_idx, end_idx)
XR_FUNC bool xr_utf8_char_range(const char *str, size_t len,
                        size_t start_idx, size_t end_idx,
                        size_t *out_start, size_t *out_end);

/* ========== UTF-16 Code Unit Conversion (for LSP) ========== */

// Get UTF-16 code units needed for a codepoint
// BMP (U+0000..U+FFFF): 1 code unit
// Supplementary (U+10000+): 2 code units (surrogate pair)
static inline int xr_utf16_codepoint_size(uint32_t cp) {
    return (cp > 0xFFFF) ? 2 : 1;
}

// Convert UTF-16 code unit offset to byte offset within a line
// (Used by LSP position to byte offset conversion)
XR_FUNC size_t xr_utf8_utf16_to_byte_offset(const char *str, size_t len, size_t utf16_offset);

// Convert byte offset to UTF-16 code unit offset within a line
// (Used by byte offset to LSP position conversion)
XR_FUNC size_t xr_utf8_byte_to_utf16_offset(const char *str, size_t len, size_t byte_offset);

/* ========== Validation ========== */

// Validate UTF-8 string
XR_FUNC bool xr_utf8_validate(const char *str, size_t len);

// Check if byte is UTF-8 continuation byte (10xxxxxx)
static inline bool xr_utf8_is_continuation(uint8_t byte) {
    return (byte & 0xC0) == 0x80;
}

#endif // XUTF8_H
