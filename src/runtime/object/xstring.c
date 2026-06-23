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
#include "../../shared/xr_float_fmt.h"
#include "../../shared/xr_string_core.h"
#include "../../base/xlog.h"
#include "xarray.h"
#include "xstring.h"
#include "xmap.h"
#include "../../base/xutf8.h"
#include "../xstrbuf.h"
#include "xstringbuilder.h"
#include "../core/xr_runtime_core.h"
#include "../xisolate_api.h"
#include "../xisolate_api.h"
#include "../../base/xchecks.h"
#include "../class/xclass_system.h"
#include "../class/xclass.h"
#include "../mem/xheap.h"
#include "../mem/xsystem_heap.h"
#include "../xshared.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ========== String Pool Management ========== */

// Initialize string pool (Isolate internal use)
void xr_string_pool_init_internal(XrStringPool *pool) {
    if (!pool)
        return;

    pool->capacity = STRING_POOL_INIT_CAPACITY;
    pool->mask = pool->capacity - 1;  // For bitwise modulo
    pool->count = 0;
    pool->threshold = (size_t) (pool->capacity * STRING_POOL_LOAD_FACTOR);
    pool->entries = (XrString **) xr_malloc(sizeof(XrString *) * pool->capacity);
    if (!pool->entries)
        return;

    // Initialize to NULL
    memset(pool->entries, 0, sizeof(XrString *) * pool->capacity);
}

// Free string pool (Isolate internal use)
// Note: String objects are GC managed, only free hash table structure
void xr_string_pool_free_internal(XrStringPool *pool) {
    if (!pool || pool->entries == NULL)
        return;

    // Free table (not String objects themselves)
    xr_free(pool->entries);
    pool->entries = NULL;
    pool->capacity = 0;
    pool->count = 0;
}

/* ========== String Creation ========== */

// Allocate string object on coroutine heap (shared by both paths)
static XrString *string_alloc(XrVMRuntime *iso, const char *chars, size_t length) {
    if (length > UINT32_MAX)
        return NULL;

    size_t total_size = sizeof(XrString) + length + 1;
    XrCoroutine *coro = iso ? xr_current_coro(iso) : NULL;

    XrString *str = (XrString *) xr_alloc(coro, total_size, XR_TSTRING);
    if (!str)
        return NULL;

    str->length = (uint32_t) length;
    if (chars)
        memcpy(str->data, chars, length);
    str->data[length] = '\0';

    return str;
}

// Create non-interned string (lazy hash: computed on first use)
XrString *xr_string_new(XrVMRuntime *iso, const char *chars, size_t length) {
    XR_DCHECK(iso != NULL, "string_new: NULL isolate");
    XR_DCHECK(length == 0 || chars != NULL, "string_new: NULL chars with length > 0");
    XrString *str = string_alloc(iso, chars, length);
    if (!str)
        return NULL;
    str->hash = 0;  // Lazy: computed when needed
    return str;
}

XrString *xr_string_concat(XrVMRuntime *iso, XrString *a, XrString *b) {
    XR_DCHECK(iso != NULL, "string_concat: NULL isolate");
    if (!a || !b)
        return NULL;

    size_t len = (size_t) a->length + (size_t) b->length;
    if (len > UINT32_MAX)
        return NULL;

    XrString *str = string_alloc(iso, NULL, len);
    if (!str)
        return NULL;
    memcpy(str->data, a->data, a->length);
    memcpy(str->data + a->length, b->data, b->length);
    str->data[len] = '\0';
    str->hash = 0;
    return str;
}

