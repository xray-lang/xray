/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_elem_type.h - Shared typed-array element type enum and size table.
 *
 * Used by VM runtime (xarray.h), JIT runtime, and AOT runtime (xrt_coll.h)
 * to agree on storage layout for typed arrays.
 *
 * Dependency: <stdint.h> only. Caller provides XrValue definition.
 */

#ifndef XR_ELEM_TYPE_H
#define XR_ELEM_TYPE_H

#include <stdint.h>

/* Element storage type for type-specialized arrays.
 * Determines the C type used for the backing buffer. */
typedef enum {
    XR_ELEM_ANY = 0, /* XrValue[] — GC-traced, default */
    XR_ELEM_I8,      /* int8_t[]  */
    XR_ELEM_U8,      /* uint8_t[] (replaces Bytes) */
    XR_ELEM_I16,     /* int16_t[] */
    XR_ELEM_U16,     /* uint16_t[] */
    XR_ELEM_I32,     /* int32_t[] */
    XR_ELEM_U32,     /* uint32_t[] */
    XR_ELEM_I64,     /* int64_t[] (Array<int>) */
    XR_ELEM_U64,     /* uint64_t[] */
    XR_ELEM_F32,     /* float[]   */
    XR_ELEM_F64,     /* double[]  (Array<float>) */
    XR_ELEM_BOOL,    /* uint8_t[] (1 byte per element) */
    XR_ELEM_COUNT
} XrArrayElemType;

/* Bytes per element for each storage type.
 * Index with XrArrayElemType. XR_ELEM_ANY uses sizeof(XrValue) = 16. */
static const uint8_t XR_ELEM_SIZES[XR_ELEM_COUNT] = {
    16,    /* ANY (XrValue = 16-byte tagged union) */
    1,  1, /* I8, U8 */
    2,  2, /* I16, U16 */
    4,  4, /* I32, U32 */
    8,  8, /* I64, U64 */
    4,     /* F32 */
    8,     /* F64 */
    1      /* BOOL */
};

/* Map semantic XrTypeId to storage layout.
 * XrTypeId values are defined in xtype_ids.h; we use literal constants
 * here to keep this header dependency-free. */
static inline XrArrayElemType xr_tid_to_elem_type(uint8_t tid) {
    switch (tid) {
        case 8:
            return XR_ELEM_I64; /* XR_TID_INT */
        case 11:
            return XR_ELEM_F64; /* XR_TID_FLOAT */
        case 1:
            return XR_ELEM_BOOL; /* XR_TID_BOOL */
        case 2:
            return XR_ELEM_I8; /* XR_TID_INT8 */
        case 3:
            return XR_ELEM_U8; /* XR_TID_UINT8 */
        case 4:
            return XR_ELEM_I16; /* XR_TID_INT16 */
        case 5:
            return XR_ELEM_U16; /* XR_TID_UINT16 */
        case 6:
            return XR_ELEM_I32; /* XR_TID_INT32 */
        case 7:
            return XR_ELEM_U32; /* XR_TID_UINT32 */
        case 9:
            return XR_ELEM_U64; /* XR_TID_UINT64 */
        case 10:
            return XR_ELEM_F32; /* XR_TID_FLOAT32 */
        default:
            return XR_ELEM_ANY; /* string, object, etc. */
    }
}

/* Convenience: element size from tid */
static inline uint8_t xr_tid_to_elem_size(uint8_t tid) {
    return XR_ELEM_SIZES[xr_tid_to_elem_type(tid)];
}

#endif  // XR_ELEM_TYPE_H
