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
#include "../class/xclass_info.h"

#include <stddef.h>
#include <string.h>

int xr_type_kind_to_native(int kind, uint8_t native_width) {
    switch ((XrTypeKind) kind) {
        case XR_KIND_BOOL:
            return XR_NATIVE_BOOL;
        case XR_KIND_INT:
            // native_width carries the explicit XrNativeType for sized
            // integers (i8/i16/i32/u8/...). Zero means "default int" (i64).
            if (native_width != 0)
                return (int) native_width;
            return XR_NATIVE_I64;
        case XR_KIND_FLOAT:
            if (native_width == XR_NATIVE_F32)
                return XR_NATIVE_F32;
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

    uint32_t offset = 0;
    uint32_t max_align = 1;
    uint32_t max_size = 0;

    for (int i = 0; i < layout->field_count; i++) {
        XrStructFieldLayout *f = &layout->fields[i];

        // Auto-compute size from native_type (except nested struct and array)
        if (f->native_type == XR_NATIVE_ARRAY) {
            // Fixed-size array: size = elem_count * elem_size
            uint8_t es = xr_native_type_size(f->elem_native_type);
            f->size = (uint16_t) (f->elem_count * es);
        } else if (f->native_type != XR_NATIVE_STRUCT) {
            f->size = xr_native_type_size(f->native_type);
        }

        uint32_t field_align;
        if (layout->kind == XR_STRUCT_LAYOUT_PACKED) {
            field_align = 1;
        } else if (f->native_type == XR_NATIVE_STRUCT) {
            field_align = f->sub_layout ? f->sub_layout->alignment : 8;
        } else if (f->native_type == XR_NATIVE_ARRAY) {
            field_align = xr_native_type_align(f->elem_native_type);
        } else {
            field_align = xr_native_type_align(f->native_type);
        }

        if (layout->kind == XR_STRUCT_LAYOUT_UNION) {
            f->offset = 0;
            if (f->size > max_size)
                max_size = f->size;
            if (field_align > max_align)
                max_align = field_align;
            continue;
        }

        // Align offset to field alignment
        offset = (offset + field_align - 1) & ~(field_align - 1);

        f->offset = (uint16_t) offset;
        offset += f->size;

        if (field_align > max_align) {
            max_align = field_align;
        }
    }

    if (layout->explicit_align > max_align) {
        max_align = layout->explicit_align;
    }

    // Pad total size to alignment
    uint32_t raw_size = (layout->kind == XR_STRUCT_LAYOUT_UNION) ? max_size : offset;
    layout->total_size = (uint16_t) ((raw_size + max_align - 1) & ~(max_align - 1));
    layout->alignment = max_align;
}

static const XrStructLayout *static_layout_struct_from_type(const XrType *type) {
    if (!type || type->is_nullable)
        return NULL;
    if (type->kind != XR_KIND_INSTANCE && type->kind != XR_KIND_CLASS)
        return NULL;
    XrClassInfo *info = type->instance.class_ref;
    if (!info || !info->struct_layout)
        return NULL;
    const XrStructLayout *layout = info->struct_layout;
    return layout;
}

bool xr_type_static_layout(const XrType *type, uint32_t *out_size, uint32_t *out_align) {
    if (out_size)
        *out_size = 0;
    if (out_align)
        *out_align = 0;
    if (!type || type->is_nullable)
        return false;

    int native = xr_type_kind_to_native(type->kind, type->native_width);
    if (native >= 0) {
        uint8_t size = xr_native_type_size((uint8_t) native);
        uint8_t align = xr_native_type_align((uint8_t) native);
        if (size == 0 || align == 0)
            return false;
        if (out_size)
            *out_size = size;
        if (out_align)
            *out_align = align;
        return true;
    }

    if (type->kind == XR_KIND_POINTER) {
        if (out_size)
            *out_size = (uint32_t) sizeof(void *);
        if (out_align)
            *out_align = (uint32_t) _Alignof(void *);
        return true;
    }

    if (type->kind == XR_KIND_FIXED_ARRAY && type->fixed_array.length > 0 &&
        type->fixed_array.element_type) {
        uint32_t elem_size = 0;
        uint32_t elem_align = 0;
        if (!xr_type_static_layout(type->fixed_array.element_type, &elem_size, &elem_align))
            return false;
        uint64_t total = (uint64_t) elem_size * (uint64_t) type->fixed_array.length;
        if (total > UINT32_MAX)
            return false;
        if (out_size)
            *out_size = (uint32_t) total;
        if (out_align)
            *out_align = elem_align;
        return true;
    }

    const XrStructLayout *layout = static_layout_struct_from_type(type);
    if (layout) {
        if (out_size)
            *out_size = layout->total_size;
        if (out_align)
            *out_align = layout->alignment;
        return true;
    }

    return false;
}

bool xr_type_static_field_offset(const XrType *type, const char *field_name, uint32_t *out_offset) {
    if (out_offset)
        *out_offset = 0;
    if (!field_name)
        return false;

    const XrStructLayout *layout = static_layout_struct_from_type(type);
    if (!layout || !layout->field_names)
        return false;

    for (uint16_t i = 0; i < layout->field_count; i++) {
        const char *name = layout->field_names[i];
        if (name && strcmp(name, field_name) == 0) {
            if (out_offset)
                *out_offset = layout->fields[i].offset;
            return true;
        }
    }
    return false;
}
