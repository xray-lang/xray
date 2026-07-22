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
#include "../../base/xmalloc.h"
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

bool xr_aggregate_layout_compute(XrAggregateLayout *layout,
                                 const XrTargetDataLayout *target_layout) {
    if (!layout || !xr_target_data_layout_validate(target_layout))
        return false;
    if (layout->field_count > XR_MAX_AGG_FIELDS || layout->kind > XR_AGG_LAYOUT_UNION ||
        (layout->explicit_align != 0 &&
         (layout->explicit_align & (layout->explicit_align - 1u)) != 0))
        return false;
    if (layout->field_count == 0) {
        layout->total_size = 0;
        layout->alignment = 1;
        layout->target_abi_hash = target_layout->stable_hash;
        return true;
    }

    uint32_t offset = 0;
    uint32_t max_align = 1;
    uint32_t max_size = 0;

    for (int i = 0; i < layout->field_count; i++) {
        XrAggregateFieldLayout *f = &layout->fields[i];

        if (f->is_flexible &&
            (layout->kind == XR_AGG_LAYOUT_UNION || i + 1 != layout->field_count ||
             f->native_type != XR_NATIVE_ARRAY || f->elem_count != 0))
            return false;

        // Auto-compute size from native_type (except nested struct and array)
        if (f->is_flexible) {
            // C flexible array member: it contributes alignment and an offset,
            // but sizeof(struct) contains only the padded header.
            f->size = 0;
        } else if (f->native_type == XR_NATIVE_ARRAY) {
            // Fixed-size array: size = elem_count * elem_size
            uint8_t es = xr_native_type_size(target_layout, f->elem_native_type);
            if (es == 0 || (uint64_t) f->elem_count * es > UINT16_MAX)
                return false;
            f->size = (uint16_t) (f->elem_count * es);
        } else if (f->native_type == XR_NATIVE_NESTED_AGGREGATE) {
            if (!f->sub_layout || f->sub_layout->target_abi_hash != target_layout->stable_hash)
                return false;
            f->size = f->sub_layout->total_size;
        } else {
            f->size = xr_native_type_size(target_layout, f->native_type);
            if (f->size == 0)
                return false;
        }

        uint32_t field_align;
        if (layout->kind == XR_AGG_LAYOUT_PACKED_STRUCT) {
            field_align = 1;
        } else if (f->is_flexible) {
            field_align = xr_native_type_align(target_layout, f->elem_native_type);
        } else if (f->native_type == XR_NATIVE_NESTED_AGGREGATE) {
            field_align = f->sub_layout->alignment;
        } else if (f->native_type == XR_NATIVE_ARRAY) {
            field_align = xr_native_type_align(target_layout, f->elem_native_type);
        } else {
            field_align = xr_native_type_align(target_layout, f->native_type);
        }
        if (field_align == 0 || (field_align & (field_align - 1u)) != 0)
            return false;

        if (layout->kind == XR_AGG_LAYOUT_UNION) {
            f->offset = 0;
            if (f->size > max_size)
                max_size = f->size;
            if (field_align > max_align)
                max_align = field_align;
            continue;
        }

        // Align offset to field alignment
        offset = (offset + field_align - 1) & ~(field_align - 1);

        if ((uint64_t) offset + f->size > UINT16_MAX)
            return false;
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
    uint32_t raw_size = (layout->kind == XR_AGG_LAYOUT_UNION) ? max_size : offset;
    uint64_t total = ((uint64_t) raw_size + max_align - 1u) & ~((uint64_t) max_align - 1u);
    if (total > UINT16_MAX)
        return false;
    layout->total_size = (uint16_t) total;
    layout->alignment = max_align;
    layout->target_abi_hash = target_layout->stable_hash;
    return true;
}

static uint64_t layout_hash_word(uint64_t hash, uint64_t word) {
    for (uint32_t i = 0; i < 8; i++) {
        hash ^= (uint8_t) (word >> (i * 8u));
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t layout_hash_string(uint64_t hash, const char *value) {
    if (value) {
        for (const unsigned char *p = (const unsigned char *) value; *p; p++) {
            hash ^= *p;
            hash *= UINT64_C(1099511628211);
        }
    }
    hash ^= UINT64_C(0xff);
    return hash * UINT64_C(1099511628211);
}

static uint64_t aggregate_layout_stable_key_depth(const XrAggregateLayout *layout, uint32_t depth) {
    uint64_t hash = UINT64_C(1469598103934665603);
    if (!layout || depth > 16)
        return layout_hash_word(hash, UINT64_C(0xffffffffffffffff));
    hash = layout_hash_word(hash, layout->target_abi_hash);
    hash = layout_hash_word(hash, layout->kind);
    hash = layout_hash_word(hash, layout->explicit_align);
    hash = layout_hash_word(hash, layout->total_size);
    hash = layout_hash_word(hash, layout->alignment);
    hash = layout_hash_word(hash, layout->field_count);
    for (uint16_t i = 0; i < layout->field_count && i < XR_MAX_AGG_FIELDS; i++) {
        const XrAggregateFieldLayout *field = &layout->fields[i];
        hash = layout_hash_string(hash, layout->field_names ? layout->field_names[i] : NULL);
        hash = layout_hash_word(hash, field->offset);
        hash = layout_hash_word(hash, field->native_type);
        hash = layout_hash_word(hash, field->size);
        hash = layout_hash_word(hash, field->elem_native_type);
        hash = layout_hash_word(hash, field->elem_count);
        hash = layout_hash_word(hash, field->is_flexible ? 1u : 0u);
        if (field->native_type == XR_NATIVE_NESTED_AGGREGATE)
            hash = layout_hash_word(
                hash, aggregate_layout_stable_key_depth(field->sub_layout, depth + 1));
    }
    return hash ? hash : UINT64_C(1);
}

uint64_t xr_aggregate_layout_stable_key(const XrAggregateLayout *layout) {
    return aggregate_layout_stable_key_depth(layout, 0);
}

static bool aggregate_layout_equal_depth(const XrAggregateLayout *left,
                                         const XrAggregateLayout *right, uint32_t depth) {
    if (left == right)
        return true;
    if (!left || !right || depth > 16 || left->target_abi_hash != right->target_abi_hash ||
        left->kind != right->kind || left->explicit_align != right->explicit_align ||
        left->total_size != right->total_size || left->alignment != right->alignment ||
        left->field_count != right->field_count || left->field_count > XR_MAX_AGG_FIELDS)
        return false;
    for (uint16_t i = 0; i < left->field_count; i++) {
        const XrAggregateFieldLayout *lf = &left->fields[i];
        const XrAggregateFieldLayout *rf = &right->fields[i];
        const char *ln = left->field_names ? left->field_names[i] : NULL;
        const char *rn = right->field_names ? right->field_names[i] : NULL;
        if ((ln == NULL) != (rn == NULL) || (ln && strcmp(ln, rn) != 0) ||
            lf->offset != rf->offset || lf->native_type != rf->native_type ||
            lf->size != rf->size || lf->elem_native_type != rf->elem_native_type ||
            lf->elem_count != rf->elem_count || lf->is_flexible != rf->is_flexible)
            return false;
        if (lf->native_type == XR_NATIVE_NESTED_AGGREGATE &&
            !aggregate_layout_equal_depth(lf->sub_layout, rf->sub_layout, depth + 1))
            return false;
    }
    return true;
}

bool xr_aggregate_layout_semantically_equal(const XrAggregateLayout *left,
                                            const XrAggregateLayout *right) {
    return aggregate_layout_equal_depth(left, right, 0);
}

void xr_aggregate_layout_free_owned(XrAggregateLayout *layout) {
    if (!layout)
        return;
    if (layout->field_names) {
        for (uint16_t i = 0; i < layout->field_count; i++)
            xr_free((void *) layout->field_names[i]);
        xr_free(layout->field_names);
    }
    xr_free(layout);
}

static const XrAggregateLayout *static_layout_struct_from_type(const XrType *type) {
    if (!type || type->is_nullable)
        return NULL;
    if (type->kind != XR_KIND_INSTANCE && type->kind != XR_KIND_CLASS)
        return NULL;
    XrClassInfo *info = type->instance.class_ref;
    if (!info || !info->struct_layout)
        return NULL;
    const XrAggregateLayout *layout = info->struct_layout;
    return layout;
}

bool xr_type_has_static_layout(const XrTargetDataLayout *target_layout, const XrType *type,
                               uint32_t *out_size, uint32_t *out_align) {
    if (out_size)
        *out_size = 0;
    if (out_align)
        *out_align = 0;
    if (!xr_target_data_layout_validate(target_layout) || !type || type->is_nullable)
        return false;

    int native = xr_type_kind_to_native(type->kind, type->native_width);
    if (native >= 0) {
        uint8_t size = xr_native_type_size(target_layout, (uint8_t) native);
        uint8_t align = xr_native_type_align(target_layout, (uint8_t) native);
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
            *out_size = target_layout->pointer.size;
        if (out_align)
            *out_align = target_layout->pointer.align;
        return true;
    }

    if (type->kind == XR_KIND_FIXED_ARRAY && type->fixed_array.length > 0 &&
        type->fixed_array.element_type) {
        uint32_t elem_size = 0;
        uint32_t elem_align = 0;
        if (!xr_type_has_static_layout(target_layout, type->fixed_array.element_type, &elem_size,
                                       &elem_align))
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

    const XrAggregateLayout *layout = static_layout_struct_from_type(type);
    if (layout && layout->target_abi_hash == target_layout->stable_hash) {
        if (out_size)
            *out_size = layout->total_size;
        if (out_align)
            *out_align = layout->alignment;
        return true;
    }

    return false;
}

static bool native_all_bit_patterns_valid(uint8_t native_type, const XrAggregateFieldLayout *field,
                                          uint32_t depth);

static bool layout_all_bit_patterns_valid(const XrAggregateLayout *layout, uint32_t depth) {
    if (!layout || depth > 16 || layout->field_count > XR_MAX_AGG_FIELDS)
        return false;
    for (uint16_t i = 0; i < layout->field_count; i++) {
        const XrAggregateFieldLayout *field = &layout->fields[i];
        if (field->is_flexible ||
            !native_all_bit_patterns_valid(field->native_type, field, depth + 1))
            return false;
    }
    return true;
}

static bool native_all_bit_patterns_valid(uint8_t native_type, const XrAggregateFieldLayout *field,
                                          uint32_t depth) {
    switch (native_type) {
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
        case XR_NATIVE_I64:
        case XR_NATIVE_U64:
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
        case XR_NATIVE_F32:
        case XR_NATIVE_F64:
            return true;
        case XR_NATIVE_NESTED_AGGREGATE:
            return field && layout_all_bit_patterns_valid(field->sub_layout, depth + 1);
        case XR_NATIVE_ARRAY:
            return field && field->elem_count > 0 &&
                   native_all_bit_patterns_valid(field->elem_native_type, NULL, depth + 1);
        case XR_NATIVE_BOOL:
        case XR_NATIVE_POINTER:
        case XR_NATIVE_STRING:
        case XR_NATIVE_ARRAY_REF:
        case XR_NATIVE_MAP_REF:
        case XR_NATIVE_SET_REF:
        case XR_NATIVE_VALUE:
        default:
            return false;
    }
}

bool xr_type_all_bit_patterns_valid(const XrType *type) {
    if (!type || type->is_nullable)
        return false;
    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
            return true;
        case XR_KIND_FIXED_ARRAY:
            return type->fixed_array.length > 0 && type->fixed_array.element_type &&
                   xr_type_all_bit_patterns_valid(type->fixed_array.element_type);
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE: {
            const XrAggregateLayout *layout = static_layout_struct_from_type(type);
            return layout_all_bit_patterns_valid(layout, 0);
        }
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
        case XR_KIND_ENUM:
        case XR_KIND_POINTER:
        default:
            return false;
    }
}

bool xr_type_has_static_field_offset(const XrTargetDataLayout *target_layout, const XrType *type,
                                     const char *field_name, uint32_t *out_offset) {
    if (out_offset)
        *out_offset = 0;
    if (!xr_target_data_layout_validate(target_layout) || !field_name)
        return false;

    const XrAggregateLayout *layout = static_layout_struct_from_type(type);
    if (!layout || layout->target_abi_hash != target_layout->stable_hash || !layout->field_names)
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
