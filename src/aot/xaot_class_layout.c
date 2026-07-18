/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_class_layout.c - Physical generated-C layout for class fields
 */

#include "xaot_class_layout.h"
#include "../analysis/xglobal_summary.h"
#include "../shared/xr_native_type_core.h"

static void class_field_scalar_layout(XaotClassFieldPhysicalLayout *layout,
                                      const XrTargetTypeLayout *type_layout) {
    layout->size = type_layout->size;
    layout->align = type_layout->align;
    layout->storage_kind = XAOT_CLASS_FIELD_STORAGE_SCALAR;
    layout->action = XAOT_CLASS_FIELD_ACTION_NATIVE_SCALAR;
    layout->representation = XAOT_CLASS_FIELD_REP_NATIVE_SCALAR;
    layout->ownership = XAOT_CLASS_FIELD_OWNERSHIP_TRIVIAL;
    layout->drop_kind = XAOT_CLASS_FIELD_DROP_NONE;
}

static void class_field_tagged_layout(XaotClassFieldPhysicalLayout *layout,
                                      const XrTargetDataLayout *target_layout, uint8_t action,
                                      uint8_t ownership, uint8_t drop_kind, uint8_t reason) {
    layout->size = target_layout->xr_value.size;
    layout->align = target_layout->xr_value.align;
    layout->native_type = XR_NATIVE_VALUE;
    layout->storage_kind = XAOT_CLASS_FIELD_STORAGE_TAGGED_VALUE;
    layout->action = action;
    layout->representation = XAOT_CLASS_FIELD_REP_TAGGED_VALUE;
    layout->ownership = ownership;
    layout->drop_kind = drop_kind;
    layout->unproven_reason = reason;
}

static void class_field_pointer_layout(XaotClassFieldPhysicalLayout *layout,
                                       const XrTargetDataLayout *target_layout, uint8_t native_type,
                                       uint8_t ref_kind) {
    layout->size = target_layout->pointer.size;
    layout->align = target_layout->pointer.align;
    layout->native_type = native_type;
    layout->ref_kind = ref_kind;
    layout->storage_kind = XAOT_CLASS_FIELD_STORAGE_POINTER_REF;
    layout->action = XAOT_CLASS_FIELD_ACTION_POINTER_REF;
    layout->representation = XAOT_CLASS_FIELD_REP_POINTER_REF;
    layout->ownership = XAOT_CLASS_FIELD_OWNERSHIP_OWNED;
    layout->drop_kind = XAOT_CLASS_FIELD_DROP_REF_RELEASE;
}