// String interning (short/long separation)
// Short strings (<=64B): global pool with rwlock
// Long strings (>64B): shared system-heap allocation
XrString *xr_string_intern_core(XrRuntimeCore *core, const char *chars, size_t length,
                                uint32_t hash) {
    if (!core || !chars) {
        xr_log_warning("string", "string_intern_core: core or chars is NULL");
        return NULL;
    }

    if (hash == 0) {
        hash = xr_string_hash(chars, length);
    }

    if (length > XR_SHORT_STR_MAX) {
        XrSystemHeap *heap = core->sys_heap;
        if (!heap)
            return NULL;
        size_t total_size = sizeof(XrString) + length + 1;
        XrString *str = (XrString *) xr_sysheap_alloc_shared(heap, total_size, XR_TSTRING);
        if (str) {
            str->length = (uint32_t) length;
            str->hash = hash;
            memcpy(str->data, chars, length);
            str->data[length] = '\0';
            XR_STR_SET_LONG(str);
            xr_shared_set_refc(&str->hdr, 1);
        }
        return str;
    }

    XrGlobalStringPool *pool = core->global_string_pool;
    if (!pool) {
        return NULL;
    }

    // Step 1: Read lock lookup
    xr_rwlock_rdlock(&pool->lock);
    XrString *found = xr_global_pool_lookup(pool, chars, length, hash);
    xr_rwlock_rdunlock(&pool->lock);

    if (found) {
        return found;
    }

    // Step 2: Write lock insert (double-check pattern)
    xr_rwlock_wrlock(&pool->lock);

    // Check again (another thread may have inserted)
    found = xr_global_pool_lookup(pool, chars, length, hash);
    if (found) {
        xr_rwlock_wrunlock(&pool->lock);
        return found;
    }

    // Insert new string
    XrString *str = xr_global_pool_insert_locked(pool, chars, length, hash);
    xr_rwlock_wrunlock(&pool->lock);

    return str;
}

XrString *xr_string_intern(XrVMRuntime *iso, const char *chars, size_t length, uint32_t hash) {
    if (!iso) {
        xr_log_warning("string", "string_intern: isolate is NULL");
        abort();
    }
    XrRuntimeCore *core = xr_isolate_get_runtime_core(iso);
    if (core) {
        return xr_string_intern_core(core, chars, length, hash);
    }
    XrString *s = string_alloc(iso, chars, length);
    if (s && hash)
        s->hash = hash;
    return s;
}

// Fast integer to string (without snprintf)
static inline int fast_int_to_str(xr_Integer i, char *buffer) {
    char *p = buffer;
    int neg = 0;
    uint64_t uval;

    // Extract digits in the unsigned domain to avoid UB on -INT64_MIN
    // (negating INT64_MIN as a signed value overflows and previously
    // produced garbage like "-(" for the most-negative integer).
    if (i < 0) {
        neg = 1;
        uval = (uint64_t) (-(i + 1)) + 1;
    } else {
        uval = (uint64_t) i;
    }

    // Write digits in reverse
    char *start = p;
    do {
        *p++ = '0' + (char) (uval % 10);
        uval /= 10;
    } while (uval > 0);

    if (neg) {
        *p++ = '-';
    }

    int len = (int) (p - start);
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
XrString *xr_string_from_int(XrVMRuntime *iso, xr_Integer i) {
    XR_DCHECK(iso != NULL, "string_from_int: NULL isolate");
    char buffer[32];
    int len = fast_int_to_str(i, buffer);
    return xr_string_intern(iso, buffer, len, 0);
}

// Create string from float
// Guarantees a decimal point so 0.0 prints as "0.0", not "0".
XrString *xr_string_from_float(XrVMRuntime *iso, xr_Number n) {
    XR_DCHECK(iso != NULL, "string_from_float: NULL isolate");
    char buffer[64];
    int len = xr_format_float(buffer, sizeof(buffer), n);
    return xr_string_intern(iso, buffer, (size_t) len, 0);
}

/* ========== String Comparison ========== */

// String equality comparison (optimized)
// Priority: pointer -> length -> hash -> pool check -> content
bool xr_string_equal(XrString *a, XrString *b) {
    XR_DCHECK(a != NULL && b != NULL, "string_equal: NULL argument");
    // Pointer equal (fastest path for interned strings)
    if (a == b)
        return true;

    if (a == NULL || b == NULL)
        return false;

    // Length not equal, fast reject
    if (a->length != b->length)
        return false;

    // Hash not equal, fast reject
    if (a->hash != b->hash)
        return false;

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
        if (a == b)
            return 0;
        return a == NULL ? -1 : 1;
    }

    if (a == b)
        return 0;

    size_t min_len = a->length < b->length ? a->length : b->length;
    int cmp = memcmp(a->data, b->data, min_len);

    if (cmp != 0)
        return cmp;

    // Different length
    if (a->length < b->length)
        return -1;
    if (a->length > b->length)
        return 1;
    return 0;
}

/* ========== String Basic Methods ========== */

