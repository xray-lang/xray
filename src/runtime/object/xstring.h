/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstring.h - Immutable string object with interning
 *
 * KEY CONCEPT:
 *   - Immutable strings with automatic interning
 *   - Hash value cached at creation time
 *   - UTF-8 byte-level operations
 *   - Compact 24-byte header + flexible array
 */

#ifndef XSTRING_H
#define XSTRING_H

#include "../value/xvalue.h"
#include "../gc/xgc_header.h"
#include "xarray.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>

// Forward declaration
struct XrArray;

/* ========== String Object ========== */

/*
 * Memory layout (24-byte header + data):
 *   [0-15]  GC header (16 bytes)
 *   [16-19] length (4 bytes, max 4GB)
 *   [20-23] hash (4 bytes, FNV-1a)
 *   [24+]   data[] (flexible array)
 */
typedef struct XrString {
    XrGCHeader gc;
    uint32_t length;
    uint32_t hash;
    char data[];
} XrString;

// Get string data pointer
#define XR_STRING_CHARS(s) ((s)->data)

// Static assert: ensure header size
_Static_assert(sizeof(XrString) == 24, "XrString header must be 24 bytes");

/* ========== Short/Long String Separation ========== */

/*
 * Short strings (≤64B): interned, pointer comparison O(1)
 * Long strings (>64B): not interned, content comparison O(n)
 * Flag stored in gc.extra lowest bit
 */

// Short string max length
#define XR_SHORT_STR_MAX 64

// Long string flag (stored in gc.extra bit 4; bit 0 is reserved for GC storage mode)
#define XR_STR_LONG_FLAG 0x10

// Check if long string
#define XR_STR_IS_LONG(s) ((s)->gc.extra & XR_STR_LONG_FLAG)
#define XR_STR_IS_SHORT(s) (!XR_STR_IS_LONG(s))

// Set long string flag
#define XR_STR_SET_LONG(s) ((s)->gc.extra |= XR_STR_LONG_FLAG)

/* ========== String Interning Pool ========== */

// Open-addressing hash table (linear probing)
typedef struct XrStringPool {
    XrString **entries;
    size_t capacity;  // Always power of 2
    size_t mask;      // capacity - 1
    size_t count;
    size_t threshold;  // Resize at capacity * 0.75
} XrStringPool;

// String pool constants
#define STRING_POOL_INIT_CAPACITY 128
#define STRING_POOL_LOAD_FACTOR 0.75

/* ========== Tiered String Pool (Thread-safe) ========== */

// String flags (stored in gc.extra)
#define STR_FLAG_INTERNED 0x02
#define STR_FLAG_GLOBAL 0x04
#define STR_FLAG_LOCAL 0x08
#define STR_FLAG_PERMANENT 0x20  // compile-time constant, never evicted
#define STR_FLAG_ACCESSED 0x40   // touched since last sweep cycle

// Check macros
#define XR_STR_IS_INTERNED(s) ((s)->gc.extra & STR_FLAG_INTERNED)
#define XR_STR_IS_GLOBAL(s) ((s)->gc.extra & STR_FLAG_GLOBAL)
#define XR_STR_IS_LOCAL(s) ((s)->gc.extra & STR_FLAG_LOCAL)
#define XR_STR_IS_PERMANENT(s) ((s)->gc.extra & STR_FLAG_PERMANENT)
#define XR_STR_IS_ACCESSED(s) ((s)->gc.extra & STR_FLAG_ACCESSED)

// Set macros
#define XR_STR_SET_GLOBAL(s) ((s)->gc.extra |= (STR_FLAG_INTERNED | STR_FLAG_GLOBAL))
#define XR_STR_SET_LOCAL(s) ((s)->gc.extra |= (STR_FLAG_INTERNED | STR_FLAG_LOCAL))
#define XR_STR_SET_PERMANENT(s) ((s)->gc.extra |= STR_FLAG_PERMANENT)
#define XR_STR_SET_ACCESSED(s) ((s)->gc.extra |= STR_FLAG_ACCESSED)
#define XR_STR_CLR_ACCESSED(s) ((s)->gc.extra &= (uint16_t) ~STR_FLAG_ACCESSED)

// XrGlobalStringPool - Global string intern pool (thread-safe)
typedef struct XrGlobalStringPool {
    XrString **entries;
    size_t capacity;
    size_t mask;
    size_t count;
    size_t permanent_count;  // compile-time constants (never evicted)
    pthread_rwlock_t lock;
} XrGlobalStringPool;

// Global pool initial capacity
#define GLOBAL_POOL_INIT_CAPACITY 256

// Soft warn threshold: log once when pool exceeds this
#define GLOBAL_POOL_WARN_THRESHOLD (512 * 1024)

/* ========== Global Pool Operations ========== */

XR_FUNC void xr_global_pool_init(XrGlobalStringPool *pool);
XR_FUNC void xr_global_pool_free(XrGlobalStringPool *pool);
XR_FUNC XrString *xr_global_pool_insert(XrGlobalStringPool *pool, XrayIsolate *iso,
                                        const char *chars, size_t len, uint32_t hash);
XR_FUNC void xr_global_pool_freeze(XrGlobalStringPool *pool);
XR_FUNC XrString *xr_global_pool_lookup(XrGlobalStringPool *pool, const char *chars, size_t len,
                                        uint32_t hash);
XR_FUNC XrString *xr_compile_time_intern(XrayIsolate *iso, const char *chars, size_t len);
XR_FUNC size_t xr_global_pool_sweep(XrGlobalStringPool *pool);

/* ========== String Creation ========== */

