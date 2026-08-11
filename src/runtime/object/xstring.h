/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstring.h - Immutable string object with lazy sharing
 *
 * KEY CONCEPT:
 *   - Runtime strings are coroutine-local by default
 *   - Literals, symbols, explicit intern(), and map/set keys use canonical storage
 *   - Hash value cached at creation time
 *   - UTF-8 byte-level operations
 *   - Compact header + flexible array
 */

#ifndef XSTRING_H
#define XSTRING_H

#include "../value/xvalue.h"
#include "../abi/xr_runtime_string_object.h"
#include "xarray.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdatomic.h>
#include "../../os/os_thread.h"

// Forward declaration
struct XrArray;
struct XrRuntimeCore;

// Get string data pointer
#define XR_STRING_CHARS(s) ((s)->data)

/* ========== Short/Long String Separation ========== */

/*
 * Short runtime strings (≤64B): coroutine-local, no global lock
 * Canonical short strings: interned in the global pool, pointer fast path
 * Long strings (>64B): content comparison; shared when crossing boundaries
 * Classification lives in the descriptor-governed string traits field; the
 * canonical object header never stores family-private bits.
 */

// Short string max length
#define XR_SHORT_STR_MAX 64

// Check if long string
#define XR_STR_IS_LONG(s)                                                               \
    ((atomic_load_explicit(&(s)->traits, memory_order_relaxed) &                         \
      XR_RUNTIME_STRING_TRAIT_LONG) != 0)
#define XR_STR_IS_SHORT(s) (!XR_STR_IS_LONG(s))

// Set long string flag
#define XR_STR_SET_LONG(s)                                                              \
    ((void) atomic_fetch_or_explicit(&(s)->traits, XR_RUNTIME_STRING_TRAIT_LONG,         \
                                     memory_order_relaxed))

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

// String-family traits (stored outside the canonical object header)
#define STR_FLAG_INTERNED XR_RUNTIME_STRING_TRAIT_INTERNED
#define STR_FLAG_GLOBAL XR_RUNTIME_STRING_TRAIT_GLOBAL
#define STR_FLAG_LOCAL XR_RUNTIME_STRING_TRAIT_LOCAL
#define STR_FLAG_PERMANENT XR_RUNTIME_STRING_TRAIT_PERMANENT
#define STR_FLAG_ACCESSED XR_RUNTIME_STRING_TRAIT_ACCESSED

/* Pooled-string trait accesses use relaxed atomics on the family trait word:
 * the ACCESSED bit is set by concurrent lookups holding only the pool read
 * lock (LRU heuristic), so plain |= would be a racy RMW that could drop a
 * concurrent bit update. Same cast-to-_Atomic idiom as task->waiter. */
#define XR_STR_FLAGS_RELAXED(s)                                                      \
    atomic_load_explicit(&(s)->traits, memory_order_relaxed)

// Check macros
#define XR_STR_IS_INTERNED(s) (XR_STR_FLAGS_RELAXED(s) & STR_FLAG_INTERNED)
#define XR_STR_IS_GLOBAL(s) (XR_STR_FLAGS_RELAXED(s) & STR_FLAG_GLOBAL)
#define XR_STR_IS_LOCAL(s) (XR_STR_FLAGS_RELAXED(s) & STR_FLAG_LOCAL)
#define XR_STR_IS_PERMANENT(s) (XR_STR_FLAGS_RELAXED(s) & STR_FLAG_PERMANENT)
#define XR_STR_IS_ACCESSED(s) (XR_STR_FLAGS_RELAXED(s) & STR_FLAG_ACCESSED)

// Set macros
#define XR_STR_SET_FLAGS_RELAXED(s, bits)                                            \
    ((void) atomic_fetch_or_explicit(&(s)->traits, (uint16_t) (bits),                 \
                                     memory_order_relaxed))
