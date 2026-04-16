/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstring.c - Immutable string implementation with interning
 *
 * KEY CONCEPT:
 *   - Short strings (<=64B): interned in global pool
 *   - Long strings (>64B): shared on system heap
 *   - FNV-1a hash cached at creation time
 */

#include "../value/xtype.h"
#include "../../base/xmalloc.h"
#include "../../base/xlog.h"
#include "../../base/xhash.h"
#include "xarray.h"
#include "xstring.h"
#include "xmap.h"
#include "xutf8.h"
#include "../xstrbuf.h"
#include "xstringbuilder.h"
#include "../xisolate_api.h"
#include "../xisolate_api.h"
#include "../../base/xchecks.h"
#include "../class/xclass_system.h"  
#include "../class/xclass.h"
#include "../gc/xgc.h"
#include "../gc/xsystem_heap.h"
#include "../xshared.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include "../../base/xsimd.h"


/* ========== Global String Pool Management ========== */

// Initialize global string pool
void xr_global_pool_init(XrGlobalStringPool *pool) {
    if (!pool) return;
    
    pool->capacity = GLOBAL_POOL_INIT_CAPACITY;
    pool->mask = pool->capacity - 1;
    pool->count = 0;
    pool->permanent_count = 0;
    pool->entries = (XrString**)xr_malloc(sizeof(XrString*) * pool->capacity);
    
    // Initialize rwlock
    pthread_rwlock_init(&pool->lock, NULL);
    
    // Initialize to NULL
    for (size_t i = 0; i < pool->capacity; i++) {
        pool->entries[i] = NULL;
    }
}

// Free global string pool
void xr_global_pool_free(XrGlobalStringPool *pool) {
    if (!pool || !pool->entries) return;
    
    // Destroy rwlock
    pthread_rwlock_destroy(&pool->lock);
    
    // Free all globally allocated strings
    for (size_t i = 0; i < pool->capacity; i++) {
        if (pool->entries[i] != NULL) {
            xr_free(pool->entries[i]);
        }
    }
    
    // Free hash table structure
    xr_free(pool->entries);
    pool->entries = NULL;
    pool->capacity = 0;
    pool->count = 0;
}

// Global pool resize (internal)
static void global_pool_grow(XrGlobalStringPool *pool) {
    if (!pool) return;
    XR_DCHECK((pool->capacity & (pool->capacity - 1)) == 0, "global_pool_grow: capacity not power-of-2");
    
    size_t old_capacity = pool->capacity;
    XrString **old_entries = pool->entries;
    
    // Double capacity
    pool->capacity = old_capacity * 2;
    pool->mask = pool->capacity - 1;
    pool->entries = (XrString**)xr_malloc(sizeof(XrString*) * pool->capacity);
    
    // Initialize to NULL
    for (size_t i = 0; i < pool->capacity; i++) {
        pool->entries[i] = NULL;
    }
    
    // Rehash all strings
    size_t saved_count = pool->count;
    pool->count = 0;
    size_t mask = pool->mask;
    for (size_t i = 0; i < old_capacity; i++) {
        XrString *str = old_entries[i];
        if (str != NULL) {
            uint32_t index = str->hash & mask;
            while (pool->entries[index] != NULL) {
                index = (index + 1) & mask;
            }
            pool->entries[index] = str;
            pool->count++;
        }
    }
    
    XR_DCHECK(pool->count == saved_count, "global_pool_grow: count mismatch after rehash");
    xr_free(old_entries);
}

// Insert string to global pool (internal, requires write lock)
// Uses global allocation (xr_malloc), not on coroutine heap
static XrString* global_pool_insert_unlocked(XrGlobalStringPool *pool,
                                              const char *chars, size_t len, uint32_t hash) {
    if (!pool) return NULL;
    
    // Soft warn threshold (no hard limit, pool grows dynamically)
    if (pool->count >= GLOBAL_POOL_WARN_THRESHOLD) {
        static _Atomic int warned = 0;
        if (!warned) {
            warned = 1;
            xr_log_warning("string", "XrGlobalStringPool: %zu strings (permanent: %zu), consider investigating",
                    pool->count, pool->permanent_count);
        }
    }
    
    // Compute hash if not provided
    if (hash == 0) {
        hash = xr_string_hash(chars, len);
    }
    
    // Check if already exists
    size_t mask = pool->mask;
    uint32_t index = hash & mask;
    
    for (;;) {
        XrString *entry = pool->entries[index];
        
        if (entry == NULL) {
            // Not found, create new string with global allocation
            size_t total_size = sizeof(XrString) + len + 1;
            XrString *str = (XrString*)xr_malloc(total_size);
            if (!str) return NULL;
            
            // Initialize GC header
            memset(&str->gc, 0, sizeof(XrGCHeader));
            str->gc.type = XR_TSTRING;
            str->gc.marked = 1;  // Mark as alive, not collected by coroutine GC
            str->gc.objsize = (uint32_t)total_size;
            
            str->length = (uint32_t)len;
            str->hash = hash;
            memcpy(str->data, chars, len);
            str->data[len] = '\0';
            
            // Set global pool flag AND shared storage flag
            // XR_GC_STORAGE_SHARED ensures coroutine GC skips this object
            XR_STR_SET_GLOBAL(str);
            XR_GC_SET_STORAGE(&str->gc, XR_GC_STORAGE_SHARED);
            
            // Insert into pool
            pool->entries[index] = str;
            pool->count++;
            
            // Check if resize needed (load factor 0.75)
            if (pool->count > pool->capacity * 3 / 4) {
                global_pool_grow(pool);
            }
            
            return str;
        }
        
        // Check if match
        if (entry->length == len && entry->hash == hash &&
            memcmp(entry->data, chars, len) == 0) {
            return entry;
        }
        
        // Collision, linear probe
        index = (index + 1) & mask;
    }
}