// Create non-interned string (for large or one-shot data like HTTP body, WS message)
XR_FUNC XrString *xr_string_new(XrayIsolate *iso, const char *chars, size_t length);

XR_FUNC XrString *xr_string_concat(XrayIsolate *iso, XrString *a, XrString *b);
XR_FUNC XrString *xr_string_from_int(XrayIsolate *iso, xr_Integer i);
XR_FUNC XrString *xr_string_from_float(XrayIsolate *iso, xr_Number n);

/* ========== String Interning ========== */

XR_FUNC XrString *xr_string_intern(XrayIsolate *iso, const char *chars, size_t length,
                                   uint32_t hash);

/* ========== String Pool Management ========== */

XR_FUNC void xr_string_pool_init_internal(XrStringPool *pool);
XR_FUNC void xr_string_pool_free_internal(XrStringPool *pool);

/* ========== String Comparison ========== */

XR_FUNC bool xr_string_equal(XrString *a, XrString *b);

// Fast pointer comparison (for interned strings in same pool)
static inline bool xr_string_equal_fast(XrString *a, XrString *b) {
    return a == b;
}

XR_FUNC int xr_string_compare(XrString *a, XrString *b);

/* ========== String Hash ========== */

XR_FUNC uint32_t xr_string_hash(const char *chars, size_t length);

/* ========== String Methods ========== */

XR_FUNC XrString *xr_string_char_at(XrayIsolate *iso, XrString *str, xr_Integer index);
XR_FUNC XrString *xr_string_substring(XrayIsolate *iso, XrString *str, xr_Integer start,
                                      xr_Integer end);
XR_FUNC XrString *xr_string_slice(XrayIsolate *iso, XrString *str, xr_Integer start,
                                  xr_Integer end);
XR_FUNC xr_Integer xr_string_index_of(XrayIsolate *iso, XrString *str, XrString *substr);
XR_FUNC int xr_string_size(XrayIsolate *iso, XrString *str);
XR_FUNC bool xr_string_is_empty(XrayIsolate *iso, XrString *str);
XR_FUNC bool xr_string_has(XrayIsolate *iso, XrString *str, XrString *substr);
XR_FUNC bool xr_string_starts_with(XrayIsolate *iso, XrString *str, XrString *prefix);
XR_FUNC bool xr_string_ends_with(XrayIsolate *iso, XrString *str, XrString *suffix);
XR_FUNC XrString *xr_string_to_lower_case(XrayIsolate *iso, XrString *str);
XR_FUNC XrString *xr_string_to_upper_case(XrayIsolate *iso, XrString *str);
XR_FUNC XrString *xr_string_trim(XrayIsolate *iso, XrString *str);
XR_FUNC XrString *xr_string_trim_start(XrayIsolate *iso, XrString *str);
XR_FUNC XrString *xr_string_trim_end(XrayIsolate *iso, XrString *str);
XR_FUNC XrString *xr_string_pad_start(XrayIsolate *iso, XrString *str, size_t target_len,
                                      XrString *pad_str);
XR_FUNC XrString *xr_string_pad_end(XrayIsolate *iso, XrString *str, size_t target_len,
                                    XrString *pad_str);
XR_FUNC xr_Integer xr_string_last_index_of(XrayIsolate *iso, XrString *str, XrString *substr);

/* ========== Advanced String Methods ========== */

XR_FUNC XrArray *xr_string_split(XrayIsolate *iso, XrString *str, XrString *delimiter);
XR_FUNC XrString *xr_string_replace(XrayIsolate *iso, XrString *str, XrString *old_str,
                                    XrString *new_str);
XR_FUNC XrString *xr_string_replace_all(XrayIsolate *iso, XrString *str, XrString *old_str,
                                        XrString *new_str);
XR_FUNC XrString *xr_string_repeat(XrayIsolate *iso, XrString *str, xr_Integer count);
XR_FUNC XrString *xr_string_reverse(XrayIsolate *iso, XrString *str);
XR_FUNC XrString *xr_string_reverse_bytes(XrayIsolate *iso, XrString *str);
XR_FUNC XrString *xr_string_byte_at(XrayIsolate *iso, XrString *str, xr_Integer index);
XR_FUNC XrString *xr_string_translate_bytes(XrayIsolate *iso, XrString *str, struct XrMap *table);
XR_FUNC XrString *xr_string_translate(XrayIsolate *iso, XrString *str, struct XrMap *table);

// Note: join method is in xarray.h/c (array.join(","))

/* ========== Unicode / UTF-8 Support ========== */

XR_FUNC size_t xr_string_char_length(XrString *str);

XR_FUNC int32_t xr_string_char_code_at(XrString *str, size_t index);
XR_FUNC XrString *xr_string_char_at_unicode(XrayIsolate *iso, XrString *str, size_t index);
XR_FUNC XrString *xr_string_substring_by_char(XrayIsolate *iso, XrString *str, size_t start,
                                              size_t end);
XR_FUNC XrString *xr_string_from_codepoint(XrayIsolate *iso, uint32_t codepoint);

/* ========== Character Classification ========== */

XR_FUNC bool xr_string_is_letter(XrString *str);
XR_FUNC bool xr_string_is_number(XrString *str);
XR_FUNC bool xr_string_is_alnum(XrString *str);
XR_FUNC bool xr_string_is_whitespace_str(XrString *str);
XR_FUNC int32_t xr_string_ord(XrString *str);

/* ========== Helper Functions ========== */

static inline bool xr_is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

XR_FUNC void xr_string_print(XrString *str);

#endif  // XSTRING_H
