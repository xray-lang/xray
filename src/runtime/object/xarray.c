/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xarray.c - Dynamic array implementation
 *
 * KEY CONCEPT:
 *   Array objects and element storage both live on Immix GC heap.
 *   Element data uses XR_TBLOB (GC header + raw bytes), so GC sweep
 *   preserves the lines. Old data buffers are reclaimed automatically.
 *   System heap arrays (shared) still use malloc for element storage.
 *   Slices share backing store with source array (zero-copy).
 */

#include "xarray.h"
#include "xstring.h"
#include "../xisolate_api.h"
#include "../../base/xchecks.h"
#include "../class/xclass.h"
#include "../gc/xalloc_unified.h"
#include "../class/xclass_system.h"
#include "../gc/xgc.h"
#include "../../base/xmalloc.h"
#include "../value/xvalue_hash.h"
#include "../xstrbuf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../xvm_call.h"

/* ====== Creation and Destruction ====== */

XrArray *xr_array_new(struct XrCoroutine *coro) {
    return xr_array_with_capacity(coro, 0);
}

XrArray *xr_array_with_capacity(struct XrCoroutine *coro, int capacity) {
    return xr_array_with_capacity_typed(coro, capacity, XR_ELEM_ANY);
}

XrArray *xr_array_with_capacity_typed(struct XrCoroutine *coro, int capacity,
                                      XrArrayElemType elem_type) {
    XR_DCHECK(coro != NULL, "array_with_capacity: NULL coro");
    XR_DCHECK(capacity >= 0, "array_with_capacity: negative capacity");
    XR_DCHECK(elem_type < XR_ELEM_COUNT, "array_with_capacity: invalid elem_type");
    // Allocate on coroutine heap
    XrArray *arr = (XrArray *) xr_alloc(coro, sizeof(XrArray), XR_TARRAY);

    if (!arr) {
        return NULL;
    }

    xr_gc_header_init_type(&arr->gc, XR_TARRAY);

    uint8_t esz = (elem_type < XR_ELEM_COUNT) ? XR_ELEM_SIZES[elem_type] : 8;
    arr->data = NULL;
    arr->length = 0;
    arr->capacity = capacity;
    arr->source = NULL;
    arr->elem_type = (uint8_t) elem_type;
    arr->elem_size = esz;
    arr->elem_tid = 0;
    arr->has_gc_ptrs = 0;
    arr->data_on_gc_heap = 0;
    memset(arr->_pad, 0, sizeof(arr->_pad));

    // Allocate data as GC blob on Immix heap (no free needed, GC reclaims)
    if (capacity > 0) {
        size_t data_bytes = (size_t) esz * capacity;
        XrCoroGC *gc = xr_coro_get_coro_gc(coro);
        if (gc) {
            arr->data = xr_coro_alloc_blob(gc, data_bytes);
            if (arr->data) {
                arr->data_on_gc_heap = 1;
            }
        } else {
            // Fallback: no coro_gc available, use malloc
            arr->data = xr_malloc(data_bytes);
        }
    }

    return arr;
}

// Initialize array in-place (for shared arrays on system heap)
// GC header must be set by caller
void xr_array_init_inplace(XrArray *arr, int capacity, uint8_t elem_type) {
    if (!arr)
        return;

    uint8_t esz = (elem_type < XR_ELEM_COUNT) ? XR_ELEM_SIZES[elem_type] : 8;
    arr->data = NULL;
    arr->length = 0;
    arr->capacity = capacity;
    arr->source = NULL;
    arr->elem_type = elem_type;
    arr->elem_size = esz;
    arr->elem_tid = 0;
    arr->has_gc_ptrs = 0;
    arr->data_on_gc_heap = 0;  // always 0 for inplace arrays
    memset(arr->_pad, 0, sizeof(arr->_pad));

    // Allocate data (no GC accounting for system heap arrays)
    if (capacity > 0) {
        arr->data = xr_malloc((size_t) esz * capacity);
    }
}

XrArray *xr_array_from_values(struct XrCoroutine *coro, XrValue *elements, int count) {
    XR_DCHECK(coro != NULL, "array_from_values: NULL coro");
    XR_DCHECK(count >= 0, "array_from_values: negative count");
    XR_DCHECK(count == 0 || elements != NULL, "array_from_values: NULL elements with count > 0");
    XrArray *arr = xr_array_with_capacity(coro, count);
    if (!arr)
        return NULL;

    // Copy elements (always XR_ELEM_ANY)
    XrValue *data = (XrValue *) arr->data;
    for (int i = 0; i < count; i++) {
        data[i] = elements[i];
        XR_ARRAY_MARK_GC_PTRS(arr, elements[i]);
    }
    arr->length = count;

    return arr;
}

/* ====== Element Access ====== */

