/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xarray.h - Dynamic array object with slice support
 *
 * KEY CONCEPT:
 *   - Dynamic resizing
 *   - Zero-copy slicing (shared backing store)
 *   - Basic methods: push, pop, unshift, shift
 *   - Higher-order: map, filter, forEach, reduce
 */

#ifndef XARRAY_H
#define XARRAY_H

#include "../value/xvalue.h"
#include "../gc/xgc_header.h"
#include "../../base/xmalloc.h"
#include "../../base/xdefs.h"

// Forward declaration

// Array initial capacity
#define XR_ARRAY_INIT_CAPACITY 8

/* Element type enum, typed-storage ops, and the array field ABI are shared
 * across VM/AOT. Canonical definitions live in src/shared/. */
#include "../../shared/xr_elem_type.h"
#include "../../shared/xr_typed_ops.h"
#include "../../shared/xr_array_abi.h"

/* ====== Convenience Macros ====== */

#define XR_ARRAY_DATA_AS(arr, type) ((type *) ((arr)->data))
#define XR_ARRAY_IS_GC_TRACED(arr) ((arr)->elem_type == XR_ELEM_ANY)

/* Set monotonic flag when storing a GC pointer into an ANY array.
 * Once set, never cleared — GC must scan this array's elements. */
#define XR_ARRAY_MARK_GC_PTRS(arr, val)                                                            \
    do {                                                                                           \
        if (!(arr)->has_gc_ptrs && XR_VALUE_NEEDS_GC(val))                                         \
            (arr)->has_gc_ptrs = 1;                                                                \
    } while (0)

// Initialize array in-place (for system heap allocation)
XR_FUNC void xr_array_init_inplace(struct XrArray *arr, int capacity, uint8_t elem_type);

/*
 * Array object structure (slice-capable design)
 *
 * Slicing sets the data pointer offset directly (zero-copy). A slice is marked
 * by data_storage == XR_ARRAY_DATA_BORROWED (single discriminator); its
 * `source` retains the backing array for RC / cycle collection.
 *
 * Shared fields (data/length/capacity/source/data_storage/elem_*) come from
 * XR_ARRAY_ABI_FIELDS so the VM and AOT array layouts stay in lockstep.
 *
 * elem_type determines storage layout:
 *   XR_ELEM_ANY  → data is XrValue[], GC-traced
 *   XR_ELEM_I8   → data is int8_t[], no GC
 *   XR_ELEM_U8   → data is uint8_t[], no GC (replaces Bytes)
 *   XR_ELEM_I64  → data is int64_t[], no GC (Array<int>)
 *   XR_ELEM_F64  → data is double[], no GC (Array<float>)
 *   etc.
 */
struct XrArray {
    XrGCHeader gc;
    XR_ARRAY_ABI_FIELDS;
    uint8_t data_on_gc_heap;  // VM-only: 1 if data buffer is on Region GC heap (no free needed)
    uint8_t _pad[2];          // Alignment / reserved
};
typedef struct XrArray XrArray;

/* ====== Creation and Destruction ====== */

XR_FUNC XrArray *xr_array_new(struct XrCoroutine *coro);
XR_FUNC XrArray *xr_array_with_capacity(struct XrCoroutine *coro, int capacity);
/* Empty ANY array on the shared (system) heap, for cross-coroutine collection
 * points pushed into by children across workers (see definition). */
struct XrayIsolate;
XR_FUNC XrArray *xr_array_new_shared(struct XrayIsolate *X, int capacity);
XR_FUNC XrArray *xr_array_with_capacity_typed(struct XrCoroutine *coro, int capacity,
                                              XrArrayElemType elem_type);
XR_FUNC XrArray *xr_array_from_values(struct XrCoroutine *coro, XrValue *elements, int count);

/* ====== Element Access ====== */

XR_FUNC XrValue xr_array_get(XrArray *arr, int index);  // Returns null if out of bounds
XR_FUNC void xr_array_set(XrArray *arr, int index, XrValue value);

// Direct set without locking (for multi-threaded scenarios like await all)
XR_FUNC void xr_array_set_direct(XrArray *arr, int index, XrValue value);

XR_FUNC int xr_array_size(XrArray *arr);

/* ====== Array Modification ====== */

XR_FUNC void xr_array_push(XrArray *arr, XrValue value);
XR_FUNC XrValue xr_array_pop(XrArray *arr);
XR_FUNC void xr_array_unshift(XrArray *arr, XrValue value);
XR_FUNC XrValue xr_array_shift(XrArray *arr);
XR_FUNC void xr_array_clear(XrArray *arr);