// Compile-time insert string to global pool (lockless, single-threaded)
// Strings inserted here are marked as PERMANENT (compile-time constants)
XrString* xr_global_pool_insert(XrGlobalStringPool *pool, XrayIsolate *iso,
                                 const char *chars, size_t len, uint32_t hash) {
    XR_DCHECK(pool != NULL, "global_pool_insert: NULL pool");
    XR_DCHECK(chars != NULL, "global_pool_insert: NULL chars");
    (void)iso;
    XrString *str = global_pool_insert_unlocked(pool, chars, len, hash);
    if (str && !XR_STR_IS_PERMANENT(str)) {
        XR_STR_SET_PERMANENT(str);
        pool->permanent_count++;
    }
    return str;
}

// Freeze global pool (kept for interface compatibility)
void xr_global_pool_freeze(XrGlobalStringPool *pool) {
    (void)pool;  // No longer needed, global pool supports runtime writes
}

// Global pool lookup (read-only, lockless)
XrString* xr_global_pool_lookup(XrGlobalStringPool *pool,
                                 const char *chars, size_t len, uint32_t hash) {
    if (!pool || !pool->entries) return NULL;
    
    size_t mask = pool->mask;
    uint32_t index = hash & mask;
    
    for (;;) {
        XrString *entry = pool->entries[index];
        
        if (entry == NULL) {
            return NULL;  // Not found
        }
        
        if (entry->length == len && entry->hash == hash &&
            memcmp(entry->data, chars, len) == 0) {
            // Mark as accessed for sweep-based eviction
            if (!XR_STR_IS_PERMANENT(entry))
                XR_STR_SET_ACCESSED(entry);
            return entry;  // Found
        }
        
        index = (index + 1) & mask;
    }
}

// Compile-time string intern (write to global pool)
// Called only at compile time, lookup first then insert
XrString* xr_compile_time_intern(XrayIsolate *iso, const char *chars, size_t len) {
    XrGlobalStringPool *pool = xr_isolate_get_string_pool(iso);
    if (!pool) {
        xr_log_warning("string", "compile_time_intern: isolate or global pool is NULL");
        return NULL;
    }
    
    // Compute hash
    uint32_t hash = xr_string_hash(chars, len);
    
    // Lookup first
    XrString *found = xr_global_pool_lookup(pool, chars, len, hash);
    if (found) {
        return found;
    }
    
    // Insert to global pool
    return xr_global_pool_insert(pool, iso, chars, len, hash);
}

/*
 * Sweep global string pool: evict non-permanent, non-accessed runtime strings.
 * Must be called with write lock held (or during single-threaded shutdown).
 * Returns the number of strings evicted.
 *
 * Algorithm: scan all entries, free non-permanent+non-accessed strings,
 * then rehash remaining entries to fix linear probe chains broken by deletions.
 */
size_t xr_global_pool_sweep(XrGlobalStringPool *pool) {
    if (!pool || !pool->entries) return 0;
    
    pthread_rwlock_wrlock(&pool->lock);
    
    size_t evicted = 0;
    size_t cap = pool->capacity;
    
    // Pass 1: mark entries for deletion
    for (size_t i = 0; i < cap; i++) {
        XrString *str = pool->entries[i];
        if (!str) continue;
        
        if (XR_STR_IS_PERMANENT(str)) {
            // Permanent strings: never evicted, clear accessed bit
            XR_STR_CLR_ACCESSED(str);
            continue;
        }
        
        if (XR_STR_IS_ACCESSED(str)) {
            // Accessed this cycle: keep, clear flag for next cycle
            XR_STR_CLR_ACCESSED(str);
            continue;
        }
        
        // Not permanent, not accessed: evict
        xr_free(str);
        pool->entries[i] = NULL;
        pool->count--;
        evicted++;
    }
    
    if (evicted > 0) {
        // Pass 2: rehash (and possibly shrink) to fix broken probe chains.
        size_t old_cap = cap;
        XrString **old = pool->entries;
        
        // Shrink if load factor < 25% and capacity > initial
        size_t new_cap = cap;
        if (pool->count < cap / 4 && cap > GLOBAL_POOL_INIT_CAPACITY) {
            // Find smallest power-of-2 >= count*2 (keeps load < 50%)
            new_cap = GLOBAL_POOL_INIT_CAPACITY;
            while (new_cap < pool->count * 2) new_cap *= 2;
            if (new_cap > cap) new_cap = cap;  // safety: never grow in sweep
        }
        
        pool->capacity = new_cap;
        pool->mask = new_cap - 1;
        pool->entries = (XrString**)xr_malloc(sizeof(XrString*) * new_cap);
        for (size_t i = 0; i < new_cap; i++) pool->entries[i] = NULL;
        
        size_t new_count = 0;
        size_t mask = pool->mask;
        for (size_t i = 0; i < old_cap; i++) {
            XrString *str = old[i];
            if (!str) continue;
            uint32_t idx = str->hash & mask;
            while (pool->entries[idx] != NULL)
                idx = (idx + 1) & mask;
            pool->entries[idx] = str;
            new_count++;
        }
        pool->count = new_count;
        xr_free(old);
    }
    
    pthread_rwlock_unlock(&pool->lock);
    return evicted;
}

/* ========== String Pool Management ========== */

// Initialize string pool (Isolate internal use)
void xr_string_pool_init_internal(XrStringPool *pool) {
    if (!pool) return;
    
    pool->capacity = STRING_POOL_INIT_CAPACITY;
    pool->mask = pool->capacity - 1;  // For bitwise modulo
    pool->count = 0;
    pool->threshold = (size_t)(pool->capacity * STRING_POOL_LOAD_FACTOR);
    pool->entries = (XrString**)xr_malloc(
        sizeof(XrString*) * pool->capacity
    );
    
    // Initialize to NULL
    for (size_t i = 0; i < pool->capacity; i++) {
        pool->entries[i] = NULL;
    }
}