XrValue xr_array_get(XrArray *arr, int index) {
    XR_DCHECK(arr != NULL, "array_get: NULL array");
    XR_DCHECK(XR_GC_GET_TYPE(&arr->gc) == XR_TARRAY || XR_GC_GET_TYPE(&arr->gc) == XR_TARRAY_SLICE,
              "array_get: object is not an array");
    // Bounds check
    if (index < 0 || index >= arr->length) {
        return xr_null();
    }

    return xr_array_get_element(arr, index);
}

// Direct set without bounds check (for multi-threaded scenarios like await.all)
// Thread-safe when each thread writes to different, non-overlapping indices
void xr_array_set_direct(XrArray *arr, int index, XrValue value) {
    XR_DCHECK(arr != NULL, "array_set_direct: NULL array");
    XR_DCHECK(index >= 0 && index < arr->capacity, "array_set_direct: index out of capacity");
    // Caller must ensure valid index and pre-allocated capacity
    xr_array_set_element(arr, index, value);
    if (XR_ARRAY_IS_GC_TRACED(arr))
        XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), arr);
}

void xr_array_set(XrArray *arr, int index, XrValue value) {
    XR_DCHECK(arr != NULL, "array_set: NULL array");
    XR_DCHECK(XR_GC_GET_TYPE(&arr->gc) == XR_TARRAY || XR_GC_GET_TYPE(&arr->gc) == XR_TARRAY_SLICE,
              "array_set: object is not an array");
    // Negative index check
    if (index < 0) {
        return;
    }

    // Extend array if index exceeds current length
    if (index >= arr->length) {
        // Slices cannot resize
        if (xr_array_is_slice(arr)) {
            return;
        }

        xr_array_ensure_capacity(arr, index + 1);

        // Fill gap with zero
        if (arr->elem_type == XR_ELEM_ANY) {
            XrValue *data = (XrValue *) arr->data;
            for (int i = arr->length; i < index; i++) {
                data[i] = xr_null();
            }
        } else {
            // Zero-fill gap for typed arrays
            memset((uint8_t *) arr->data + (size_t) arr->length * arr->elem_size, 0,
                   (size_t) (index - arr->length) * arr->elem_size);
        }

        arr->length = index + 1;
    }

    xr_array_set_element(arr, index, value);
    if (XR_ARRAY_IS_GC_TRACED(arr))
        XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), arr);
}

int xr_array_size(XrArray *arr) {
    XR_DCHECK(arr != NULL, "array_size: NULL array");
    return arr->length;
}

/* ====== Array Modification ====== */

void xr_array_push(XrArray *arr, XrValue value) {
    XR_DCHECK(arr != NULL, "array_push: NULL array");
    XR_DCHECK(XR_GC_GET_TYPE(&arr->gc) == XR_TARRAY || XR_GC_GET_TYPE(&arr->gc) == XR_TARRAY_SLICE,
              "array_push: object is not an array");
    // Slices cannot push
    if (xr_array_is_slice(arr)) {
        return;
    }

    if (arr->length >= arr->capacity) {
        xr_array_grow(arr);
    }

    xr_array_set_element(arr, arr->length++, value);
    XR_DCHECK(arr->length <= arr->capacity, "array_push: length > capacity after push");
    if (XR_ARRAY_IS_GC_TRACED(arr))
        XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), arr);
}

XrValue xr_array_pop(XrArray *arr) {
    XR_DCHECK(arr != NULL, "array_pop: NULL array");
    if (arr->length == 0) {
        return xr_null();
    }

    // Slices cannot pop
    if (xr_array_is_slice(arr)) {
        return xr_null();
    }

    arr->length--;
    return xr_array_get_element(arr, arr->length);
}

void xr_array_unshift(XrArray *arr, XrValue value) {
    XR_DCHECK(arr != NULL, "array_unshift: NULL array");
    // Slices cannot unshift
    if (xr_array_is_slice(arr)) {
        return;
    }

    if (arr->length >= arr->capacity) {
        xr_array_grow(arr);
    }

    // Shift all elements right by one (use memmove for typed arrays)
    memmove((uint8_t *) arr->data + arr->elem_size, arr->data,
            (size_t) arr->length * arr->elem_size);

    xr_array_set_element(arr, 0, value);
    arr->length++;
}

XrValue xr_array_shift(XrArray *arr) {
    XR_DCHECK(arr != NULL, "array_shift: NULL array");
    if (arr->length == 0) {
        return xr_null();
    }

    // Slices cannot shift
    if (xr_array_is_slice(arr)) {
        return xr_null();
    }

    XrValue first = xr_array_get_element(arr, 0);

    // Shift all elements left by one (use memmove for typed arrays)
    if (arr->length > 1) {
        memmove(arr->data, (uint8_t *) arr->data + arr->elem_size,
                (size_t) (arr->length - 1) * arr->elem_size);
    }

    arr->length--;
    return first;
}

