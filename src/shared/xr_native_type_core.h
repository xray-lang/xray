/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_native_type_core.h - Runtime-neutral native field layout rules.
 */

#ifndef XR_NATIVE_TYPE_CORE_H
#define XR_NATIVE_TYPE_CORE_H

#include "../base/xtarget_data_layout.h"
#include <stddef.h>
#include <stdint.h>

// Native type tags for value struct fields and aggregate runtime layouts.
typedef enum {
    XR_NATIVE_I64 = 0,                // int64_t (8 bytes)
    XR_NATIVE_F64 = 1,                // double (8 bytes)
    XR_NATIVE_BOOL = 2,               // uint8_t (1 byte, padded to alignment)
    XR_NATIVE_I8 = 3,                 // int8_t (1 byte)
    XR_NATIVE_I16 = 4,                // int16_t (2 bytes)
    XR_NATIVE_I32 = 5,                // int32_t (4 bytes)
    XR_NATIVE_U8 = 6,                 // uint8_t (1 byte)
    XR_NATIVE_U16 = 7,                // uint16_t (2 bytes)
    XR_NATIVE_U32 = 8,                // uint32_t (4 bytes)
    XR_NATIVE_U64 = 9,                // uint64_t (8 bytes)
    XR_NATIVE_F32 = 10,               // float (4 bytes)
    XR_NATIVE_NESTED_AGGREGATE = 11,  // nested aggregate layout (variable size)
    XR_NATIVE_ARRAY = 12,             // fixed-size inline array [T; N]
    XR_NATIVE_STRING = 13,            // string pointer (8 bytes)
    XR_NATIVE_ARRAY_REF = 14,         // tagged XrValue-width aggregate ref slot
    XR_NATIVE_MAP_REF = 15,           // tagged XrValue-width aggregate ref slot
    XR_NATIVE_SET_REF = 16,           // tagged XrValue-width aggregate ref slot
    XR_NATIVE_VALUE = 17,             // full tagged XrValue lane
    XR_NATIVE_ISIZE = 18,             // ptrdiff_t (target pointer-width signed int)
    XR_NATIVE_USIZE = 19,             // size_t (target pointer-width unsigned int)
    XR_NATIVE_POINTER = 20,           // raw C pointer (void *, target pointer width)
} XrNativeType;

static inline uint8_t xr_native_type_size(const XrTargetDataLayout *layout, uint8_t native_type) {
    if (!layout)
        return 0;
    switch (native_type) {
        case XR_NATIVE_I64:
            return (uint8_t) layout->i64.size;
        case XR_NATIVE_U64:
            return (uint8_t) layout->u64.size;
        case XR_NATIVE_F64:
            return (uint8_t) layout->f64.size;
        case XR_NATIVE_STRING:
        case XR_NATIVE_POINTER:
            return (uint8_t) layout->pointer.size;
        case XR_NATIVE_ISIZE:
            return (uint8_t) layout->isize.size;
        case XR_NATIVE_USIZE:
            return (uint8_t) layout->usize.size;
        case XR_NATIVE_ARRAY_REF:
        case XR_NATIVE_MAP_REF:
        case XR_NATIVE_SET_REF:
        case XR_NATIVE_VALUE:
            return (uint8_t) layout->xr_value.size;
        case XR_NATIVE_I32:
            return (uint8_t) layout->i32.size;
        case XR_NATIVE_U32:
            return (uint8_t) layout->u32.size;
        case XR_NATIVE_F32:
            return (uint8_t) layout->f32.size;
        case XR_NATIVE_I16:
            return (uint8_t) layout->i16.size;
        case XR_NATIVE_U16:
            return (uint8_t) layout->u16.size;
        case XR_NATIVE_I8:
            return (uint8_t) layout->i8.size;
        case XR_NATIVE_U8:
            return (uint8_t) layout->u8.size;
        case XR_NATIVE_BOOL:
            return (uint8_t) layout->boolean.size;
        case XR_NATIVE_NESTED_AGGREGATE:
        case XR_NATIVE_ARRAY:
            return 0;
        default:
            return 0;
    }
}

static inline uint8_t xr_native_type_align(const XrTargetDataLayout *layout, uint8_t native_type) {
    if (!layout)
        return 0;
    switch (native_type) {
        case XR_NATIVE_ARRAY_REF:
        case XR_NATIVE_MAP_REF:
        case XR_NATIVE_SET_REF:
        case XR_NATIVE_VALUE:
            return (uint8_t) layout->xr_value.align;
        case XR_NATIVE_I64:
            return (uint8_t) layout->i64.align;
        case XR_NATIVE_U64:
            return (uint8_t) layout->u64.align;
        case XR_NATIVE_F64:
            return (uint8_t) layout->f64.align;
        case XR_NATIVE_STRING:
        case XR_NATIVE_POINTER:
            return (uint8_t) layout->pointer.align;
        case XR_NATIVE_ISIZE:
            return (uint8_t) layout->isize.align;
        case XR_NATIVE_USIZE:
            return (uint8_t) layout->usize.align;
        case XR_NATIVE_I32:
            return (uint8_t) layout->i32.align;
        case XR_NATIVE_U32:
            return (uint8_t) layout->u32.align;
        case XR_NATIVE_F32:
            return (uint8_t) layout->f32.align;
        case XR_NATIVE_I16:
            return (uint8_t) layout->i16.align;
        case XR_NATIVE_U16:
            return (uint8_t) layout->u16.align;
        case XR_NATIVE_I8:
            return (uint8_t) layout->i8.align;
        case XR_NATIVE_U8:
            return (uint8_t) layout->u8.align;
        case XR_NATIVE_BOOL:
            return (uint8_t) layout->boolean.align;
        case XR_NATIVE_NESTED_AGGREGATE:
        case XR_NATIVE_ARRAY:
        default:
            return 0;
    }
}

#endif /* XR_NATIVE_TYPE_CORE_H */