// Free string pool (Isolate internal use)
// Note: String objects are GC managed, only free hash table structure
void xr_string_pool_free_internal(XrStringPool *pool) {
    if (!pool || pool->entries == NULL) return;
    
    // Free table (not String objects themselves)
    xr_free(pool->entries);
    pool->entries = NULL;
    pool->capacity = 0;
    pool->count = 0;
}

/* ========== String Hash ========== */

// Delegate to unified hash function
uint32_t xr_string_hash(const char *chars, size_t length) {
    return xr_hash_bytes(chars, length);
}

/* ========== String Creation ========== */

// Allocate string object on coroutine heap (shared by both paths)
static XrString* string_alloc(XrayIsolate *iso, const char *chars, size_t length) {
    if (length > UINT32_MAX) return NULL;
    
    size_t total_size = sizeof(XrString) + length + 1;
    XrCoroutine *coro = iso ? xr_current_coro(iso) : NULL;
    
    XrString *str = (XrString*)xr_alloc(coro, total_size, XR_TSTRING);
    if (!str) return NULL;
    
    str->length = (uint32_t)length;
    memcpy(str->data, chars, length);
    str->data[length] = '\0';
    
    return str;
}

// Create non-interned string (lazy hash: computed on first use)
XrString* xr_string_new(XrayIsolate *iso, const char *chars, size_t length) {
    XR_DCHECK(iso != NULL, "string_new: NULL isolate");
    XR_DCHECK(length == 0 || chars != NULL, "string_new: NULL chars with length > 0");
    XrString *str = string_alloc(iso, chars, length);
    if (!str) return NULL;
    str->hash = 0;  // Lazy: computed when needed
    return str;
}

// String interning (short/long separation)
// Short strings (<=64B): global pool with rwlock
// Long strings (>64B): coroutine heap allocation
XrString* xr_string_intern(XrayIsolate *iso, const char *chars, size_t length, uint32_t hash) {
    if (!iso) {
        xr_log_warning("string", "string_intern: isolate is NULL");
        abort();
    }
    
    // Compute hash if not provided
    if (hash == 0) {
        hash = xr_string_hash(chars, length);
    }
    
    // Long string: not interned, allocate as shared on system heap.
    // Strings are immutable → safe for concurrent read by multiple coroutines.
    // Refcount managed by per-coroutine GC shared_refs tracking.
    if (length > XR_SHORT_STR_MAX) {
        XrString *str = NULL;
        if (xr_isolate_get_sys_heap(iso)) {
            size_t total_size = sizeof(XrString) + length + 1;
            str = (XrString*)xr_sysheap_alloc_shared(xr_isolate_get_sys_heap(iso), total_size, XR_TSTRING);
            if (str) {
                str->length = (uint32_t)length;
                str->hash = hash;
                memcpy(str->data, chars, length);
                str->data[length] = '\0';
                XR_STR_SET_LONG(str);
                xr_shared_set_refc(&str->gc, 1);
            }
        } else {
            // Fallback: coroutine heap (during early init before sys_heap)
            str = string_alloc(iso, chars, length);
            if (str) {
                str->hash = hash;
                XR_STR_SET_LONG(str);
            }
        }
        return str;
    }
    
    XrGlobalStringPool *pool = xr_isolate_get_string_pool(iso);
    if (!pool) {
        // No global pool, fallback to normal allocation
        XrString *s = string_alloc(iso, chars, length);
        if (s) s->hash = hash;
        return s;
    }
    
    // Step 1: Read lock lookup
    pthread_rwlock_rdlock(&pool->lock);
    XrString *found = xr_global_pool_lookup(pool, chars, length, hash);
    pthread_rwlock_unlock(&pool->lock);
    
    if (found) {
        return found;
    }
    
    // Step 2: Write lock insert (double-check pattern)
    pthread_rwlock_wrlock(&pool->lock);
    
    // Check again (another thread may have inserted)
    found = xr_global_pool_lookup(pool, chars, length, hash);
    if (found) {
        pthread_rwlock_unlock(&pool->lock);
        return found;
    }
    
    // Insert new string
    XrString *str = global_pool_insert_unlocked(pool, chars, length, hash);
    pthread_rwlock_unlock(&pool->lock);
    
    // If global pool full, fallback to coroutine heap
    if (!str) {
        str = string_alloc(iso, chars, length);
        if (str) str->hash = hash;
    }
    
    return str;
}

// Concatenate two strings (uses XrStrBuf for efficiency)
XrString* xr_string_concat(XrayIsolate *iso, XrString *a, XrString *b) {
    XR_DCHECK(iso != NULL, "string_concat: NULL isolate");
    if (a == NULL || b == NULL) return NULL;
    
    XrStrBuf *sb = xr_strbuf_tmp(iso);
    xr_strbuf_reserve(sb, a->length + b->length);
    xr_strbuf_append_str(sb, a);
    xr_strbuf_append_str(sb, b);
    return xr_strbuf_to_string(sb);
}

// Fast integer to string (without snprintf)
static inline int fast_int_to_str(xr_Integer i, char *buffer) {
    char *p = buffer;
    int neg = 0;
    
    if (i < 0) {
        neg = 1;
        i = -i;
    }
    
    // Write digits in reverse
    char *start = p;
    do {
        *p++ = '0' + (i % 10);
        i /= 10;
    } while (i > 0);
    
    if (neg) {
        *p++ = '-';
    }
    
    int len = (int)(p - start);
    *p = '\0';
    
    // Reverse string
    char *end = p - 1;
    while (start < end) {
        char tmp = *start;
        *start = *end;
        *end = tmp;
        start++;
        end--;
    }
    
    return len;
}