void xr_array_clear(XrArray *arr) {
    XR_DCHECK(arr != NULL, "array_clear: NULL array");
    arr->length = 0;
}

/* ====== Query Methods ====== */

int xr_array_index_of(XrArray *arr, XrValue value) {
    XR_DCHECK(arr != NULL, "array_index_of: NULL array");
    for (int i = 0; i < arr->length; i++) {
        if (xr_value_eq(xr_array_get_element(arr, i), value)) {
            return i;
        }
    }
    return -1;
}

bool xr_array_has(XrArray *arr, XrValue value) {
    return xr_array_index_of(arr, value) != -1;
}

bool xr_array_is_empty(XrArray *arr) {
    XR_DCHECK(arr != NULL, "array_is_empty: NULL array");
    return arr->length == 0;
}

/* ====== Higher-Order Methods ====== */

void xr_array_foreach(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback) {
    if (!arr || !callback)
        return;

    for (int i = 0; i < arr->length; i++) {
        XrValue args[1];
        args[0] = xr_array_get_element(arr, i);
        xr_vm_call_closure(iso, callback, args, 1);
    }
}

// Runtime implementation for dynamic dispatch (inline compilation handles static types)

XrArray *xr_array_map(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback) {
    if (!arr || !callback)
        return xr_array_new(xr_current_coro(iso));
    // map always returns Array<any> (callback return type unknown at runtime)
    XrArray *result = xr_array_with_capacity(xr_current_coro(iso), arr->length);
    XrValue *rdata = (XrValue *) result->data;

    for (int i = 0; i < arr->length; i++) {
        XrValue args[1];
        args[0] = xr_array_get_element(arr, i);
        rdata[i] = xr_vm_call_closure(iso, callback, args, 1);
    }
    result->length = arr->length;

    return result;
}

XrArray *xr_array_filter(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback) {
    if (!arr || !callback)
        return xr_array_new(xr_current_coro(iso));
    // filter preserves source elem_type
    XrArray *result = xr_array_with_capacity_typed(xr_current_coro(iso), arr->length,
                                                   (XrArrayElemType) arr->elem_type);

    for (int i = 0; i < arr->length; i++) {
        XrValue elem = xr_array_get_element(arr, i);
        XrValue args[1];
        args[0] = elem;
        XrValue test_result = xr_vm_call_closure(iso, callback, args, 1);

        if (xr_vm_is_truthy(test_result)) {
            xr_array_push(result, elem);
        }
    }

    return result;
}

XrValue xr_array_reduce(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback,
                        XrValue initial) {
    if (!arr || !callback)
        return initial;
    XrValue accumulator = initial;

    for (int i = 0; i < arr->length; i++) {
        XrValue args[2];
        args[0] = accumulator;
        args[1] = xr_array_get_element(arr, i);
        accumulator = xr_vm_call_closure(iso, callback, args, 2);
    }

    return accumulator;
}

XrValue xr_array_find(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback) {
    if (!arr || !callback)
        return xr_null();
    for (int i = 0; i < arr->length; i++) {
        XrValue elem = xr_array_get_element(arr, i);
        XrValue args[1];
        args[0] = elem;
        XrValue result = xr_vm_call_closure(iso, callback, args, 1);
        if (xr_vm_is_truthy(result)) {
            return elem;
        }
    }
    return xr_null();
}

int xr_array_find_index(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback) {
    if (!arr || !callback)
        return -1;
    for (int i = 0; i < arr->length; i++) {
        XrValue args[1];
        args[0] = xr_array_get_element(arr, i);
        XrValue result = xr_vm_call_closure(iso, callback, args, 1);
        if (xr_vm_is_truthy(result)) {
            return i;
        }
    }
    return -1;
}

bool xr_array_every(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback) {
    if (!arr || !callback)
        return true;
    for (int i = 0; i < arr->length; i++) {
        XrValue args[1];
        args[0] = xr_array_get_element(arr, i);
        XrValue result = xr_vm_call_closure(iso, callback, args, 1);
        if (!xr_vm_is_truthy(result)) {
            return false;
        }
    }
    return true;
}

bool xr_array_some(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback) {
    if (!arr || !callback)
        return false;
    for (int i = 0; i < arr->length; i++) {
        XrValue args[1];
        args[0] = xr_array_get_element(arr, i);
        XrValue result = xr_vm_call_closure(iso, callback, args, 1);
        if (xr_vm_is_truthy(result)) {
            return true;
        }
    }
    return false;
}

// Typed fill macro: write native value directly without switch dispatch
#define TYPED_FILL(type, arr, val, start, end)                                                     \
    do {                                                                                           \
        type *d = (type *) (arr)->data;                                                            \
        type v = (val);                                                                            \
        for (int _i = (start); _i < (end); _i++)                                                   \
            d[_i] = v;                                                                             \
    } while (0)

