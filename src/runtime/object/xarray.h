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

/* ====== Element Type for Type-Specialized Storage ====== */

typedef enum {
    XR_ELEM_ANY = 0,    // XrValue[] — GC-traced, default
    XR_ELEM_I8,         // int8_t[]
    XR_ELEM_U8,         // uint8_t[]    (replaces Bytes)
    XR_ELEM_I16,        // int16_t[]
    XR_ELEM_U16,        // uint16_t[]
    XR_ELEM_I32,        // int32_t[]
    XR_ELEM_U32,        // uint32_t[]
    XR_ELEM_I64,        // int64_t[]    (Array<int>)
    XR_ELEM_U64,        // uint64_t[]
    XR_ELEM_F32,        // float[]
    XR_ELEM_F64,        // double[]     (Array<float>)
    XR_ELEM_BOOL,       // uint8_t[]    (Array<bool>, 1 byte per element)
    XR_ELEM_COUNT
} XrArrayElemType;

static const uint8_t XR_ELEM_SIZES[XR_ELEM_COUNT] = {
    16,             // ANY (XrValue = 16-byte tagged union)
    1, 1,           // I8, U8
    2, 2,           // I16, U16
    4, 4,           // I32, U32
    8, 8,           // I64, U64
    4, 8,           // F32, F64
    1,              // BOOL
};

/* ====== XrTypeId → XrArrayElemType Mapping ====== */

// Derive storage layout from semantic type ID (for reified generics)
static inline XrArrayElemType xr_tid_to_elem_type(uint8_t tid) {
    switch (tid) {
        case 8:  return XR_ELEM_I64;   // XR_TID_INT
        case 11: return XR_ELEM_F64;   // XR_TID_FLOAT
        case 1:  return XR_ELEM_BOOL;  // XR_TID_BOOL
        case 2:  return XR_ELEM_I8;    // XR_TID_INT8
        case 3:  return XR_ELEM_U8;    // XR_TID_UINT8
        case 4:  return XR_ELEM_I16;   // XR_TID_INT16
        case 5:  return XR_ELEM_U16;   // XR_TID_UINT16
        case 6:  return XR_ELEM_I32;   // XR_TID_INT32
        case 7:  return XR_ELEM_U32;   // XR_TID_UINT32
        case 9:  return XR_ELEM_U64;   // XR_TID_UINT64
        case 10: return XR_ELEM_F32;   // XR_TID_FLOAT32
        default: return XR_ELEM_ANY;   // string, object, etc. → generic storage
    }
}

/* ====== Convenience Macros ====== */

#define XR_ARRAY_DATA_AS(arr, type) ((type*)((arr)->data))
#define XR_ARRAY_IS_GC_TRACED(arr)  ((arr)->elem_type == XR_ELEM_ANY)

/* Set monotonic flag when storing a GC pointer into an ANY array.
 * Once set, never cleared — GC must scan this array's elements. */
#define XR_ARRAY_MARK_GC_PTRS(arr, val) do { \
    if (!(arr)->has_gc_ptrs && XR_VALUE_NEEDS_GC(val)) \
        (arr)->has_gc_ptrs = 1; \
} while(0)

// Initialize array in-place (for system heap allocation)
XR_FUNC void xr_array_init_inplace(struct XrArray *arr, int capacity, uint8_t elem_type);

/*
 * Array object structure (slice-capable design)
 *
 * Array and ArraySlice share the same structure.
 * Slicing sets data pointer offset directly (zero-copy).
 * capacity == 0 marks a slice (cannot resize).
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
    void *data;                 // Generic data pointer (type depends on elem_type)
    int32_t length;
    int32_t capacity;           // 0 for slices (no resize)
    struct XrArray *source;     // Source array for slices (GC tracking)
    uint8_t elem_type;          // XrArrayElemType (storage layout)
    uint8_t elem_size;          // Cached: bytes per element
    uint8_t elem_tid;           // XrTypeId: semantic type for reified generics (0=any)
    uint8_t has_gc_ptrs;        // Monotonic flag: 1 if any GC pointer was ever stored
    uint8_t data_on_gc_heap;    // 1 if data buffer is on Immix GC heap (no free needed)
    uint8_t _pad[3];            // Alignment / reserved
};
typedef struct XrArray XrArray;

// ArraySlice is an alias for XrArray
typedef XrArray XrArraySlice;

/* ====== Creation and Destruction ====== */

XR_FUNC XrArray* xr_array_new(struct XrCoroutine *coro);
XR_FUNC XrArray* xr_array_with_capacity(struct XrCoroutine *coro, int capacity);
XR_FUNC XrArray* xr_array_with_capacity_typed(struct XrCoroutine *coro, int capacity, XrArrayElemType elem_type);
XR_FUNC XrArray* xr_array_from_values(struct XrCoroutine *coro, XrValue *elements, int count);

/* ====== Element Access ====== */

XR_FUNC XrValue xr_array_get(XrArray *arr, int index);  // Returns null if out of bounds
XR_FUNC void xr_array_set(XrArray *arr, int index, XrValue value);