// Create string from integer
XrString* xr_string_from_int(XrayIsolate *iso, xr_Integer i) {
    XR_DCHECK(iso != NULL, "string_from_int: NULL isolate");
    char buffer[32];
    int len = fast_int_to_str(i, buffer);
    return xr_string_intern(iso, buffer, len, 0);
}

// Create string from float
XrString* xr_string_from_float(XrayIsolate *iso, xr_Number n) {
    XR_DCHECK(iso != NULL, "string_from_float: NULL isolate");
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.15g", n);
    return xr_string_intern(iso, buffer, strlen(buffer), 0);
}

/* ========== String Comparison ========== */

// String equality comparison (optimized)
// Priority: pointer -> length -> hash -> pool check -> content
bool xr_string_equal(XrString *a, XrString *b) {
    XR_DCHECK(a != NULL && b != NULL, "string_equal: NULL argument");
    // Pointer equal (fastest path for interned strings)
    if (a == b) return true;
    
    if (a == NULL || b == NULL) return false;
    
    // Length not equal, fast reject
    if (a->length != b->length) return false;
    
    // Hash not equal, fast reject
    if (a->hash != b->hash) return false;
    
    // Same pool interned: pointer not equal means content not equal
    if (XR_STR_IS_GLOBAL(a) && XR_STR_IS_GLOBAL(b)) {
        return false;  // Pointer not equal, content not equal
    }
    
    // Local pool strings: need content comparison for cross-pool
    if (XR_STR_IS_LOCAL(a) && XR_STR_IS_LOCAL(b)) {
        // Cannot determine if same pool, conservatively do content compare
    }
    
    // Content comparison (long strings or cross-pool)
    return memcmp(a->data, b->data, a->length) == 0;
}

// String lexicographic comparison
int xr_string_compare(XrString *a, XrString *b) {
    XR_DCHECK(a != NULL && b != NULL, "string_compare: NULL argument");
    if (a == NULL || b == NULL) {
        if (a == b) return 0;
        return a == NULL ? -1 : 1;
    }
    
    if (a == b) return 0;
    
    size_t min_len = a->length < b->length ? a->length : b->length;
    int cmp = memcmp(a->data, b->data, min_len);
    
    if (cmp != 0) return cmp;
    
    // Different length
    if (a->length < b->length) return -1;
    if (a->length > b->length) return 1;
    return 0;
}

/* ========== String Basic Methods ========== */

// charAt - get character at position (supports negative index)
XrString* xr_string_char_at(XrayIsolate *iso, XrString *str, xr_Integer index) {
    XR_DCHECK(iso != NULL, "string_char_at: NULL isolate");
    if (str == NULL || str->length == 0) return NULL;
    
    // Handle negative index
    if (index < 0) {
        index = (xr_Integer)str->length + index;
    }
    
    // Bounds check
    if (index < 0 || (size_t)index >= str->length) {
        return NULL;
    }
    
    // Create single character string
    return xr_string_intern(iso, &str->data[index], 1, 0);
}

// substring - extract substring
XrString* xr_string_substring(XrayIsolate *iso, XrString *str, xr_Integer start, xr_Integer end) {
    XR_DCHECK(iso != NULL, "string_substring: NULL isolate");
    if (str == NULL) return NULL;
    
    // Handle negative index
    if (start < 0) start = 0;
    if (end < 0 || (size_t)end > str->length) end = str->length;
    
    // Bounds check
    if (start >= end || (size_t)start >= str->length) {
        return xr_string_intern(iso, "", 0, 0);
    }
    
    // Extract substring
    size_t len = end - start;
    return xr_string_intern(iso, &str->data[start], len, 0);
}

// slice - slice with negative index support
XrString* xr_string_slice(XrayIsolate *iso, XrString *str, xr_Integer start, xr_Integer end) {
    if (!iso || !str) return NULL;
    
    xr_Integer len = (xr_Integer)str->length;
    
    // Handle negative index: count from end
    if (start < 0) {
        start = len + start;
        if (start < 0) start = 0;
    }
    if (end < 0) {
        end = len + end;
        if (end < 0) end = 0;
    }
    
    // Bounds check
    if (start > len) start = len;
    if (end > len) end = len;
    
    // start > end returns empty string
    if (start >= end) {
        return xr_string_intern(iso, "", 0, 0);
    }
    
    // Extract substring
    size_t slice_len = (size_t)(end - start);
    return xr_string_intern(iso, &str->data[start], slice_len, 0);
}

// indexOf - find substring position
// Tiered optimization: single char (memchr), short pattern (<=8), long pattern (Horspool)
xr_Integer xr_string_index_of(XrayIsolate *iso, XrString *str, XrString *substr) {
    (void)iso;
    if (str == NULL || substr == NULL) return -1;
    if (substr->length == 0) return 0;
    if (substr->length > str->length) return -1;
    
    size_t n = str->length;
    size_t m = substr->length;
    const char *haystack = str->data;
    const char *needle = substr->data;
    
    // Strategy 1: single char - use memchr (usually SIMD optimized)
    if (m == 1) {
        const char *p = memchr(haystack, needle[0], n);
        return p ? (xr_Integer)(p - haystack) : -1;
    }
    
    // Strategy 2: short pattern (<=8 bytes) - first char jump + memcmp
    if (m <= 8) {
        char first = needle[0];
        size_t limit = n - m;
        for (size_t i = 0; i <= limit; ) {
            // Jump to first char match
            const char *p = memchr(haystack + i, first, limit - i + 1);
            if (!p) return -1;
            i = (size_t)(p - haystack);
            // Full compare
            if (memcmp(p, needle, m) == 0) {
                return (xr_Integer)i;
            }
            i++;
        }
        return -1;
    }
    
    // Strategy 3: long pattern (>8 bytes) - Horspool algorithm
    size_t skip[256];
    
    // Initialize skip table: default skip entire pattern length
    for (int c = 0; c < 256; c++) {
        skip[c] = m;
    }
    // Pattern chars: skip to alignment position
    for (size_t i = 0; i < m - 1; i++) {
        skip[(unsigned char)needle[i]] = m - 1 - i;
    }
    
    // Horspool search
    size_t i = 0;
    size_t limit = n - m;
    while (i <= limit) {
        // Compare from end
        size_t j = m - 1;
        while (j > 0 && haystack[i + j] == needle[j]) {
            j--;
        }
        
        if (j == 0 && haystack[i] == needle[0]) {
            return (xr_Integer)i;
        }
        
        // Bad character skip
        i += skip[(unsigned char)haystack[i + m - 1]];
    }
    
    return -1;
}