void xr_array_fill(XrArray *arr, XrValue value, int start, int end) {
    if (!arr)
        return;
    if (start < 0)
        start = 0;
    if (end > (int) arr->length)
        end = (int) arr->length;
    if (start >= end)
        return;

    int count = end - start;

    if (arr->elem_type == XR_ELEM_ANY) {
        // ANY array: direct XrValue fill
        XrValue *data = (XrValue *) arr->data;
        for (int i = start; i < end; i++)
            data[i] = value;
        XR_ARRAY_MARK_GC_PTRS(arr, value);
        return;
    }

    // Typed arrays: extract native value and fill directly
    switch (arr->elem_type) {
        case XR_ELEM_I8:
            TYPED_FILL(
                int8_t, arr,
                (int8_t) (XR_IS_INT(value) ? XR_TO_INT(value) : (int64_t) XR_TO_FLOAT(value)),
                start, end);
            break;
        case XR_ELEM_U8: {
            // Special case: memset for byte arrays
            uint8_t v =
                (uint8_t) (XR_IS_INT(value) ? XR_TO_INT(value) : (int64_t) XR_TO_FLOAT(value));
            memset((uint8_t *) arr->data + start, v, (size_t) count);
            break;
        }
        case XR_ELEM_I16:
            TYPED_FILL(
                int16_t, arr,
                (int16_t) (XR_IS_INT(value) ? XR_TO_INT(value) : (int64_t) XR_TO_FLOAT(value)),
                start, end);
            break;
        case XR_ELEM_U16:
            TYPED_FILL(
                uint16_t, arr,
                (uint16_t) (XR_IS_INT(value) ? XR_TO_INT(value) : (int64_t) XR_TO_FLOAT(value)),
                start, end);
            break;
        case XR_ELEM_I32:
            TYPED_FILL(
                int32_t, arr,
                (int32_t) (XR_IS_INT(value) ? XR_TO_INT(value) : (int64_t) XR_TO_FLOAT(value)),
                start, end);
            break;
        case XR_ELEM_U32:
            TYPED_FILL(
                uint32_t, arr,
                (uint32_t) (XR_IS_INT(value) ? XR_TO_INT(value) : (int64_t) XR_TO_FLOAT(value)),
                start, end);
            break;
        case XR_ELEM_I64:
            TYPED_FILL(
                int64_t, arr,
                (int64_t) (XR_IS_INT(value) ? XR_TO_INT(value) : (int64_t) XR_TO_FLOAT(value)),
                start, end);
            break;
        case XR_ELEM_U64:
            TYPED_FILL(
                uint64_t, arr,
                (uint64_t) (XR_IS_INT(value) ? XR_TO_INT(value) : (int64_t) XR_TO_FLOAT(value)),
                start, end);
            break;
        case XR_ELEM_F32:
            TYPED_FILL(
                float, arr,
                (float) (XR_IS_FLOAT(value) ? XR_TO_FLOAT(value) : (double) XR_TO_INT(value)),
                start, end);
            break;
        case XR_ELEM_F64:
            TYPED_FILL(
                double, arr,
                (double) (XR_IS_FLOAT(value) ? XR_TO_FLOAT(value) : (double) XR_TO_INT(value)),
                start, end);
            break;
        case XR_ELEM_BOOL: {
            uint8_t v = xr_vm_is_truthy(value) ? 1 : 0;
            memset((uint8_t *) arr->data + start, v, (size_t) count);
            break;
        }
        default:
            break;
    }
}

// Sort helper: compare two XrValues for default ordering
static int xr_value_compare_default(XrValue a, XrValue b) {
    if (XR_IS_INT(a) && XR_IS_INT(b)) {
        int64_t ia = XR_TO_INT(a), ib = XR_TO_INT(b);
        return (ia > ib) - (ia < ib);
    }
    if (XR_IS_FLOAT(a) && XR_IS_FLOAT(b)) {
        double fa = XR_TO_FLOAT(a), fb = XR_TO_FLOAT(b);
        return (fa > fb) - (fa < fb);
    }
    if (XR_IS_INT(a) && XR_IS_FLOAT(b)) {
        double fa = (double) XR_TO_INT(a), fb = XR_TO_FLOAT(b);
        return (fa > fb) - (fa < fb);
    }
    if (XR_IS_FLOAT(a) && XR_IS_INT(b)) {
        double fa = XR_TO_FLOAT(a), fb = (double) XR_TO_INT(b);
        return (fa > fb) - (fa < fb);
    }
    if (XR_IS_STRING(a) && XR_IS_STRING(b)) {
        const char *da = xr_value_str_data(&a);
        uint32_t la = xr_value_str_len(&a);
        const char *db = xr_value_str_data(&b);
        uint32_t lb = xr_value_str_len(&b);
        int minlen = la < lb ? la : lb;
        int cmp = memcmp(da, db, minlen);
        if (cmp != 0)
            return cmp;
        return (la > lb) - (la < lb);
    }
    return 0;
}

