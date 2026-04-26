/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstruct_layout.c - Struct layout computation
 */

#include "xstruct_layout.h"
#include "xtype.h"

int xr_type_kind_to_native(int kind, uint8_t native_width) {
    switch ((XrTypeKind)kind) {
        case XR_KIND_BOOL:
            return XR_NATIVE_BOOL;
        case XR_KIND_INT:
            // native_width carries the explicit XrNativeType for sized
            // integers (i8/i16/i32/u8/...). Zero means "default int" (i64).
            if (native_width != 0) return (int)native_width;
            return XR_NATIVE_I64;
        case XR_KIND_FLOAT:
            if (native_width == XR_NATIVE_F32) return XR_NATIVE_F32;
            return XR_NATIVE_F64;
        case XR_KIND_STRING:
            return XR_NATIVE_STRING;
        default:
            return -1;
    }
}

void xr_struct_layout_compute(XrStructLayout *layout) {
    if (!layout || layout->field_count == 0) {
        if (layout) {
            layout->total_size = 0;
            layout->alignment = 1;
        }
        return;
    }

    uint16_t offset = 0;
    uint16_t max_align = 1;

    for (int i = 0; i < layout->field_count; i++) {
        XrStructFieldLayout *f = &layout->fields[i];

        // Auto-compute size from native_type (except nested struct and array)
        if (f->native_type == XR_NATIVE_ARRAY) {
            // Fixed-size array: size = elem_count * elem_size
            uint8_t es = xr_native_type_size(f->elem_native_type);
            f->size = (uint16_t)(f->elem_count * es);
        } else if (f->native_type != XR_NATIVE_STRUCT) {
            f->size = xr_native_type_size(f->native_type);
        }

        uint8_t field_align;
        if (f->native_type == XR_NATIVE_STRUCT) {
            field_align = 8; // nested structs: 8-byte aligned
        } else if (f->native_type == XR_NATIVE_ARRAY) {
            field_align = xr_native_type_align(f->elem_native_type);
        } else {
            field_align = xr_native_type_align(f->native_type);
        }

        // Align offset to field alignment
        offset = (offset + field_align - 1) & ~(field_align - 1);

        f->offset = offset;
        offset += f->size;

        if (field_align > max_align) {
            max_align = field_align;
        }
    }

    // Pad total size to alignment
    layout->total_size = (offset + max_align - 1) & ~(max_align - 1);
    layout->alignment = max_align;
}