// size - get string length
int xr_string_size(XrayIsolate *iso, XrString *str) {
    (void)iso;
    if (!str) return 0;
    return (int)str->length;
}

// isEmpty - check if string is empty
bool xr_string_is_empty(XrayIsolate *iso, XrString *str) {
    (void)iso;
    if (!str) return true;
    return str->length == 0;
}

// has - check if contains substring
bool xr_string_has(XrayIsolate *iso, XrString *str, XrString *substr) {
    return xr_string_index_of(iso, str, substr) >= 0;
}

// startsWith - check prefix
bool xr_string_starts_with(XrayIsolate *iso, XrString *str, XrString *prefix) {
    (void)iso;
    if (str == NULL || prefix == NULL) return false;
    if (prefix->length > str->length) return false;
    if (prefix->length == 0) return true;
    
    return memcmp(str->data, prefix->data, prefix->length) == 0;
}

// endsWith - check suffix
bool xr_string_ends_with(XrayIsolate *iso, XrString *str, XrString *suffix) {
    (void)iso;
    if (str == NULL || suffix == NULL) return false;
    if (suffix->length > str->length) return false;
    if (suffix->length == 0) return true;
    
    size_t offset = str->length - suffix->length;
    return memcmp(&str->data[offset], suffix->data, suffix->length) == 0;
}

#define CASE_STACK_BUF 256

// toLowerCase - convert to lowercase
XrString* xr_string_to_lower_case(XrayIsolate *iso, XrString *str) {
    XR_DCHECK(iso != NULL, "string_to_lower_case: NULL isolate");
    if (str == NULL) return NULL;
    
    char stack_buf[CASE_STACK_BUF];
    char *buffer = (str->length < CASE_STACK_BUF) ? stack_buf : (char*)xr_malloc(str->length + 1);
    
    for (size_t i = 0; i < str->length; i++) {
        buffer[i] = tolower((unsigned char)str->data[i]);
    }
    buffer[str->length] = '\0';
    
    XrString *result = xr_string_intern(iso, buffer, str->length, 0);
    if (buffer != stack_buf) xr_free(buffer);
    
    return result;
}

// toUpperCase - convert to uppercase
XrString* xr_string_to_upper_case(XrayIsolate *iso, XrString *str) {
    XR_DCHECK(iso != NULL, "string_to_upper_case: NULL isolate");
    if (str == NULL) return NULL;
    
    char stack_buf[CASE_STACK_BUF];
    char *buffer = (str->length < CASE_STACK_BUF) ? stack_buf : (char*)xr_malloc(str->length + 1);
    
    for (size_t i = 0; i < str->length; i++) {
        buffer[i] = toupper((unsigned char)str->data[i]);
    }
    buffer[str->length] = '\0';
    
    XrString *result = xr_string_intern(iso, buffer, str->length, 0);
    if (buffer != stack_buf) xr_free(buffer);
    
    return result;
}

// trim - remove leading and trailing whitespace
XrString* xr_string_trim(XrayIsolate *iso, XrString *str) {
    XR_DCHECK(iso != NULL, "string_trim: NULL isolate");
    if (str == NULL) return NULL;
    if (str->length == 0) return str;
    
    // Find first non-whitespace
    size_t start = 0;
    while (start < str->length && xr_is_whitespace(str->data[start])) {
        start++;
    }
    
    // All whitespace
    if (start == str->length) {
        return xr_string_intern(iso, "", 0, 0);
    }
    
    // Find last non-whitespace
    size_t end = str->length - 1;
    while (end > start && xr_is_whitespace(str->data[end])) {
        end--;
    }
    
    // Extract
    size_t len = end - start + 1;
    return xr_string_intern(iso, &str->data[start], len, 0);
}

// trimStart - remove leading whitespace (SIMD optimized)
XrString* xr_string_trim_start(XrayIsolate *iso, XrString *str) {
    XR_DCHECK(iso != NULL, "string_trim_start: NULL isolate");
    if (str == NULL) return NULL;
    if (str->length == 0) return str;
    
    // SIMD skip whitespace
    const char *p = xr_simd_skip_whitespace(str->data, str->length);
    size_t start = p - str->data;
    
    // All whitespace
    if (start == str->length) {
        return xr_string_intern(iso, "", 0, 0);
    }
    
    // Extract to end
    size_t len = str->length - start;
    return xr_string_intern(iso, &str->data[start], len, 0);
}

// trimEnd - remove trailing whitespace
XrString* xr_string_trim_end(XrayIsolate *iso, XrString *str) {
    XR_DCHECK(iso != NULL, "string_trim_end: NULL isolate");
    if (str == NULL) return NULL;
    if (str->length == 0) return str;
    
    // Find last non-whitespace
    size_t end = str->length;
    while (end > 0 && xr_is_whitespace(str->data[end - 1])) {
        end--;
    }
    
    // All whitespace
    if (end == 0) {
        return xr_string_intern(iso, "", 0, 0);
    }
    
    return xr_string_intern(iso, str->data, end, 0);
}