// Sort context for qsort_r / comparator closure
typedef struct {
    XrayIsolate *iso;
    struct XrClosure *comparator;
} XrSortCtx;

static int xr_sort_with_comparator(const void *a, const void *b, void *ctx_ptr) {
    XrSortCtx *ctx = (XrSortCtx *) ctx_ptr;
    XrValue va = *(const XrValue *) a;
    XrValue vb = *(const XrValue *) b;
    if (ctx->comparator) {
        XrValue args[2] = {va, vb};
        XrValue result = xr_vm_call_closure(ctx->iso, ctx->comparator, args, 2);
        if (XR_IS_INT(result))
            return (int) XR_TO_INT(result);
        if (XR_IS_FLOAT(result)) {
            double d = XR_TO_FLOAT(result);
            return (d > 0) - (d < 0);
        }
        return 0;
    }
    return xr_value_compare_default(va, vb);
}

#define XR_SORT_INSERTION_THRESHOLD 32

// Insertion sort for small segments (stable, low overhead)
static void xr_sort_insertion(XrValue *data, int lo, int hi, XrSortCtx *ctx) {
    for (int i = lo + 1; i <= hi; i++) {
        XrValue key = data[i];
        int j = i - 1;
        while (j >= lo && xr_sort_with_comparator(&data[j], &key, ctx) > 0) {
            data[j + 1] = data[j];
            j--;
        }
        data[j + 1] = key;
    }
}

// Merge two sorted runs: data[lo..mid] and data[mid+1..hi] into data[lo..hi]
static void xr_sort_merge(XrValue *data, XrValue *tmp, int lo, int mid, int hi, XrSortCtx *ctx) {
    memcpy(tmp + lo, data + lo, (size_t) (hi - lo + 1) * sizeof(XrValue));
    int i = lo, j = mid + 1, k = lo;
    while (i <= mid && j <= hi) {
        if (xr_sort_with_comparator(&tmp[i], &tmp[j], ctx) <= 0)
            data[k++] = tmp[i++];
        else
            data[k++] = tmp[j++];
    }
    while (i <= mid)
        data[k++] = tmp[i++];
    while (j <= hi)
        data[k++] = tmp[j++];
}

// Hybrid sort: insertion sort for small runs, bottom-up merge sort for large arrays
static void xr_array_hybrid_sort(XrValue *data, int n, XrSortCtx *ctx) {
    if (n <= XR_SORT_INSERTION_THRESHOLD) {
        xr_sort_insertion(data, 0, n - 1, ctx);
        return;
    }

    // Allocate temp buffer for merge
    XrValue *tmp = (XrValue *) xr_malloc((size_t) n * sizeof(XrValue));
    if (!tmp) {
        // Fallback to insertion sort on OOM
        xr_sort_insertion(data, 0, n - 1, ctx);
        return;
    }

    // Phase 1: sort small runs with insertion sort
    for (int i = 0; i < n; i += XR_SORT_INSERTION_THRESHOLD) {
        int hi = i + XR_SORT_INSERTION_THRESHOLD - 1;
        if (hi >= n)
            hi = n - 1;
        xr_sort_insertion(data, i, hi, ctx);
    }

    // Phase 2: bottom-up merge
    for (int width = XR_SORT_INSERTION_THRESHOLD; width < n; width *= 2) {
        for (int lo = 0; lo < n; lo += width * 2) {
            int mid = lo + width - 1;
            int hi = lo + width * 2 - 1;
            if (mid >= n)
                break;  // only one run left, already sorted
            if (hi >= n)
                hi = n - 1;
            xr_sort_merge(data, tmp, lo, mid, hi, ctx);
        }
    }

    xr_free(tmp);
}

// Typed insertion sort for small segments
#define TYPED_INSERTION(type, d, lo, hi)                                                           \
    do {                                                                                           \
        for (int _i = (lo) + 1; _i <= (hi); _i++) {                                                \
            type _key = (d)[_i];                                                                   \
            int _j = _i - 1;                                                                       \
            while (_j >= (lo) && (d)[_j] > _key) {                                                 \
                (d)[_j + 1] = (d)[_j];                                                             \
                _j--;                                                                              \
            }                                                                                      \
            (d)[_j + 1] = _key;                                                                    \
        }                                                                                          \
    } while (0)