/* ====== Query Methods ====== */

XR_FUNC int xr_array_index_of(XrArray *arr, XrValue value);
XR_FUNC bool xr_array_has(XrArray *arr, XrValue value);
XR_FUNC bool xr_array_is_empty(XrArray *arr);

/* ====== Higher-Order Methods ====== */

XR_FUNC void xr_array_foreach(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC XrArray *xr_array_map(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC XrArray *xr_array_filter(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC XrValue xr_array_reduce(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback,
                                XrValue initial);
XR_FUNC XrValue xr_array_find(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC int xr_array_find_index(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC bool xr_array_every(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC bool xr_array_some(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC void xr_array_fill(XrArray *arr, XrValue value, int start, int end);
XR_FUNC void xr_array_sort(XrayIsolate *iso, XrArray *arr, struct XrClosure *comparator);
XR_FUNC struct XrString *xr_array_join(XrayIsolate *iso, XrArray *arr, struct XrString *delimiter);
XR_FUNC bool xr_array_reserve(XrArray *arr, int32_t capacity);
XR_FUNC bool xr_array_resize(XrArray *arr, int32_t length, XrValue fill);

/* ====== Utility Methods ====== */

XR_FUNC void xr_array_reverse(XrArray *arr);
XR_FUNC XrArray *xr_array_copy(struct XrCoroutine *coro, XrArray *arr);
/* ====== Internal Functions ====== */

XR_FUNC void xr_array_grow(XrArray *arr);
XR_FUNC void xr_array_ensure_capacity(XrArray *arr, int min_capacity);

/* ====== Slice Operations (zero-copy) ====== */

XR_FUNC XrArray *xr_array_slice(struct XrCoroutine *coro, XrArray *arr, int32_t start, int32_t end);

static inline bool xr_array_is_slice(XrArray *arr) {
    return arr && arr->data_storage == XR_ARRAY_DATA_BORROWED;
}

XR_FUNC XrArray *xr_array_slice_to_array(struct XrCoroutine *coro, XrArray *slice);

/* ====== Type-Specialized Element Access ====== */

// Read element at index, returning XrValue (delegates to shared xr_typed_get)
static inline XrValue xr_array_get_element(XrArray *arr, int32_t index) {
    return xr_typed_get(arr->data, index, arr->elem_type);
}

// Write element at index from XrValue (delegates to shared xr_typed_set)
static inline void xr_array_set_element(XrArray *arr, int32_t index, XrValue value) {
    if (xr_typed_set(arr->data, index, value, arr->elem_type)) {
        XR_ARRAY_MARK_GC_PTRS(arr, value);
    }
}

/* ====== Raw Typed Access (fast path, no boxing) ====== */

static inline int64_t xr_array_get_i64(XrArray *arr, int32_t index) {
    return ((int64_t *) arr->data)[index];
}

static inline void xr_array_set_i64(XrArray *arr, int32_t index, int64_t value) {
    ((int64_t *) arr->data)[index] = value;
}

static inline double xr_array_get_f64(XrArray *arr, int32_t index) {
    return ((double *) arr->data)[index];
}

static inline void xr_array_set_f64(XrArray *arr, int32_t index, double value) {
    ((double *) arr->data)[index] = value;
}

/* ====== Bytes Convenience API (Array<uint8>) ====== */

static inline XrArray *xr_array_bytes_new(struct XrCoroutine *coro, int32_t size) {
    return xr_array_with_capacity_typed(coro, size, XR_ELEM_U8);
}

static inline uint8_t *xr_array_raw_u8(XrArray *arr) {
    return (uint8_t *) arr->data;
}

XR_FUNC void xr_array_append_data(XrArray *arr, const uint8_t *data, int32_t len);
XR_FUNC struct XrString *xr_array_to_string(struct XrayIsolate *iso, XrArray *arr);
XR_FUNC uint32_t xr_array_load_u32_le(XrArray *arr, int32_t offset, bool *ok);
XR_FUNC uint64_t xr_array_load_u64_le(XrArray *arr, int32_t offset, bool *ok);
XR_FUNC bool xr_array_bytes_copy_within(XrArray *arr, int32_t dst_offset, int32_t src_offset,
                                        int32_t count);
XR_FUNC bool xr_array_bytes_copy_from(XrArray *dst, XrArray *src, int32_t src_offset,
                                      int32_t dst_offset, int32_t count);
XR_FUNC bool xr_array_bytes_repeat_from(XrArray *arr, int32_t dst_offset, int32_t distance,
                                        int32_t count);

#endif  // XARRAY_H
