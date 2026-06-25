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

#include <stdint.h>

// Native type tags for value struct fields and aggregate runtime layouts.
typedef enum {
    XR_NATIVE_I64 = 0,         // int64_t (8 bytes)
    XR_NATIVE_F64 = 1,         // double (8 bytes)
    XR_NATIVE_BOOL = 2,        // uint8_t (1 byte, padded to alignment)
    XR_NATIVE_I8 = 3,          // int8_t (1 byte)
    XR_NATIVE_I16 = 4,         // int16_t (2 bytes)
    XR_NATIVE_I32 = 5,         // int32_t (4 bytes)
    XR_NATIVE_U8 = 6,          // uint8_t (1 byte)
    XR_NATIVE_U16 = 7,         // uint16_t (2 bytes)
    XR_NATIVE_U32 = 8,         // uint32_t (4 bytes)
    XR_NATIVE_U64 = 9,         // uint64_t (8 bytes)
    XR_NATIVE_F32 = 10,        // float (4 bytes)
    XR_NATIVE_STRUCT = 11,     // nested struct (variable size)
    XR_NATIVE_ARRAY = 12,      // fixed-size inline array [N]T
    XR_NATIVE_STRING = 13,     // string pointer (8 bytes)
    XR_NATIVE_ARRAY_REF = 14,  // tagged XrValue-width aggregate ref slot
    XR_NATIVE_MAP_REF = 15,    // tagged XrValue-width aggregate ref slot
    XR_NATIVE_SET_REF = 16,    // tagged XrValue-width aggregate ref slot
} XrNativeType;

static inline uint8_t xr_native_type_size(uint8_t native_type) {
    switch (native_type) {
        case XR_NATIVE_I64:
        case XR_NATIVE_U64:
        case XR_NATIVE_F64:
        case XR_NATIVE_STRING:
            return 8;
        case XR_NATIVE_ARRAY_REF:
        case XR_NATIVE_MAP_REF:
        case XR_NATIVE_SET_REF:
            return 16;
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
        case XR_NATIVE_F32:
            return 4;
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
            return 2;
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
        case XR_NATIVE_BOOL:
            return 1;
        case XR_NATIVE_ARRAY:
            return 0;
        default:
            return 8;
    }
}

static inline uint8_t xr_native_type_align(uint8_t native_type) {
    switch (native_type) {
        case XR_NATIVE_ARRAY_REF:
        case XR_NATIVE_MAP_REF:
        case XR_NATIVE_SET_REF:
            return 8;
        default:
            return xr_native_type_size(native_type);
    }
}

#endif /* XR_NATIVE_TYPE_CORE_H */