// Typed merge for numeric arrays
#define TYPED_MERGE(type, d, tmp, lo, mid, hi)                                                     \
    do {                                                                                           \
        memcpy((tmp) + (lo), (d) + (lo), (size_t) ((hi) - (lo) + 1) * sizeof(type));               \
        int _i = (lo), _j = (mid) + 1, _k = (lo);                                                  \
        while (_i <= (mid) && _j <= (hi)) {                                                        \
            if ((tmp)[_i] <= (tmp)[_j])                                                            \
                (d)[_k++] = (tmp)[_i++];                                                           \
            else                                                                                   \
                (d)[_k++] = (tmp)[_j++];                                                           \
        }                                                                                          \
        while (_i <= (mid))                                                                        \
            (d)[_k++] = (tmp)[_i++];                                                               \
        while (_j <= (hi))                                                                         \
            (d)[_k++] = (tmp)[_j++];                                                               \
    } while (0)

// Typed hybrid sort (insertion + merge)
#define TYPED_SORT(type, arr, n)                                                                   \
    do {                                                                                           \
        type *_d = (type *) (arr)->data;                                                           \
        int _n = (n);                                                                              \
        if (_n <= XR_SORT_INSERTION_THRESHOLD) {                                                   \
            TYPED_INSERTION(type, _d, 0, _n - 1);                                                  \
        } else {                                                                                   \
            type *_tmp = (type *) xr_malloc((size_t) _n * sizeof(type));                           \
            if (!_tmp) {                                                                           \
                TYPED_INSERTION(type, _d, 0, _n - 1);                                              \
                break;                                                                             \
            }                                                                                      \
            for (int _r = 0; _r < _n; _r += XR_SORT_INSERTION_THRESHOLD) {                         \
                int _hi = _r + XR_SORT_INSERTION_THRESHOLD - 1;                                    \
                if (_hi >= _n)                                                                     \
                    _hi = _n - 1;                                                                  \
                TYPED_INSERTION(type, _d, _r, _hi);                                                \
            }                                                                                      \
            for (int _w = XR_SORT_INSERTION_THRESHOLD; _w < _n; _w *= 2) {                         \
                for (int _lo = 0; _lo < _n; _lo += _w * 2) {                                       \
                    int _mid = _lo + _w - 1;                                                       \
                    int _hi2 = _lo + _w * 2 - 1;                                                   \
                    if (_mid >= _n)                                                                \
                        break;                                                                     \
                    if (_hi2 >= _n)                                                                \
                        _hi2 = _n - 1;                                                             \
                    TYPED_MERGE(type, _d, _tmp, _lo, _mid, _hi2);                                  \
                }                                                                                  \
            }                                                                                      \
            xr_free(_tmp);                                                                         \
        }                                                                                          \
    } while (0)

// Typed sort with custom comparator: box elements into temp XrValue array,
// sort with hybrid merge sort, then unbox back
static void xr_array_typed_sort_with_comparator(XrayIsolate *iso, XrArray *arr,
                                                struct XrClosure *cmp) {
    int n = arr->length;
    // Box all elements into a temp XrValue array
    XrValue *boxed = (XrValue *) xr_malloc((size_t) n * sizeof(XrValue));
    if (!boxed)
        return;
    for (int i = 0; i < n; i++)
        boxed[i] = xr_array_get_element(arr, i);
    // Sort the boxed array
    XrSortCtx ctx = {iso, cmp};
    xr_array_hybrid_sort(boxed, n, &ctx);
    // Unbox back
    for (int i = 0; i < n; i++)
        xr_array_set_element(arr, i, boxed[i]);
    xr_free(boxed);
}

void xr_array_sort(XrayIsolate *iso, XrArray *arr, struct XrClosure *comparator) {
    if (!arr || arr->length <= 1)
        return;

    if (arr->elem_type == XR_ELEM_ANY) {
        XrSortCtx ctx = {iso, comparator};
        xr_array_hybrid_sort((XrValue *) arr->data, (int) arr->length, &ctx);
        return;
    }

    // Typed arrays with custom comparator: box/unbox through callback
    if (comparator) {
        xr_array_typed_sort_with_comparator(iso, arr, comparator);
        return;
    }

    // Default numeric sort (ascending)
    int n = arr->length;
    switch (arr->elem_type) {
        case XR_ELEM_I8:
            TYPED_SORT(int8_t, arr, n);
            break;
        case XR_ELEM_U8:
            TYPED_SORT(uint8_t, arr, n);
            break;
        case XR_ELEM_I16:
            TYPED_SORT(int16_t, arr, n);
            break;
        case XR_ELEM_U16:
            TYPED_SORT(uint16_t, arr, n);
            break;
        case XR_ELEM_I32:
            TYPED_SORT(int32_t, arr, n);
            break;
        case XR_ELEM_U32:
            TYPED_SORT(uint32_t, arr, n);
            break;
        case XR_ELEM_I64:
            TYPED_SORT(int64_t, arr, n);
            break;
        case XR_ELEM_U64:
            TYPED_SORT(uint64_t, arr, n);
            break;
        case XR_ELEM_F32:
            TYPED_SORT(float, arr, n);
            break;
        case XR_ELEM_F64:
            TYPED_SORT(double, arr, n);
            break;
        default:
            break;
    }
}

