/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstruct_layout.h - Compile-time struct layout descriptor
 *
 * KEY CONCEPT:
 *   Each struct type has a fixed layout computed at compile time.
 *   Fields are stored in native format (double/int64_t/bool) without
 *   XrValue boxing. The layout drives stack-frame struct_area allocation
 *   and OP_STRUCT_GET/SET field access.
 *
 * MEMORY LAYOUT (example Vec3{x:float, y:float, z:float}):
 *
 *   struct_area + offset
 *   +----------+----------+----------+
 *   | x: f64   | y: f64   | z: f64   |
 *   | 8 bytes  | 8 bytes  | 8 bytes  |
 *   +----------+----------+----------+
 *   offset: 0        8        16
 *   total_size = 24, alignment = 8
 *
 * WHY THIS DESIGN:
 *   - Zero GC overhead: structs live on the stack frame
 *   - Native field access: base_ptr + compile-time offset
 *   - Minimal copy cost: memcpy(total_size) for value semantics
 *   - JIT/AOT friendly: direct C struct mapping
 */

#ifndef XSTRUCT_LAYOUT_H
#define XSTRUCT_LAYOUT_H

#include <stdint.h>
#include <stdbool.h>
#include "../../base/xdefs.h"

// Native type tags for struct fields
typedef enum {
    XR_NATIVE_I64 = 0,      // int64_t (8 bytes)
    XR_NATIVE_F64 = 1,      // double (8 bytes)
    XR_NATIVE_BOOL = 2,     // uint8_t (1 byte, padded to alignment)
    XR_NATIVE_I8 = 3,       // int8_t (1 byte)
    XR_NATIVE_I16 = 4,      // int16_t (2 bytes)
    XR_NATIVE_I32 = 5,      // int32_t (4 bytes)
    XR_NATIVE_U8 = 6,       // uint8_t (1 byte)
    XR_NATIVE_U16 = 7,      // uint16_t (2 bytes)
    XR_NATIVE_U32 = 8,      // uint32_t (4 bytes)
    XR_NATIVE_U64 = 9,      // uint64_t (8 bytes)
    XR_NATIVE_F32 = 10,     // float (4 bytes)
    XR_NATIVE_STRUCT = 11,  // nested struct (variable size)
    XR_NATIVE_ARRAY = 12,   // fixed-size inline array [N]T
    XR_NATIVE_STRING = 13,  // XrString* pointer (8 bytes, immutable, GC-traced)
} XrNativeType;

// Per-field descriptor within a struct layout
typedef struct {
    uint16_t offset;           // byte offset within struct
    uint8_t native_type;       // XrNativeType
    uint16_t size;             // field size in bytes
    uint16_t sub_layout_id;    // layout_id for nested struct (XR_NATIVE_STRUCT only)
    uint8_t elem_native_type;  // element type for XR_NATIVE_ARRAY
    uint16_t elem_count;       // element count for XR_NATIVE_ARRAY
} XrStructFieldLayout;

#define XR_MAX_STRUCT_FIELDS 64

/*
 * XrStructLayout - Fixed layout for a struct type
 *
 * Allocated once at compile time, shared by all instances of the type.
 * Referenced by XrClass.struct_layout for VALUE_TYPE classes.
 */
typedef struct XrStructLayout {
    uint16_t total_size;   // total struct size in bytes (aligned)
    uint16_t alignment;    // alignment requirement
    uint16_t field_count;  // number of fields
    uint16_t layout_id;    // global layout registry index
    const char **field_names;  // [field_count] parallel to fields[], NULL-able
    XrStructFieldLayout fields[XR_MAX_STRUCT_FIELDS];
} XrStructLayout;

/* ========== Layout Computation ========== */

// Return native size for a given XrNativeType
static inline uint8_t xr_native_type_size(uint8_t native_type) {
    switch (native_type) {
        case XR_NATIVE_I64:
        case XR_NATIVE_U64:
        case XR_NATIVE_F64:
        case XR_NATIVE_STRING:
            return 8;
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
            return 0;  // variable: elem_count * elem_size
        default:
            return 8;
    }
}

// Return natural alignment for a given XrNativeType
static inline uint8_t xr_native_type_align(uint8_t native_type) {
    return xr_native_type_size(native_type);
}

/* Initialize a layout and compute field offsets + total size.
 * Caller must have set fields[i].native_type and fields[i].size for
 * nested structs before calling. For non-STRUCT fields, size is
 * auto-computed from native_type. */
XR_FUNC void xr_struct_layout_compute(XrStructLayout *layout);

/*
 * Convert XrTypeKind + native_width to XrNativeType for struct fields.
 * native_width stores XrNativeType directly (0 = use default for kind).
 * Returns -1 if the type is not valid for struct fields.
 *
 * Implemented in xstruct_layout.c to avoid pulling xtype.h into this
 * header (xtype.h already includes xstruct_layout.h, so a back-reference
 * here would form an include cycle).
 */
XR_FUNC int xr_type_kind_to_native(int kind, uint8_t native_width);

#endif  // XSTRUCT_LAYOUT_H
