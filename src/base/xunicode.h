/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xunicode.h - Unicode character classification with fast path optimization
 *
 * KEY CONCEPT:
 *   Provides Unicode character classification using range tables.
 *   ASCII characters use O(1) fast path, non-ASCII use O(log n) binary search.
 *
 * SUPPORTED CATEGORIES:
 *   General Category:
 *     L  - Letter (includes Lu, Ll)
 *     Lu - Uppercase Letter
 *     Ll - Lowercase Letter
 *     N  - Number
 *     Nd - Decimal Number
 *     P  - Punctuation
 *     S  - Symbol
 *     Z  - Separator (whitespace)
 *     C  - Other (control chars)
 *
 *   Scripts:
 *     Han, Hiragana, Katakana, Latin, Greek, Cyrillic, Arabic, Hebrew
 *
 *   Special:
 *     ASCII - 0x00-0x7F
 *     Any   - all Unicode
 */

#ifndef XUNICODE_H
#define XUNICODE_H

#include <stdint.h>
#include <stdbool.h>
#include "xdefs.h"

/* ========== Unicode Range ========== */

typedef struct {
    uint32_t lo;
    uint32_t hi;
} XrUnicodeRange;

/* ========== Unicode Property Enum ========== */

typedef enum {
    XR_UP_INVALID = 0,
    
    // General Category
    XR_UP_L,        // Letter
    XR_UP_Lu,       // Uppercase Letter
    XR_UP_Ll,       // Lowercase Letter
    XR_UP_N,        // Number
    XR_UP_Nd,       // Decimal Number
    XR_UP_P,        // Punctuation
    XR_UP_S,        // Symbol
    XR_UP_Z,        // Separator
    XR_UP_C,        // Other
    
    // Scripts
    XR_UP_Han,      // CJK Unified Ideographs
    XR_UP_Hiragana,
    XR_UP_Katakana,
    XR_UP_Latin,
    XR_UP_Greek,
    XR_UP_Cyrillic,
    XR_UP_Arabic,
    XR_UP_Hebrew,
    
    // Special
    XR_UP_ASCII,    // 0x00-0x7F
    XR_UP_Any,      // all Unicode
    
    XR_UP_COUNT
} XrUnicodeProperty;

/* ========== Core API ========== */

// Lookup property by name ("L", "Han", "Latin", etc.)
XR_FUNC XrUnicodeProperty xr_unicode_property_lookup(const char *name, int len);

// Get range table for property
XR_FUNC bool xr_unicode_property_ranges(XrUnicodeProperty prop,
                                const XrUnicodeRange **out_ranges,
                                int *out_count);

// Check if codepoint has property (uses binary search)
XR_FUNC bool xr_unicode_is_property(uint32_t cp, XrUnicodeProperty prop);

// Get property name (for debug)
XR_FUNC const char* xr_unicode_property_name(XrUnicodeProperty prop);

/* ========== Fast Path Classification ========== */
// ASCII fast path O(1), non-ASCII falls back to range table O(log n)

static inline bool xr_unicode_is_letter(uint32_t cp) {
    if (cp < 0x80) {
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
    }
    return xr_unicode_is_property(cp, XR_UP_L);
}

static inline bool xr_unicode_is_upper(uint32_t cp) {
    if (cp < 0x80) {
        return cp >= 'A' && cp <= 'Z';
    }
    return xr_unicode_is_property(cp, XR_UP_Lu);
}

static inline bool xr_unicode_is_lower(uint32_t cp) {
    if (cp < 0x80) {
        return cp >= 'a' && cp <= 'z';
    }
    return xr_unicode_is_property(cp, XR_UP_Ll);
}

static inline bool xr_unicode_is_number(uint32_t cp) {
    if (cp < 0x80) {
        return cp >= '0' && cp <= '9';
    }
    return xr_unicode_is_property(cp, XR_UP_Nd);
}

static inline bool xr_unicode_is_alnum(uint32_t cp) {
    if (cp < 0x80) {
        return (cp >= 'A' && cp <= 'Z') || 
               (cp >= 'a' && cp <= 'z') || 
               (cp >= '0' && cp <= '9');
    }
    return xr_unicode_is_property(cp, XR_UP_L) || 
           xr_unicode_is_property(cp, XR_UP_Nd);
}

static inline bool xr_unicode_is_whitespace(uint32_t cp) {
    if (cp < 0x80) {
        return cp == ' ' || cp == '\t' || cp == '\n' || 
               cp == '\r' || cp == '\f' || cp == '\v';
    }
    return xr_unicode_is_property(cp, XR_UP_Z);
}

static inline bool xr_unicode_is_punct(uint32_t cp) {
    if (cp < 0x80) {
        return (cp >= 0x21 && cp <= 0x2F) ||  // !"#$%&'()*+,-./
               (cp >= 0x3A && cp <= 0x40) ||  // :;<=>?@
               (cp >= 0x5B && cp <= 0x60) ||  // [\]^_`
               (cp >= 0x7B && cp <= 0x7E);    // {|}~
    }
    return xr_unicode_is_property(cp, XR_UP_P);
}

/* ========== Case Conversion ========== */
// ASCII only for now, full Unicode would need case mapping tables

static inline uint32_t xr_unicode_toupper(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 32;
    return cp;
}

static inline uint32_t xr_unicode_tolower(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    return cp;
}

#endif // XUNICODE_H