/* ====== Utility Methods ====== */

void xr_array_reverse(XrArray *arr) {
    if (!arr || arr->length <= 1)
        return;

    int left = 0;
    int right = arr->length - 1;
    uint8_t esz = arr->elem_size;
    uint8_t tmp[16];  // max elem_size is 16 (XrValue Tagged Union)

    while (left < right) {
        uint8_t *lp = (uint8_t *) arr->data + (size_t) left * esz;
        uint8_t *rp = (uint8_t *) arr->data + (size_t) right * esz;
        memcpy(tmp, lp, esz);
        memcpy(lp, rp, esz);
        memcpy(rp, tmp, esz);
        left++;
        right--;
    }
}

XrArray *xr_array_copy(struct XrCoroutine *coro, XrArray *arr) {
    if (!arr)
        return xr_array_new(coro);
    if (arr->elem_type == XR_ELEM_ANY) {
        return xr_array_from_values(coro, (XrValue *) arr->data, arr->length);
    }
    // Typed array: allocate same type and memcpy
    XrArray *new_arr =
        xr_array_with_capacity_typed(coro, arr->length, (XrArrayElemType) arr->elem_type);
    if (!new_arr)
        return NULL;
    new_arr->elem_tid = arr->elem_tid;
    if (arr->length > 0) {
        memcpy(new_arr->data, arr->data, (size_t) arr->length * arr->elem_size);
    }
    new_arr->length = arr->length;
    return new_arr;
}

/* ====== Internal Functions ====== */

void xr_array_grow(XrArray *arr) {
    // Slices cannot grow
    if (xr_array_is_slice(arr)) {
        return;
    }

    int32_t old_capacity = arr->capacity;
    int32_t new_capacity = old_capacity == 0 ? XR_ARRAY_INIT_CAPACITY : old_capacity * 2;

    size_t old_bytes = (size_t) arr->elem_size * old_capacity;
    size_t new_bytes = (size_t) arr->elem_size * new_capacity;

    if (arr->data_on_gc_heap) {
        /* Force malloc during grow to avoid Immix blob overlap.
         * GC blob allocation may return memory overlapping with
         * the old data array, making memcpy undefined behavior. */
        void *new_data = xr_malloc(new_bytes);
        if (!new_data)
            return;
        if (arr->data && old_bytes > 0) {
            memcpy(new_data, arr->data, old_bytes);
        }
        arr->data = new_data;
        arr->data_on_gc_heap = 0;
        arr->capacity = new_capacity;
    } else {
        // System heap path: realloc + external memory accounting
        void *new_data = xr_realloc(arr->data, new_bytes);
        if (!new_data)
            return;
        arr->data = new_data;
        arr->capacity = new_capacity;
        XrCoroGC *gc = xr_current_coro_gc();
        xr_gc_add_external(gc, (int64_t) (new_bytes - old_bytes));
    }
}

void xr_array_ensure_capacity(XrArray *arr, int min_capacity) {
    XR_DCHECK(arr != NULL, "ensure_capacity: NULL array");
    XR_DCHECK(min_capacity >= 0, "ensure_capacity: negative min_capacity");
    // Slices cannot grow
    if (xr_array_is_slice(arr)) {
        return;
    }

    if (arr->capacity >= min_capacity) {
        return;
    }

    int32_t old_capacity = arr->capacity;
    int32_t new_capacity = old_capacity;
    if (new_capacity == 0) {
        new_capacity = XR_ARRAY_INIT_CAPACITY;
    }

    while (new_capacity < min_capacity) {
        new_capacity *= 2;
    }

    size_t old_bytes = (size_t) arr->elem_size * old_capacity;
    size_t new_bytes = (size_t) arr->elem_size * new_capacity;

    if (arr->data_on_gc_heap) {
        // Force malloc during ensure_capacity to avoid Immix blob overlap.
        void *new_data = xr_malloc(new_bytes);
        if (!new_data)
            return;
        if (arr->data && old_bytes > 0) {
            memcpy(new_data, arr->data, old_bytes);
        }
        arr->data = new_data;
        arr->data_on_gc_heap = 0;
        arr->capacity = new_capacity;
    } else {
        // System heap path: realloc + external memory accounting
        void *new_data = xr_realloc(arr->data, new_bytes);
        if (!new_data)
            return;
        arr->data = new_data;
        arr->capacity = new_capacity;
        XrCoroGC *gc = xr_current_coro_gc();
        xr_gc_add_external(gc, (int64_t) (new_bytes - old_bytes));
    }
}

// Join array elements with delimiter into string (O(n) via XrStrBuf)
struct XrString *xr_array_join(XrayIsolate *iso, XrArray *arr, struct XrString *delimiter) {
    if (arr == NULL || arr->length == 0) {
        return xr_string_intern(iso, "", 0, 0);
    }