XR_FUNC bool xaot_class_field_physical_layout(const XrTargetDataLayout *target_layout,
                                              uint8_t semantic_kind,
                                              XaotClassFieldPhysicalLayout *out_layout) {
    XaotClassFieldPhysicalLayout layout = {0};
    if (!xr_target_data_layout_validate(target_layout))
        return false;
    switch ((XgClassFieldTypeKind) semantic_kind) {
        case XG_CLASS_FIELD_TYPE_I8:
            layout.native_type = XR_NATIVE_I8;
            class_field_scalar_layout(&layout, &target_layout->i8);
            break;
        case XG_CLASS_FIELD_TYPE_U8:
            layout.native_type = XR_NATIVE_U8;
            class_field_scalar_layout(&layout, &target_layout->i8);
            break;
        case XG_CLASS_FIELD_TYPE_I16:
            layout.native_type = XR_NATIVE_I16;
            class_field_scalar_layout(&layout, &target_layout->i16);
            break;
        case XG_CLASS_FIELD_TYPE_U16:
            layout.native_type = XR_NATIVE_U16;
            class_field_scalar_layout(&layout, &target_layout->i16);
            break;
        case XG_CLASS_FIELD_TYPE_I32:
            layout.native_type = XR_NATIVE_I32;
            class_field_scalar_layout(&layout, &target_layout->i32);
            break;
        case XG_CLASS_FIELD_TYPE_U32:
        case XG_CLASS_FIELD_TYPE_RUNE:
            layout.native_type = XR_NATIVE_U32;
            class_field_scalar_layout(&layout, &target_layout->i32);
            break;
        case XG_CLASS_FIELD_TYPE_I64:
            layout.native_type = XR_NATIVE_I64;
            class_field_scalar_layout(&layout, &target_layout->i64);
            break;
        case XG_CLASS_FIELD_TYPE_U64:
            layout.native_type = XR_NATIVE_U64;
            class_field_scalar_layout(&layout, &target_layout->i64);
            break;
        case XG_CLASS_FIELD_TYPE_ISIZE:
            layout.native_type = XR_NATIVE_ISIZE;
            class_field_scalar_layout(&layout, &target_layout->isize);
            break;
        case XG_CLASS_FIELD_TYPE_USIZE:
            layout.native_type = XR_NATIVE_USIZE;
            class_field_scalar_layout(&layout, &target_layout->usize);
            break;
        case XG_CLASS_FIELD_TYPE_F32:
            layout.native_type = XR_NATIVE_F32;
            class_field_scalar_layout(&layout, &target_layout->f32);
            break;
        case XG_CLASS_FIELD_TYPE_F64:
            layout.native_type = XR_NATIVE_F64;
            class_field_scalar_layout(&layout, &target_layout->f64);
            break;
        case XG_CLASS_FIELD_TYPE_BOOL:
            layout.native_type = XR_NATIVE_BOOL;
            class_field_scalar_layout(&layout, &target_layout->i8);
            break;
        case XG_CLASS_FIELD_TYPE_STRING:
            class_field_tagged_layout(&layout, target_layout, XAOT_CLASS_FIELD_ACTION_TAGGED_VALUE,
                                      XAOT_CLASS_FIELD_OWNERSHIP_OWNED,
                                      XAOT_CLASS_FIELD_DROP_VALUE_RELEASE,
                                      XAOT_CLASS_FIELD_UNPROVEN_NONE);
            layout.native_type = XR_NATIVE_STRING;
            break;
        case XG_CLASS_FIELD_TYPE_ARRAY:
            class_field_pointer_layout(&layout, target_layout, XR_NATIVE_ARRAY_REF,
                                       XAOT_CLASS_FIELD_REF_ARRAY);
            break;
        case XG_CLASS_FIELD_TYPE_MAP:
            class_field_pointer_layout(&layout, target_layout, XR_NATIVE_MAP_REF,
                                       XAOT_CLASS_FIELD_REF_MAP);
            break;
        case XG_CLASS_FIELD_TYPE_SET:
            class_field_pointer_layout(&layout, target_layout, XR_NATIVE_SET_REF,
                                       XAOT_CLASS_FIELD_REF_SET);
            break;
        case XG_CLASS_FIELD_TYPE_CLASS:
            class_field_tagged_layout(
                &layout, target_layout, XAOT_CLASS_FIELD_ACTION_TAGGED_FALLBACK,
                XAOT_CLASS_FIELD_OWNERSHIP_OWNED, XAOT_CLASS_FIELD_DROP_VALUE_RELEASE,
                XAOT_CLASS_FIELD_UNPROVEN_CLASS_LAYOUT);
            break;
        case XG_CLASS_FIELD_TYPE_INTERFACE:
            class_field_tagged_layout(
                &layout, target_layout, XAOT_CLASS_FIELD_ACTION_TAGGED_FALLBACK,
                XAOT_CLASS_FIELD_OWNERSHIP_OWNED, XAOT_CLASS_FIELD_DROP_VALUE_RELEASE,
                XAOT_CLASS_FIELD_UNPROVEN_INTERFACE_LAYOUT);
            break;
        case XG_CLASS_FIELD_TYPE_ENUM:
            class_field_tagged_layout(
                &layout, target_layout, XAOT_CLASS_FIELD_ACTION_TAGGED_FALLBACK,
                XAOT_CLASS_FIELD_OWNERSHIP_OWNED, XAOT_CLASS_FIELD_DROP_VALUE_RELEASE,
                XAOT_CLASS_FIELD_UNPROVEN_ENUM_LAYOUT);
            break;
        case XG_CLASS_FIELD_TYPE_STRUCT:
        case XG_CLASS_FIELD_TYPE_FIXED_UNION:
        case XG_CLASS_FIELD_TYPE_FIXED_ARRAY:
            class_field_tagged_layout(
                &layout, target_layout, XAOT_CLASS_FIELD_ACTION_TAGGED_FALLBACK,
                XAOT_CLASS_FIELD_OWNERSHIP_OWNED, XAOT_CLASS_FIELD_DROP_VALUE_RELEASE,
                XAOT_CLASS_FIELD_UNPROVEN_FIXED_LAYOUT);
            break;
        case XG_CLASS_FIELD_TYPE_OPTIONAL:
            class_field_tagged_layout(
                &layout, target_layout, XAOT_CLASS_FIELD_ACTION_TAGGED_FALLBACK,
                XAOT_CLASS_FIELD_OWNERSHIP_OWNED, XAOT_CLASS_FIELD_DROP_VALUE_RELEASE,
                XAOT_CLASS_FIELD_UNPROVEN_OPTIONAL_LAYOUT);
            break;
        case XG_CLASS_FIELD_TYPE_UNION:
            class_field_tagged_layout(
                &layout, target_layout, XAOT_CLASS_FIELD_ACTION_TAGGED_FALLBACK,
                XAOT_CLASS_FIELD_OWNERSHIP_OWNED, XAOT_CLASS_FIELD_DROP_VALUE_RELEASE,
                XAOT_CLASS_FIELD_UNPROVEN_UNION_LAYOUT);
            break;
        case XG_CLASS_FIELD_TYPE_FUNCTION:
        case XG_CLASS_FIELD_TYPE_TUPLE:
        case XG_CLASS_FIELD_TYPE_OBJECT:
            class_field_tagged_layout(
                &layout, target_layout, XAOT_CLASS_FIELD_ACTION_TAGGED_FALLBACK,
                XAOT_CLASS_FIELD_OWNERSHIP_OWNED, XAOT_CLASS_FIELD_DROP_VALUE_RELEASE,
                XAOT_CLASS_FIELD_UNPROVEN_NESTED_LAYOUT);
            break;
        case XG_CLASS_FIELD_TYPE_TYPE_PARAM:
            class_field_tagged_layout(
                &layout, target_layout, XAOT_CLASS_FIELD_ACTION_TAGGED_FALLBACK,
                XAOT_CLASS_FIELD_OWNERSHIP_OWNED, XAOT_CLASS_FIELD_DROP_VALUE_RELEASE,
                XAOT_CLASS_FIELD_UNPROVEN_TYPE_PARAM);
            break;
        case XG_CLASS_FIELD_TYPE_UNIT:
            class_field_tagged_layout(
                &layout, target_layout, XAOT_CLASS_FIELD_ACTION_TAGGED_FALLBACK,
                XAOT_CLASS_FIELD_OWNERSHIP_TRIVIAL, XAOT_CLASS_FIELD_DROP_NONE,
                XAOT_CLASS_FIELD_UNPROVEN_UNIT_VALUE);
            break;
        case XG_CLASS_FIELD_TYPE_NULL:
            class_field_tagged_layout(
                &layout, target_layout, XAOT_CLASS_FIELD_ACTION_TAGGED_FALLBACK,
                XAOT_CLASS_FIELD_OWNERSHIP_TRIVIAL, XAOT_CLASS_FIELD_DROP_NONE,
                XAOT_CLASS_FIELD_UNPROVEN_NULL_VALUE);
            break;
        case XG_CLASS_FIELD_TYPE_DYNAMIC:
            class_field_tagged_layout(
                &layout, target_layout, XAOT_CLASS_FIELD_ACTION_TAGGED_FALLBACK,
                XAOT_CLASS_FIELD_OWNERSHIP_OWNED, XAOT_CLASS_FIELD_DROP_VALUE_RELEASE,
                XAOT_CLASS_FIELD_UNPROVEN_DYNAMIC_TYPE);
            break;
        default:
            return false;
    }
    if (out_layout)
        *out_layout = layout;
    return true;
}