// charAt - get character at position (supports negative index)
XrString *xr_string_char_at(XrVMRuntime *iso, XrString *str, xr_Integer index) {
    XR_DCHECK(iso != NULL, "string_char_at: NULL isolate");
    if (str == NULL)
        return NULL;

    XrStringCoreSlice slice = xr_string_core_byte_slice_at(str->data, str->length, index);
    if (slice.len == 0)
        return NULL;
    return xr_string_intern(iso, slice.data, slice.len, 0);
}

// substring - extract substring
XrString *xr_string_substring(XrVMRuntime *iso, XrString *str, xr_Integer start, xr_Integer end) {
    XR_DCHECK(iso != NULL, "string_substring: NULL isolate");
    if (str == NULL)
        return NULL;

    XrStringCoreSlice slice = xr_string_core_substring_slice(str->data, str->length, start, end);
    return xr_string_intern(iso, slice.data, slice.len, 0);
}

// slice - slice with negative index support
XrString *xr_string_slice(XrVMRuntime *iso, XrString *str, xr_Integer start, xr_Integer end) {
    if (!iso || !str)
        return NULL;

    XrStringCoreSlice slice = xr_string_core_range_slice(str->data, str->length, start, end);
    return xr_string_intern(iso, slice.data, slice.len, 0);
}

// indexOf - find substring position
xr_Integer xr_string_index_of(XrVMRuntime *iso, XrString *str, XrString *substr) {
    (void) iso;
    if (str == NULL || substr == NULL)
        return -1;
    return (xr_Integer) xr_string_core_index_of(str->data, str->length, substr->data,
                                                substr->length);
}

// size - get string length
int xr_string_size(XrVMRuntime *iso, XrString *str) {
    (void) iso;
    if (!str)
        return 0;
    return (int) str->length;
}

// isEmpty - check if string is empty
bool xr_string_is_empty(XrVMRuntime *iso, XrString *str) {
    (void) iso;
    if (!str)
        return true;
    return str->length == 0;
}

// has - check if contains substring
bool xr_string_has(XrVMRuntime *iso, XrString *str, XrString *substr) {
    return xr_string_index_of(iso, str, substr) >= 0;
}

// startsWith - check prefix
bool xr_string_starts_with(XrVMRuntime *iso, XrString *str, XrString *prefix) {
    (void) iso;
    if (str == NULL || prefix == NULL)
        return false;
    return xr_string_core_starts_with(str->data, str->length, prefix->data, prefix->length);
}

// endsWith - check suffix
bool xr_string_ends_with(XrVMRuntime *iso, XrString *str, XrString *suffix) {
    (void) iso;
    if (str == NULL || suffix == NULL)
        return false;
    return xr_string_core_ends_with(str->data, str->length, suffix->data, suffix->length);
}

#define CASE_STACK_BUF 256

// toLowerCase - convert to lowercase
XrString *xr_string_to_lower_case(XrVMRuntime *iso, XrString *str) {
    XR_DCHECK(iso != NULL, "string_to_lower_case: NULL isolate");
    if (str == NULL)
        return NULL;

    char stack_buf[CASE_STACK_BUF];
    char *buffer = (str->length < CASE_STACK_BUF) ? stack_buf : (char *) xr_malloc(str->length + 1);

    xr_string_core_ascii_lower_write(buffer, str->data, str->length);

    XrString *result = xr_string_intern(iso, buffer, str->length, 0);
    if (buffer != stack_buf)
        xr_free(buffer);

    return result;
}

// toUpperCase - convert to uppercase
XrString *xr_string_to_upper_case(XrVMRuntime *iso, XrString *str) {
    XR_DCHECK(iso != NULL, "string_to_upper_case: NULL isolate");
    if (str == NULL)
        return NULL;

    char stack_buf[CASE_STACK_BUF];
    char *buffer = (str->length < CASE_STACK_BUF) ? stack_buf : (char *) xr_malloc(str->length + 1);

    xr_string_core_ascii_upper_write(buffer, str->data, str->length);

    XrString *result = xr_string_intern(iso, buffer, str->length, 0);
    if (buffer != stack_buf)
        xr_free(buffer);

    return result;
}