    XrStrBuf *sb = xr_strbuf_tmp(iso);

    for (int i = 0; i < arr->length; i++) {
        if (i > 0 && delimiter != NULL) {
            xr_strbuf_append_str(sb, delimiter);
        }

        XrValue val = xr_array_get_element(arr, i);

        if (XR_IS_STRING(val)) {
            xr_strbuf_append_cstr(sb, xr_value_str_data(&val), xr_value_str_len(&val));
        } else if (XR_IS_INT(val)) {
            xr_strbuf_append_int(sb, XR_TO_INT(val));
        } else if (XR_IS_FLOAT(val)) {
            xr_strbuf_append_float(sb, XR_TO_FLOAT(val));
        } else if (XR_IS_BOOL(val)) {
            const char *s = XR_TO_BOOL(val) ? "true" : "false";
            xr_strbuf_append_cstr(sb, s, strlen(s));
        } else if (XR_IS_NULL(val)) {
            xr_strbuf_append_cstr(sb, "null", 4);
        } else {
            xr_strbuf_append_cstr(sb, "[object]", 8);
        }
    }

    return xr_strbuf_to_string(sb);
}

/* ====== Slice Operations (zero-copy) ====== */

// Create array slice with direct data pointer offset
// capacity = 0 marks slice as non-resizable
XrArray *xr_array_slice(struct XrCoroutine *coro, XrArray *arr, int32_t start, int32_t end) {
    if (!coro || !arr)
        return NULL;

    int32_t len = arr->length;

    // Normalize indices
    if (start < 0)
        start = 0;
    if (end < 0 || end > len)
        end = len;
    if (start > end)
        start = end;

    // Allocate Slice on coroutine heap
    XrArray *slice = (XrArray *) xr_alloc(coro, sizeof(XrArray), XR_TARRAY_SLICE);
    if (!slice)
        return NULL;

    // Direct pointer offset (zero-copy, using elem_size)
    slice->data = (uint8_t *) arr->data + (size_t) start * arr->elem_size;
    slice->length = end - start;
    slice->capacity = 0;  // Mark as non-resizable

    // Track source for GC (chase to original if source is also a slice)
    slice->source = arr->source ? arr->source : arr;

    // Inherit elem_type, elem_tid, and has_gc_ptrs from source
    slice->elem_type = arr->elem_type;
    slice->elem_size = arr->elem_size;
    slice->elem_tid = arr->elem_tid;
    slice->has_gc_ptrs = arr->has_gc_ptrs;
    slice->data_on_gc_heap = 0;  // Slice doesn't own the blob; source array marks it
    memset(slice->_pad, 0, sizeof(slice->_pad));

    return slice;
}

// Copy slice to independent array
XrArray *xr_array_slice_to_array(struct XrCoroutine *coro, XrArray *slice) {
    if (!coro || !slice)
        return xr_array_new(coro);

    // If already independent array, return self
    if (slice->source == NULL && slice->capacity > 0) {
        return slice;
    }

    // Copy to new array (preserving elem_type and elem_tid)
    XrArray *arr =
        xr_array_with_capacity_typed(coro, slice->length, (XrArrayElemType) slice->elem_type);
    if (!arr)
        return NULL;

    arr->elem_tid = slice->elem_tid;
    if (slice->length > 0) {
        memcpy(arr->data, slice->data, (size_t) slice->length * slice->elem_size);
    }
    arr->length = slice->length;

    return arr;
}

/* ========== GC Integration ========== */

void xr_gc_destroy_array(XrGCHeader *obj, struct XrCoroGC *owning_gc) {
    XrArray *arr = (XrArray *) obj;
    // Only free data if this is not a slice (capacity > 0 means owns data)
    if (arr->data && arr->capacity > 0) {
        if (arr->data_on_gc_heap) {
            // GC blob: no free needed, Immix sweep reclaims the blob
            arr->data = NULL;
        } else {
            // System heap: free and update external memory accounting
            size_t data_bytes = (size_t) arr->elem_size * arr->capacity;
            xr_free(arr->data);
            arr->data = NULL;
            xr_gc_sub_external(owning_gc, (int64_t) data_bytes);
        }
    }
}

/* ====== Bytes Convenience Functions ====== */

void xr_array_append_data(XrArray *arr, const uint8_t *src_data, int32_t len) {
    if (!arr || !src_data || len <= 0)
        return;
    if (arr->elem_type != XR_ELEM_U8)
        return;

    xr_array_ensure_capacity(arr, arr->length + len);
    memcpy((uint8_t *) arr->data + arr->length, src_data, len);
    arr->length += len;
}

struct XrString *xr_array_to_string(XrayIsolate *iso, XrArray *arr) {
    if (!arr || arr->length == 0) {
        return xr_string_intern(iso, "", 0, 0);
    }
    return xr_string_intern(iso, (const char *) arr->data, arr->length, 0);
}