// padStart - pad at start to target length
XrString* xr_string_pad_start(XrayIsolate *iso, XrString *str, size_t target_len, XrString *pad_str) {
    if (str == NULL) return NULL;
    
    // Already at target length
    if (str->length >= target_len) {
        return str;
    }
    
    // Default pad with space
    const char *pad = " ";
    size_t pad_len = 1;
    if (pad_str && pad_str->length > 0) {
        pad = pad_str->data;
        pad_len = pad_str->length;
    }
    
    size_t fill_len = target_len - str->length;
    
    char *result = xr_malloc(target_len + 1);
    if (!result) return NULL;
    
    // Fill start
    size_t pos = 0;
    while (pos < fill_len) {
        size_t copy_len = (fill_len - pos < pad_len) ? (fill_len - pos) : pad_len;
        memcpy(result + pos, pad, copy_len);
        pos += copy_len;
    }
    
    // Copy original string
    memcpy(result + fill_len, str->data, str->length);
    result[target_len] = '\0';
    
    XrString *ret = xr_string_intern(iso, result, target_len, 0);
    xr_free(result);
    return ret;
}

// padEnd - pad at end to target length
XrString* xr_string_pad_end(XrayIsolate *iso, XrString *str, size_t target_len, XrString *pad_str) {
    if (str == NULL) return NULL;
    
    // Already at target length
    if (str->length >= target_len) {
        return str;
    }
    
    // Default pad with space
    const char *pad = " ";
    size_t pad_len = 1;
    if (pad_str && pad_str->length > 0) {
        pad = pad_str->data;
        pad_len = pad_str->length;
    }
    
    size_t fill_len = target_len - str->length;
    (void)fill_len;
    
    char *result = xr_malloc(target_len + 1);
    if (!result) return NULL;
    
    // Copy original string
    memcpy(result, str->data, str->length);
    
    // Fill end
    size_t pos = str->length;
    while (pos < target_len) {
        size_t copy_len = (target_len - pos < pad_len) ? (target_len - pos) : pad_len;
        memcpy(result + pos, pad, copy_len);
        pos += copy_len;
    }
    result[target_len] = '\0';
    
    XrString *ret = xr_string_intern(iso, result, target_len, 0);
    xr_free(result);
    return ret;
}

// lastIndexOf - find substring from end
xr_Integer xr_string_last_index_of(XrayIsolate *iso, XrString *str, XrString *substr) {
    (void)iso;
    if (str == NULL || substr == NULL) return -1;
    if (substr->length == 0) return (xr_Integer)str->length;
    if (substr->length > str->length) return -1;
    
    // Search from end
    size_t last_pos = str->length - substr->length;
    for (size_t i = last_pos + 1; i > 0; i--) {
        size_t pos = i - 1;
        if (memcmp(&str->data[pos], substr->data, substr->length) == 0) {
            return (xr_Integer)pos;
        }
    }
    
    return -1;
}

/* ========== String Advanced Methods ========== */

// split - split string into array
XrArray* xr_string_split(XrayIsolate *iso, XrString *str, XrString *delimiter) {
    XR_DCHECK(iso != NULL, "string_split: NULL isolate");
    XrArray *result = xr_array_new(xr_current_coro(iso));
    
    if (str == NULL) return result;
    
    // Empty delimiter, split by character
    if (delimiter == NULL || delimiter->length == 0) {
        for (size_t i = 0; i < str->length; i++) {
            XrString *ch = xr_string_intern(iso, &str->data[i], 1, 0);
            xr_array_push(result, xr_string_value(ch));
        }
        return result;
    }
    
    // Split by delimiter
    const char *start = str->data;
    const char *end = str->data;
    const char *str_end = str->data + str->length;
    
    while (end <= str_end - delimiter->length) {
        if (memcmp(end, delimiter->data, delimiter->length) == 0) {
            // Found delimiter
            size_t len = end - start;
            XrString *part = xr_string_intern(iso, start, len, 0);
            xr_array_push(result, xr_string_value(part));
            
            end += delimiter->length;
            start = end;
        } else {
            end++;
        }
    }
    
    // Add last part
    size_t len = str_end - start;
    XrString *part = xr_string_intern(iso, start, len, 0);
    xr_array_push(result, xr_string_value(part));
    
    return result;
}

// replace - replace first occurrence
XrString* xr_string_replace(XrayIsolate *iso, XrString *str, XrString *old_str, XrString *new_str) {
    XR_DCHECK(iso != NULL, "string_replace: NULL isolate");
    if (str == NULL || old_str == NULL || new_str == NULL) return str;
    if (old_str->length == 0) return str;
    
    // Find first occurrence
    xr_Integer pos = xr_string_index_of(iso, str, old_str);
    if (pos < 0) return str;  // Not found
    
    // Calculate new length
    size_t new_length = str->length - old_str->length + new_str->length;
    char *buffer = (char*)xr_malloc(new_length + 1);
    
    // Concatenate: prefix + new_str + suffix
    memcpy(buffer, str->data, pos);
    memcpy(buffer + pos, new_str->data, new_str->length);
    memcpy(buffer + pos + new_str->length, 
           str->data + pos + old_str->length,
           str->length - pos - old_str->length);
    buffer[new_length] = '\0';
    
    XrString *result = xr_string_intern(iso, buffer, new_length, 0);
    xr_free(buffer);
    
    return result;
}