#define XR_STR_SET_GLOBAL(s) XR_STR_SET_FLAGS_RELAXED(s, STR_FLAG_INTERNED | STR_FLAG_GLOBAL)
#define XR_STR_SET_LOCAL(s) XR_STR_SET_FLAGS_RELAXED(s, STR_FLAG_LOCAL)
#define XR_STR_SET_PERMANENT(s) XR_STR_SET_FLAGS_RELAXED(s, STR_FLAG_PERMANENT)
#define XR_STR_SET_ACCESSED(s) XR_STR_SET_FLAGS_RELAXED(s, STR_FLAG_ACCESSED)
#define XR_STR_CLR_ACCESSED(s)                                                      \
    ((void) atomic_fetch_and_explicit(&(s)->traits,                                 \
                                      (uint16_t) ~STR_FLAG_ACCESSED,                \
                                      memory_order_relaxed))

// XrGlobalStringPool - Global string intern pool (thread-safe)
typedef struct XrGlobalStringPool {
    XrString **entries;
    size_t capacity;
    size_t mask;
    size_t count;
    size_t permanent_count;  // compile-time constants (never evicted)
    xr_rwlock_t lock;
} XrGlobalStringPool;

// Global pool initial capacity
#define GLOBAL_POOL_INIT_CAPACITY 256

// Soft warn threshold: log once when pool exceeds this
#define GLOBAL_POOL_WARN_THRESHOLD (512 * 1024)

/* ========== Global Pool Operations ========== */

XR_FUNC void xr_global_pool_init(XrGlobalStringPool *pool);
XR_FUNC void xr_global_pool_free(XrGlobalStringPool *pool);
XR_FUNC XrString *xr_global_pool_insert_locked(XrGlobalStringPool *pool, const char *chars,
                                               size_t len, uint32_t hash);
XR_FUNC XrString *xr_global_pool_insert(XrGlobalStringPool *pool, XrVMRuntime *iso,
                                        const char *chars, size_t len, uint32_t hash);
XR_FUNC void xr_global_pool_freeze(XrGlobalStringPool *pool);
XR_FUNC XrString *xr_global_pool_lookup(XrGlobalStringPool *pool, const char *chars, size_t len,
                                        uint32_t hash);
XR_FUNC XrString *xr_string_intern_permanent(XrVMRuntime *iso, const char *chars, size_t len);
XR_FUNC size_t xr_global_pool_sweep(XrGlobalStringPool *pool);

/* ========== String Creation ========== */

// Create non-interned string (for large or one-shot data like HTTP body, WS message)
XR_FUNC XrString *xr_string_new(XrVMRuntime *iso, const char *chars, size_t length);
// Internal adapter for bytes already accepted by xr_utf8_scan_strict().
XR_FUNC XrString *xr_string_new_valid_utf8(XrVMRuntime *iso, const char *chars, size_t length,
                                           size_t rune_count);
XR_FUNC XrString *xr_string_new_raw_bytes(XrVMRuntime *iso, const char *bytes, size_t length);

XR_FUNC XrString *xr_string_concat(XrVMRuntime *iso, XrString *a, XrString *b);
XR_FUNC XrString *xr_string_from_int(XrVMRuntime *iso, xr_Integer i);
XR_FUNC XrString *xr_string_from_uint64(XrVMRuntime *iso, uint64_t i);
XR_FUNC XrString *xr_string_from_float(XrVMRuntime *iso, xr_Number n);

/* ========== String Interning ========== */

XR_FUNC XrString *xr_string_intern(XrVMRuntime *iso, const char *chars, size_t length,
                                   uint32_t hash);
XR_FUNC XrString *xr_string_intern_core(struct XrRuntimeCore *core, const char *chars,
                                        size_t length, uint32_t hash);
XR_FUNC XrString *xr_string_clone_shared_core(struct XrRuntimeCore *core, XrString *str);

/* ========== String Pool Management ========== */

XR_FUNC void xr_string_pool_init_internal(XrStringPool *pool);
XR_FUNC void xr_string_pool_free_internal(XrStringPool *pool);

/* ========== String Comparison ========== */

XR_FUNC bool xr_string_equal(XrString *a, XrString *b);

// Fast pointer comparison (valid only when the caller already knows both
// strings are canonical/interned or only identity matters).
static inline bool xr_string_equal_fast(XrString *a, XrString *b) {
    return a == b;
}