// trim - remove leading and trailing whitespace
XrString *xr_string_trim(XrVMRuntime *iso, XrString *str) {
    XR_DCHECK(iso != NULL, "string_trim: NULL isolate");
    if (str == NULL)
        return NULL;
    if (str->length == 0)
        return str;

    XrStringCoreSlice slice =
        xr_string_core_trim_slice(str->data, str->length, XR_STRING_CORE_TRIM_BOTH);
    return xr_string_intern(iso, slice.data, slice.len, 0);
}

// trimStart - remove leading whitespace
XrString *xr_string_trim_start(XrVMRuntime *iso, XrString *str) {
    XR_DCHECK(iso != NULL, "string_trim_start: NULL isolate");
    if (str == NULL)
        return NULL;
    if (str->length == 0)
        return str;

    XrStringCoreSlice slice =
        xr_string_core_trim_slice(str->data, str->length, XR_STRING_CORE_TRIM_START);
    return xr_string_intern(iso, slice.data, slice.len, 0);
}

// trimEnd - remove trailing whitespace
XrString *xr_string_trim_end(XrVMRuntime *iso, XrString *str) {
    XR_DCHECK(iso != NULL, "string_trim_end: NULL isolate");
    if (str == NULL)
        return NULL;
    if (str->length == 0)
        return str;

    XrStringCoreSlice slice =
        xr_string_core_trim_slice(str->data, str->length, XR_STRING_CORE_TRIM_END);
    return xr_string_intern(iso, slice.data, slice.len, 0);
}

// padStart - pad at start to target length
XrString *xr_string_pad_start(XrVMRuntime *iso, XrString *str, xr_Integer target_len,
                              XrString *pad_str) {
    if (str == NULL)
        return NULL;

    const char *pad = pad_str ? pad_str->data : NULL;
    size_t pad_len = pad_str ? pad_str->length : 0;
    XrStringCorePadPlan plan =
        xr_string_core_pad_plan(str->data, str->length, target_len, pad, pad_len);
    if (plan.kind == XR_STRING_CORE_PAD_INVALID || plan.kind == XR_STRING_CORE_PAD_ORIGINAL)
        return str;

    char *result = xr_malloc(plan.len + 1);
    if (!result)
        return NULL;

    xr_string_core_pad_write(result, str->data, str->length, plan, XR_STRING_CORE_PAD_START);
    XrString *ret = xr_string_intern(iso, result, plan.len, 0);
    xr_free(result);
    return ret;
}

// padEnd - pad at end to target length
XrString *xr_string_pad_end(XrVMRuntime *iso, XrString *str, xr_Integer target_len,
                            XrString *pad_str) {
    if (str == NULL)
        return NULL;

    const char *pad = pad_str ? pad_str->data : NULL;
    size_t pad_len = pad_str ? pad_str->length : 0;
    XrStringCorePadPlan plan =
        xr_string_core_pad_plan(str->data, str->length, target_len, pad, pad_len);
    if (plan.kind == XR_STRING_CORE_PAD_INVALID || plan.kind == XR_STRING_CORE_PAD_ORIGINAL)
        return str;

    char *result = xr_malloc(plan.len + 1);
    if (!result)
        return NULL;

    xr_string_core_pad_write(result, str->data, str->length, plan, XR_STRING_CORE_PAD_END);
    XrString *ret = xr_string_intern(iso, result, plan.len, 0);
    xr_free(result);
    return ret;
}

// lastIndexOf - find substring from end
xr_Integer xr_string_last_index_of(XrVMRuntime *iso, XrString *str, XrString *substr) {
    (void) iso;
    if (str == NULL || substr == NULL)
        return -1;
    return (xr_Integer) xr_string_core_last_index_of(str->data, str->length, substr->data,
                                                     substr->length);
}

/* ========== String Advanced Methods ========== */

