/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_elem_type.h - Shared typed-array element type enum and size table.
 *
 * Used by VM runtime (xarray.h) and AOT runtime (xrt_coll.h)
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
    XR_ELEM_U8,      /* uint8_t[] byte-array lane */
    XR_ELEM_I16,     /* int16_t[] */
    XR_ELEM_U16,     /* uint16_t[] */
    XR_ELEM_I32,     /* int32_t[] */
    XR_ELEM_U32,     /* uint32_t[] */
    XR_ELEM_I64,     /* int64_t[] (Array<int>) */
    XR_ELEM_U64,     /* uint64_t[] */
    XR_ELEM_F32,     /* float[]   */
    XR_ELEM_F64,     /* double[]  (Array<float>) */
    XR_ELEM_BOOL,    /* uint8_t[] (1 byte per element) */
    XR_ELEM_RUNE,    /* uint32_t[] (Unicode scalar, Array<char>) */
    XR_ELEM_RAWPTR,  /* void*[] raw C pointer / CFn function pointer (8 bytes, GC-invisible; AOT) */
    XR_ELEM_COUNT
} XrArrayElemType;

/* Byte width per element for each storage type.
 * Index with XrArrayElemType. XR_ELEM_ANY uses sizeof(XrValue) = 16. */
static const uint8_t XR_ELEM_SIZES[XR_ELEM_COUNT] = {
    16,    /* ANY (XrValue = 16-byte tagged union) */
    1,  1, /* I8, U8 */
    2,  2, /* I16, U16 */
    4,  4, /* I32, U32 */
    8,  8, /* I64, U64 */
    4,     /* F32 */
    8,     /* F64 */
    1,     /* BOOL */
    4,     /* CHAR */
    8      /* RAWPTR (void* address) */
};

/* Map semantic XrTypeId to storage layout.
 *
 * The ids are literal constants so this header stays dependency-free: every
 * profile that maps element storage can include it without dragging in the
 * public type vocabulary.
 *
 * The copy is pinned from the other side: xr_type_names_core.h generates a
 * _Static_assert for every id from xr_type_names.def. Without that, deleting
 * two TIDs
 * shifted XR_TID_RUNE from 45 to 43, the hand-copied `case 45` stopped
 * matching, and every Slice<rune> silently became XR_ELEM_ANY — Slice.fill
 * then rejected its own receiver as "not POD" at runtime, with nothing failing
 * at build time. Renumbering is now a compile error. */
static inline XrArrayElemType xr_tid_to_elem_type(uint8_t tid) {
    switch (tid) {
        case 8:
            return XR_ELEM_I64; /* XR_TID_I64 */
        case 11:
            return XR_ELEM_F64; /* XR_TID_F64 */
        case 1:
            return XR_ELEM_BOOL; /* XR_TID_BOOL */
        case 2:
            return XR_ELEM_I8; /* XR_TID_I8 */
        case 3:
            return XR_ELEM_U8; /* XR_TID_U8 */
        case 4:
            return XR_ELEM_I16; /* XR_TID_I16 */
        case 5:
            return XR_ELEM_U16; /* XR_TID_U16 */
        case 6:
            return XR_ELEM_I32; /* XR_TID_I32 */
        case 7:
            return XR_ELEM_U32; /* XR_TID_U32 */
        case 9:
            return XR_ELEM_U64; /* XR_TID_U64 */
        case 10:
            return XR_ELEM_F32; /* XR_TID_F32 */
        case 45:
            return XR_ELEM_I64; /* XR_TID_ISIZE on the current 64-bit targets */
        case 46:
            return XR_ELEM_U64; /* XR_TID_USIZE on the current 64-bit targets */
        case 43:
            return XR_ELEM_RUNE; /* XR_TID_RUNE */
        default:
            return XR_ELEM_ANY; /* string, object, etc. */
    }
}

/* Convenience: element size from tid */
static inline uint8_t xr_tid_to_elem_size(uint8_t tid) {
    return XR_ELEM_SIZES[xr_tid_to_elem_type(tid)];
}

/* Map native fixed-array lane tags to Slice/Array element storage tags.
 * Non-scalar lanes use XR_ELEM_ANY because their storage is full XrValue slots. */
static inline XrArrayElemType xr_native_type_to_elem_type(uint8_t native_type) {
    switch (native_type) {
        case 3: /* XR_NATIVE_I8 */
            return XR_ELEM_I8;
        case 6: /* XR_NATIVE_U8 */
            return XR_ELEM_U8;
        case 4: /* XR_NATIVE_I16 */
            return XR_ELEM_I16;
        case 7: /* XR_NATIVE_U16 */
            return XR_ELEM_U16;
        case 5: /* XR_NATIVE_I32 */
            return XR_ELEM_I32;
        case 8: /* XR_NATIVE_U32 */
            return XR_ELEM_U32;
        case 0: /* XR_NATIVE_I64 */
            return XR_ELEM_I64;
        case 9: /* XR_NATIVE_U64 */
            return XR_ELEM_U64;
        case 10: /* XR_NATIVE_F32 */
            return XR_ELEM_F32;
        case 1: /* XR_NATIVE_F64 */
            return XR_ELEM_F64;
        case 2: /* XR_NATIVE_BOOL */
            return XR_ELEM_BOOL;
        default:
            return XR_ELEM_ANY;
    }
}

#endif  // XR_ELEM_TYPE_H