XR_FUNC int xr_string_compare(XrString *a, XrString *b);

/* ========== String Hash ========== */

XR_FUNC uint32_t xr_string_hash(const char *chars, size_t length);

/* ========== String Methods ========== */

XR_FUNC XrString *xr_string_rune_at(XrVMRuntime *iso, XrString *str, xr_Integer index);
XR_FUNC XrString *xr_string_substring(XrVMRuntime *iso, XrString *str, xr_Integer start,
                                      xr_Integer end);
XR_FUNC XrString *xr_string_slice(XrVMRuntime *iso, XrString *str, xr_Integer start,
                                  xr_Integer end);
XR_FUNC XrString *xr_string_slice_bytes(XrVMRuntime *iso, XrString *str, xr_Integer start,
                                        xr_Integer end);
XR_FUNC xr_Integer xr_string_index_of(XrVMRuntime *iso, XrString *str, XrString *substr);
XR_FUNC xr_Integer xr_string_index_of_from(XrVMRuntime *iso, XrString *str, XrString *substr,
                                           xr_Integer start);
XR_FUNC int xr_string_size(XrVMRuntime *iso, XrString *str);
XR_FUNC bool xr_string_is_empty(XrVMRuntime *iso, XrString *str);
XR_FUNC bool xr_string_has(XrVMRuntime *iso, XrString *str, XrString *substr);
XR_FUNC bool xr_string_starts_with(XrVMRuntime *iso, XrString *str, XrString *prefix);
XR_FUNC bool xr_string_ends_with(XrVMRuntime *iso, XrString *str, XrString *suffix);
XR_FUNC XrString *xr_string_to_lower_case(XrVMRuntime *iso, XrString *str);
XR_FUNC XrString *xr_string_to_upper_case(XrVMRuntime *iso, XrString *str);
XR_FUNC XrString *xr_string_trim(XrVMRuntime *iso, XrString *str);
XR_FUNC XrString *xr_string_trim_start(XrVMRuntime *iso, XrString *str);
XR_FUNC XrString *xr_string_trim_end(XrVMRuntime *iso, XrString *str);
XR_FUNC XrString *xr_string_pad_start(XrVMRuntime *iso, XrString *str, int64_t target_len,
                                      XrString *pad_str);
XR_FUNC XrString *xr_string_pad_end(XrVMRuntime *iso, XrString *str, int64_t target_len,
                                    XrString *pad_str);
XR_FUNC xr_Integer xr_string_last_index_of(XrVMRuntime *iso, XrString *str, XrString *substr);

/* ========== Advanced String Methods ========== */

XR_FUNC XrArray *xr_string_split(XrVMRuntime *iso, XrString *str, XrString *delimiter);
XR_FUNC XrString *xr_string_replace(XrVMRuntime *iso, XrString *str, XrString *old_str,
                                    XrString *new_str);
XR_FUNC XrString *xr_string_replace_all(XrVMRuntime *iso, XrString *str, XrString *old_str,
                                        XrString *new_str);
XR_FUNC XrString *xr_string_repeat(XrVMRuntime *iso, XrString *str, xr_Integer count);
XR_FUNC XrString *xr_string_reverse(XrVMRuntime *iso, XrString *str);
XR_FUNC XrString *xr_string_byte_at(XrVMRuntime *iso, XrString *str, xr_Integer index);
XR_FUNC XrString *xr_string_translate(XrVMRuntime *iso, XrString *str, struct XrMap *table);

// Note: join method is in xarray.h/c (array.join(","))

/* ========== Unicode / UTF-8 Support ========== */

XR_FUNC size_t xr_string_rune_length(XrString *str);

XR_FUNC int32_t xr_string_rune_code_at(XrString *str, size_t index);
XR_FUNC XrString *xr_string_rune_at_unicode(XrVMRuntime *iso, XrString *str, size_t index);
XR_FUNC XrString *xr_string_substring_by_rune(XrVMRuntime *iso, XrString *str, size_t start,
                                              size_t end);
XR_FUNC XrString *xr_string_from_codepoint(XrVMRuntime *iso, uint32_t codepoint);

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