// Direct set without locking (for multi-threaded scenarios like await.all)
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
XR_FUNC XrArray* xr_array_map(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC XrArray* xr_array_filter(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC XrValue xr_array_reduce(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback, XrValue initial);
XR_FUNC XrValue xr_array_find(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC int xr_array_find_index(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC bool xr_array_every(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC bool xr_array_some(XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC void xr_array_fill(XrArray *arr, XrValue value, int start, int end);
XR_FUNC void xr_array_sort(XrayIsolate *iso, XrArray *arr, struct XrClosure *comparator);
XR_FUNC struct XrString* xr_array_join(XrayIsolate *iso, XrArray *arr, struct XrString *delimiter);

/* ====== Utility Methods ====== */

XR_FUNC void xr_array_reverse(XrArray *arr);
XR_FUNC XrArray* xr_array_copy(struct XrCoroutine *coro, XrArray *arr);
/* ====== Internal Functions ====== */

XR_FUNC void xr_array_grow(XrArray *arr);
XR_FUNC void xr_array_ensure_capacity(XrArray *arr, int min_capacity);

/* ====== Slice Operations (zero-copy) ====== */

XR_FUNC XrArray* xr_array_slice(struct XrCoroutine *coro, XrArray *arr, int32_t start, int32_t end);

static inline bool xr_array_is_slice(XrArray *arr) {
    return arr && arr->capacity == 0 && arr->source != NULL;
}

XR_FUNC XrArray* xr_array_slice_to_array(struct XrCoroutine *coro, XrArray *slice);

/* ====== Type-Specialized Element Access ====== */

// Read element at index, returning XrValue (auto-boxes typed elements)
static inline XrValue xr_array_get_element(XrArray *arr, int32_t index) {
    switch (arr->elem_type) {
        case XR_ELEM_ANY:  return ((XrValue*)arr->data)[index];
        case XR_ELEM_I8:   return xr_int(((int8_t*)arr->data)[index]);
        case XR_ELEM_U8:   return xr_int(((uint8_t*)arr->data)[index]);
        case XR_ELEM_I16:  return xr_int(((int16_t*)arr->data)[index]);
        case XR_ELEM_U16:  return xr_int(((uint16_t*)arr->data)[index]);
        case XR_ELEM_I32:  return xr_int(((int32_t*)arr->data)[index]);
        case XR_ELEM_U32:  return xr_int(((uint32_t*)arr->data)[index]);
        case XR_ELEM_I64:  return xr_int(((int64_t*)arr->data)[index]);
        case XR_ELEM_U64:  return xr_int((int64_t)((uint64_t*)arr->data)[index]);
        case XR_ELEM_F32:  return XR_FROM_FLOAT((double)((float*)arr->data)[index]);
        case XR_ELEM_F64:  return XR_FROM_FLOAT(((double*)arr->data)[index]);
        case XR_ELEM_BOOL: return ((uint8_t*)arr->data)[index] ? XR_TRUE_VAL : XR_FALSE_VAL;
        default:           return xr_null();
    }
}

// Write element at index from XrValue (auto-unboxes for typed arrays)
static inline void xr_array_set_element(XrArray *arr, int32_t index, XrValue value) {
    switch (arr->elem_type) {
        case XR_ELEM_ANY:
            ((XrValue*)arr->data)[index] = value;
            XR_ARRAY_MARK_GC_PTRS(arr, value);
            break;
        case XR_ELEM_I8:
            ((int8_t*)arr->data)[index] = (int8_t)xr_value_to_int64_coerce(value);
            break;
        case XR_ELEM_U8:
            ((uint8_t*)arr->data)[index] = (uint8_t)xr_value_to_int64_coerce(value);
            break;
        case XR_ELEM_I16:
            ((int16_t*)arr->data)[index] = (int16_t)xr_value_to_int64_coerce(value);
            break;
        case XR_ELEM_U16:
            ((uint16_t*)arr->data)[index] = (uint16_t)xr_value_to_int64_coerce(value);
            break;
        case XR_ELEM_I32:
            ((int32_t*)arr->data)[index] = (int32_t)xr_value_to_int64_coerce(value);
            break;
        case XR_ELEM_U32:
            ((uint32_t*)arr->data)[index] = (uint32_t)xr_value_to_int64_coerce(value);
            break;
        case XR_ELEM_I64:
            ((int64_t*)arr->data)[index] = xr_value_to_int64_coerce(value);
            break;
        case XR_ELEM_U64:
            ((uint64_t*)arr->data)[index] = (uint64_t)xr_value_to_int64_coerce(value);
            break;
        case XR_ELEM_F32:
            ((float*)arr->data)[index] = (float)xr_value_to_f64_coerce(value);
            break;
        case XR_ELEM_F64:
            ((double*)arr->data)[index] = xr_value_to_f64_coerce(value);
            break;
        case XR_ELEM_BOOL: {
            bool falsy = XR_IS_FALSE(value) || XR_IS_NULL(value)
                      || (XR_IS_INT(value) && XR_TO_INT(value) == 0)
                      || (XR_IS_FLOAT(value) && XR_TO_FLOAT(value) == 0.0);
            ((uint8_t*)arr->data)[index] = falsy ? 0 : 1;
            break;
        }
        default:
            break;
    }
}

/* ====== Raw Typed Access (fast path, no boxing) ====== */

static inline int64_t xr_array_get_i64(XrArray *arr, int32_t index) {
    return ((int64_t*)arr->data)[index];
}

static inline void xr_array_set_i64(XrArray *arr, int32_t index, int64_t value) {
    ((int64_t*)arr->data)[index] = value;
}

static inline double xr_array_get_f64(XrArray *arr, int32_t index) {
    return ((double*)arr->data)[index];
}

static inline void xr_array_set_f64(XrArray *arr, int32_t index, double value) {
    ((double*)arr->data)[index] = value;
}

/* ====== Bytes Convenience API (Array<uint8>) ====== */

static inline XrArray* xr_array_bytes_new(struct XrCoroutine *coro, int32_t size) {
    return xr_array_with_capacity_typed(coro, size, XR_ELEM_U8);
}

static inline uint8_t* xr_array_raw_u8(XrArray *arr) {
    return (uint8_t*)arr->data;
}

XR_FUNC void xr_array_append_data(XrArray *arr, const uint8_t *data, int32_t len);
XR_FUNC struct XrString* xr_array_to_string(struct XrayIsolate *iso, XrArray *arr);

#endif // XARRAY_H