// replaceAll - replace all occurrences
XrString* xr_string_replace_all(XrayIsolate *iso, XrString *str, XrString *old_str, XrString *new_str) {
    XR_DCHECK(iso != NULL, "string_replace_all: NULL isolate");
    if (str == NULL || old_str == NULL || new_str == NULL) return str;
    if (old_str->length == 0) return str;
    
    // Count replacements needed
    int count = 0;
    size_t pos = 0;
    while (pos + old_str->length <= str->length) {
        if (memcmp(&str->data[pos], old_str->data, old_str->length) == 0) {
            count++;
            pos += old_str->length;
        } else {
            pos++;
        }
    }
    
    // No match, return original
    if (count == 0) return str;
    
    // Calculate new length
    size_t new_length = str->length + count * (new_str->length - old_str->length);
    char *buffer = (char*)xr_malloc(new_length + 1);
    
    // Build new string
    size_t src_pos = 0;
    size_t dst_pos = 0;
    
    while (src_pos < str->length) {
        if (src_pos + old_str->length <= str->length &&
            memcmp(&str->data[src_pos], old_str->data, old_str->length) == 0) {
            // Found match, copy new_str
            memcpy(&buffer[dst_pos], new_str->data, new_str->length);
            dst_pos += new_str->length;
            src_pos += old_str->length;
        } else {
            // No match, copy single char
            buffer[dst_pos++] = str->data[src_pos++];
        }
    }
    
    buffer[new_length] = '\0';
    
    XrString *result = xr_string_intern(iso, buffer, new_length, 0);
    xr_free(buffer);
    
    return result;
}

// repeat - repeat string
XrString* xr_string_repeat(XrayIsolate *iso, XrString *str, xr_Integer count) {
    XR_DCHECK(iso != NULL, "string_repeat: NULL isolate");
    if (str == NULL || count <= 0) {
        return xr_string_intern(iso, "", 0, 0);
    }
    if (count == 1) return str;
    
    size_t new_length = str->length * count;
    char *buffer = (char*)xr_malloc(new_length + 1);
    
    for (xr_Integer i = 0; i < count; i++) {
        memcpy(buffer + i * str->length, str->data, str->length);
    }
    buffer[new_length] = '\0';
    
    XrString *result = xr_string_intern(iso, buffer, new_length, 0);
    xr_free(buffer);
    
    return result;
}

// reverse - reverse string (Unicode aware, no temp arrays)
XrString* xr_string_reverse(XrayIsolate *iso, XrString *str) {
    if (!iso || !str) return NULL;
    if (str->length == 0) return str;
    
    char stack_buf[CASE_STACK_BUF];
    size_t len = str->length;
    char *buffer = (len < CASE_STACK_BUF) ? stack_buf : (char*)xr_malloc(len + 1);
    if (!buffer) return NULL;
    
    // Scan backwards: find UTF-8 char boundaries and copy forward
    const char *src = str->data;
    size_t dst = 0;
    size_t end = len;
    
    while (end > 0) {
        // Walk back past continuation bytes (10xxxxxx)
        size_t start = end - 1;
        while (start > 0 && ((unsigned char)src[start] & 0xC0) == 0x80) {
            start--;
        }
        size_t char_len = end - start;
        memcpy(buffer + dst, src + start, char_len);
        dst += char_len;
        end = start;
    }
    buffer[dst] = '\0';
    
    XrString *result = xr_string_intern(iso, buffer, dst, 0);
    if (buffer != stack_buf) xr_free(buffer);
    
    return result;
}

// reverseBytes - byte-level reverse (O(n), no UTF-8 parsing)
XrString* xr_string_reverse_bytes(XrayIsolate *iso, XrString *str) {
    if (!iso || !str) return NULL;
    if (str->length == 0) return str;
    
    char *buffer = (char*)xr_malloc(str->length + 1);
    if (!buffer) return NULL;
    
    // Byte-level reverse: copy from end to start
    const char *src = str->data;
    size_t len = str->length;
    for (size_t i = 0; i < len; i++) {
        buffer[i] = src[len - 1 - i];
    }
    buffer[len] = '\0';
    
    // Intern result string
    XrString *result = xr_string_intern(iso, buffer, len, 0);
    xr_free(buffer);
    
    return result;
}

// byteAt - O(1) byte index (supports negative index)
XrString* xr_string_byte_at(XrayIsolate *iso, XrString *str, xr_Integer index) {
    if (!iso || !str || str->length == 0) return NULL;
    
    // Handle negative index
    if (index < 0) {
        index = (xr_Integer)str->length + index;
    }
    
    // Bounds check
    if (index < 0 || (size_t)index >= str->length) return NULL;
    
    // Get single byte
    char c = str->data[index];
    
    // Return interned single char string
    return xr_string_intern(iso, &c, 1, 0);
}

// translateBytes - byte-level char mapping (O(n), no UTF-8 parsing)
XrString* xr_string_translate_bytes(XrayIsolate *iso, XrString *str, XrMap *table) {
    if (!iso || !str) return NULL;
    if (!table) return str;  // No table, return original
    if (str->length == 0) return str;
    
    char *result = (char*)xr_malloc(str->length + 1);
    if (!result) return NULL;
    
    // Single pass, replace byte by byte
    for (size_t i = 0; i < str->length; i++) {
        char c = str->data[i];
        
        // Create key from single byte to lookup Map
        XrString *key = xr_string_intern(iso, &c, 1, 0);
        bool found = false;
        XrValue val = xr_map_get(table, xr_string_value(key), &found);
        
        if (found && XR_IS_STRING(val)) {
            XrString *replacement = XR_TO_STRING(val);
            // Get first byte of replacement
            if (replacement->length > 0) {
                result[i] = replacement->data[0];
            } else {
                result[i] = c;  // Empty replacement, keep original
            }
        } else {
            result[i] = c;  // No mapping, keep original
        }
    }
    result[str->length] = '\0';
    
    // Create result string and free buffer
    XrString *ret = xr_string_intern(iso, result, str->length, 0);
    xr_free(result);
    return ret;
}