// split - split string into array
XrArray *xr_string_split(XrVMRuntime *iso, XrString *str, XrString *delimiter) {
    XR_DCHECK(iso != NULL, "string_split: NULL isolate");
    XrArray *result = xr_array_new(xr_current_coro(iso));

    if (str == NULL)
        return result;

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
XrString *xr_string_replace(XrVMRuntime *iso, XrString *str, XrString *old_str, XrString *new_str) {
    XR_DCHECK(iso != NULL, "string_replace: NULL isolate");
    if (str == NULL || old_str == NULL || new_str == NULL)
        return str;

    XrStringCoreReplacePlan plan =
        xr_string_core_replace_plan(str->data, str->length, old_str->data, old_str->length,
                                    new_str->data, new_str->length, false);
    if (plan.kind == XR_STRING_CORE_REPLACE_INVALID)
        return NULL;
    if (plan.kind == XR_STRING_CORE_REPLACE_ORIGINAL)
        return str;

    char *buffer = (char *) xr_malloc(plan.len + 1);
    if (!buffer)
        return NULL;
    xr_string_core_replace_write(buffer, str->data, str->length, old_str->data, old_str->length,
                                 new_str->data, new_str->length, plan, false);

    XrString *result = xr_string_intern(iso, buffer, plan.len, 0);
    xr_free(buffer);

    return result;
}

// replaceAll - replace all occurrences
XrString *xr_string_replace_all(XrVMRuntime *iso, XrString *str, XrString *old_str,
                                XrString *new_str) {
    XR_DCHECK(iso != NULL, "string_replace_all: NULL isolate");
    if (str == NULL || old_str == NULL || new_str == NULL)
        return str;

    XrStringCoreReplacePlan plan =
        xr_string_core_replace_plan(str->data, str->length, old_str->data, old_str->length,
                                    new_str->data, new_str->length, true);
    if (plan.kind == XR_STRING_CORE_REPLACE_INVALID)
        return NULL;
    if (plan.kind == XR_STRING_CORE_REPLACE_ORIGINAL)
        return str;

    char *buffer = (char *) xr_malloc(plan.len + 1);
    if (!buffer)
        return NULL;
    xr_string_core_replace_write(buffer, str->data, str->length, old_str->data, old_str->length,
                                 new_str->data, new_str->length, plan, true);

    XrString *result = xr_string_intern(iso, buffer, plan.len, 0);
    xr_free(buffer);

    return result;
}

// repeat - repeat string
XrString *xr_string_repeat(XrVMRuntime *iso, XrString *str, xr_Integer count) {
    XR_DCHECK(iso != NULL, "string_repeat: NULL isolate");
    if (str == NULL) {
        return xr_string_intern(iso, "", 0, 0);
    }

    XrStringCoreRepeatPlan plan = xr_string_core_repeat_plan(str->data, str->length, count);
    if (plan.kind == XR_STRING_CORE_REPEAT_INVALID)
        return NULL;
    if (plan.kind == XR_STRING_CORE_REPEAT_EMPTY)
        return xr_string_intern(iso, "", 0, 0);
    if (plan.kind == XR_STRING_CORE_REPEAT_ORIGINAL)
        return str;

    char *buffer = (char *) xr_malloc(plan.len + 1);
    if (!buffer)
        return NULL;
    xr_string_core_repeat_write(buffer, str->data, str->length, count);

    XrString *result = xr_string_intern(iso, buffer, plan.len, 0);
    xr_free(buffer);

    return result;
}

// reverse - reverse string (Unicode aware, no temp arrays)
XrString *xr_string_reverse(XrVMRuntime *iso, XrString *str) {
    if (!iso || !str)
        return NULL;
    if (str->length == 0)
        return str;

    char stack_buf[CASE_STACK_BUF];
    size_t len = str->length;
    char *buffer = (len < CASE_STACK_BUF) ? stack_buf : (char *) xr_malloc(len + 1);
    if (!buffer)
        return NULL;

    size_t dst = xr_string_core_reverse_utf8_write(buffer, str->data, len);

    XrString *result = xr_string_intern(iso, buffer, dst, 0);
    if (buffer != stack_buf)
        xr_free(buffer);

    return result;
}

// byteAt - O(1) byte index (supports negative index)
XrString *xr_string_byte_at(XrVMRuntime *iso, XrString *str, xr_Integer index) {
    if (!iso || !str)
        return NULL;

    XrStringCoreSlice slice = xr_string_core_byte_slice_at(str->data, str->length, index);
    if (slice.len == 0)
        return NULL;
    return xr_string_intern(iso, slice.data, slice.len, 0);
}

// translate - Unicode char mapping (UTF-8 aware)
XrString *xr_string_translate(XrVMRuntime *iso, XrString *str, XrMap *table) {
    if (!iso || !str)
        return NULL;
    if (!table)
        return str;
    if (str->length == 0)
        return str;

    // Use StringBuilder (replacement may change length)
    XrStringBuilder *sb = xr_stringbuilder_new(xr_current_coro(iso));
    if (!sb)
        return NULL;

    size_t pos = 0;
    while (pos < str->length) {
        // Get current UTF-8 char length
        size_t char_len = xr_utf8_char_size((unsigned char) str->data[pos]);
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
    if (!str)
        return 0;
    return xr_string_core_utf8_char_count(str->data, str->length);
}

// charCodeAt - get Unicode codepoint at char index
int32_t xr_string_char_code_at(XrString *str, size_t index) {
    if (!str)
        return -1;
    if (index > (size_t) INT64_MAX)
        return -1;

    uint32_t cp;
    if (xr_string_core_codepoint_at(str->data, str->length, (int64_t) index, &cp)) {
        return (int32_t) cp;
    }
    return -1;  // Index out of bounds
}

// charAtUnicode - get char by Unicode char index
XrString *xr_string_char_at_unicode(XrVMRuntime *iso, XrString *str, size_t index) {
    if (!iso || !str)
        return NULL;
    if (index > (size_t) INT64_MAX)
        return NULL;

    XrStringCoreSlice slice =
        xr_string_core_utf8_char_slice_at(str->data, str->length, (int64_t) index);
    if (slice.len == 0)
        return NULL;
    return xr_string_intern(iso, slice.data, slice.len, 0);
}

/*
** substringByChar - substring by char index
*/
XrString *xr_string_substring_by_char(XrVMRuntime *iso, XrString *str, size_t start, size_t end) {
    if (!iso || !str)
        return NULL;
    if (start > end)
        return NULL;

    size_t byte_start, byte_end;
    if (!xr_utf8_char_range(str->data, str->length, start, end, &byte_start, &byte_end)) {
        return NULL;
    }

    // Bounds check
    if (byte_start > str->length)
        byte_start = str->length;
    if (byte_end > str->length)
        byte_end = str->length;

    return xr_string_intern(iso, str->data + byte_start, byte_end - byte_start, 0);
}

/*
** fromCodePoint - create string from Unicode codepoint
*/
XrString *xr_string_from_codepoint(XrVMRuntime *iso, uint32_t codepoint) {
    if (!iso)
        return NULL;

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
        printf("%.*s", (int) str->length, str->data);
    }
}

/* ========== Character Classification ========== */

#include "../../base/xunicode.h"

bool xr_string_is_letter(XrString *str) {
    if (!str || str->length == 0)
        return false;

    const char *p = str->data;
    const char *end = p + str->length;

    while (p < end) {
        uint32_t cp;
        int bytes = xr_utf8_decode(p, end - p, &cp);
        if (!xr_unicode_is_letter(cp))
            return false;
        p += bytes;
    }
    return true;
}

bool xr_string_is_number(XrString *str) {
    if (!str || str->length == 0)
        return false;

    const char *p = str->data;
    const char *end = p + str->length;

    while (p < end) {
        uint32_t cp;
        int bytes = xr_utf8_decode(p, end - p, &cp);
        if (!xr_unicode_is_number(cp))
            return false;
        p += bytes;
    }
    return true;
}

bool xr_string_is_alnum(XrString *str) {
    if (!str || str->length == 0)
        return false;

    const char *p = str->data;
    const char *end = p + str->length;

    while (p < end) {
        uint32_t cp;
        int bytes = xr_utf8_decode(p, end - p, &cp);
        if (!xr_unicode_is_alnum(cp))
            return false;
        p += bytes;
    }
    return true;
}

bool xr_string_is_whitespace_str(XrString *str) {
    if (!str || str->length == 0)
        return false;

    const char *p = str->data;
    const char *end = p + str->length;

    while (p < end) {
        uint32_t cp;
        int bytes = xr_utf8_decode(p, end - p, &cp);
        if (!xr_unicode_is_whitespace(cp))
            return false;
        p += bytes;
    }
    return true;
}

int32_t xr_string_ord(XrString *str) {
    if (!str || str->length == 0)
        return -1;

    /* Single-byte strings from byteAt() on binary buffers are raw octets,
     * not UTF-8 code units — return the unsigned byte value directly. */
    if (str->length == 1)
        return (unsigned char) str->data[0];

    uint32_t cp;
    xr_utf8_decode(str->data, str->length, &cp);
    return (int32_t) cp;
}