// translate - Unicode char mapping (UTF-8 aware)
XrString* xr_string_translate(XrayIsolate *iso, XrString *str, XrMap *table) {
    if (!iso || !str) return NULL;
    if (!table) return str;
    if (str->length == 0) return str;
    
    // Use StringBuilder (replacement may change length)
    XrStringBuilder *sb = xr_stringbuilder_new(xr_current_coro(iso));
    if (!sb) return NULL;
    
    size_t pos = 0;
    while (pos < str->length) {
        // Get current UTF-8 char length
        size_t char_len = xr_utf8_char_size((unsigned char)str->data[pos]);
        if (char_len == 0 || pos + char_len > str->length) {
            // Invalid UTF-8, treat as single byte
            char_len = 1;
        }
        
        // Create current char string as key
        XrString *key = xr_string_intern(iso, str->data + pos, char_len, 0);
        bool found = false;
        XrValue val = xr_map_get(table, xr_string_value(key), &found);
        
        if (found && XR_IS_STRING(val)) {
            XrString *replacement = XR_TO_STRING(val);
            xr_stringbuilder_append_str(sb, replacement);
        } else {
            // No mapping, keep original
            xr_stringbuilder_append_cstr(sb, str->data + pos, char_len);
        }
        
        pos += char_len;
    }
    
    return xr_stringbuilder_to_string(sb);
}

// Note: join method moved to xarray.c (array.join(delimiter))

/* ========== Unicode / UTF-8 Support ========== */

// charLength - get character count
size_t xr_string_char_length(XrString *str) {
    if (!str) return 0;
    return xr_utf8_strlen(str->data, str->length);
}

// charCodeAt - get Unicode codepoint at char index
int32_t xr_string_char_code_at(XrString *str, size_t index) {
    if (!str) return -1;
    
    uint32_t cp;
    if (xr_utf8_char_at(str->data, str->length, index, &cp, NULL)) {
        return (int32_t)cp;
    }
    return -1;  // Index out of bounds
}

// charAtUnicode - get char by Unicode char index
XrString* xr_string_char_at_unicode(XrayIsolate *iso, XrString *str, size_t index) {
    if (!iso || !str) return NULL;
    
    uint32_t cp;
    size_t pos;
    if (!xr_utf8_char_at(str->data, str->length, index, &cp, &pos)) {
        return NULL;  // Index out of bounds
    }
    
    // Get byte length of this char
    int char_size = xr_utf8_char_size((uint8_t)str->data[pos]);
    if (pos + char_size > str->length) {
        return NULL;  // Incomplete char
    }
    
    // Create single char string
    return xr_string_intern(iso, str->data + pos, char_size, 0);
}

/*
** substringByChar - substring by char index
*/
XrString* xr_string_substring_by_char(XrayIsolate *iso, XrString *str, 
                                       size_t start, size_t end) {
    if (!iso || !str) return NULL;
    if (start > end) return NULL;
    
    size_t byte_start, byte_end;
    if (!xr_utf8_char_range(str->data, str->length, start, end, 
                            &byte_start, &byte_end)) {
        return NULL;
    }
    
    // Bounds check
    if (byte_start > str->length) byte_start = str->length;
    if (byte_end > str->length) byte_end = str->length;
    
    return xr_string_intern(iso, str->data + byte_start, 
                            byte_end - byte_start, 0);
}

/*
** fromCodePoint - create string from Unicode codepoint
*/
XrString* xr_string_from_codepoint(XrayIsolate *iso, uint32_t codepoint) {
    if (!iso) return NULL;
    
    char buf[XR_UTF8_MAX_BYTES];
    int len = xr_utf8_encode(codepoint, buf);
    
    if (len == 0) {
        // Invalid codepoint, return replacement char
        len = xr_utf8_encode(XR_UNICODE_INVALID, buf);
    }
    
    return xr_string_intern(iso, buf, len, 0);
}

/* ========== Helper Functions ========== */

// Print string (debug)
void xr_string_print(XrString *str) {
    if (str == NULL) {
        printf("(null)");
    } else {
        printf("%.*s", (int)str->length, str->data);
    }
}

/* ========== Character Classification ========== */

#include "../../base/xunicode.h"

bool xr_string_is_letter(XrString *str) {
    if (!str || str->length == 0) return false;
    
    const char *p = str->data;
    const char *end = p + str->length;
    
    while (p < end) {
        uint32_t cp;
        int bytes = xr_utf8_decode(p, end - p, &cp);
        if (!xr_unicode_is_letter(cp)) return false;
        p += bytes;
    }
    return true;
}

bool xr_string_is_number(XrString *str) {
    if (!str || str->length == 0) return false;
    
    const char *p = str->data;
    const char *end = p + str->length;
    
    while (p < end) {
        uint32_t cp;
        int bytes = xr_utf8_decode(p, end - p, &cp);
        if (!xr_unicode_is_number(cp)) return false;
        p += bytes;
    }
    return true;
}

bool xr_string_is_alnum(XrString *str) {
    if (!str || str->length == 0) return false;
    
    const char *p = str->data;
    const char *end = p + str->length;
    
    while (p < end) {
        uint32_t cp;
        int bytes = xr_utf8_decode(p, end - p, &cp);
        if (!xr_unicode_is_alnum(cp)) return false;
        p += bytes;
    }
    return true;
}

bool xr_string_is_whitespace_str(XrString *str) {
    if (!str || str->length == 0) return false;
    
    const char *p = str->data;
    const char *end = p + str->length;
    
    while (p < end) {
        uint32_t cp;
        int bytes = xr_utf8_decode(p, end - p, &cp);
        if (!xr_unicode_is_whitespace(cp)) return false;
        p += bytes;
    }
    return true;
}

int32_t xr_string_ord(XrString *str) {
    if (!str || str->length == 0) return -1;
    
    uint32_t cp;
    xr_utf8_decode(str->data, str->length, &cp);
    return (int32_t)cp;
}

