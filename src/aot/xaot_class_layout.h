/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_class_layout.h - Physical generated-C layout for class fields
 */

#ifndef XAOT_CLASS_LAYOUT_H
#define XAOT_CLASS_LAYOUT_H

#include "../base/xdefs.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum XaotClassFieldStorageKind {
    XAOT_CLASS_FIELD_STORAGE_SCALAR = 1,
    XAOT_CLASS_FIELD_STORAGE_TAGGED_VALUE,
    XAOT_CLASS_FIELD_STORAGE_POINTER_REF,
} XaotClassFieldStorageKind;

typedef enum XaotClassFieldAction {
    XAOT_CLASS_FIELD_ACTION_NATIVE_SCALAR = 1,
    XAOT_CLASS_FIELD_ACTION_TAGGED_VALUE,
    XAOT_CLASS_FIELD_ACTION_POINTER_REF,
    XAOT_CLASS_FIELD_ACTION_TAGGED_FALLBACK,
} XaotClassFieldAction;

typedef enum XaotClassFieldValueRep {
    XAOT_CLASS_FIELD_REP_NATIVE_SCALAR = 1,
    XAOT_CLASS_FIELD_REP_TAGGED_VALUE,
    XAOT_CLASS_FIELD_REP_POINTER_REF,
} XaotClassFieldValueRep;

typedef enum XaotClassFieldOwnership {
    XAOT_CLASS_FIELD_OWNERSHIP_TRIVIAL = 1,
    XAOT_CLASS_FIELD_OWNERSHIP_OWNED,
} XaotClassFieldOwnership;

typedef enum XaotClassFieldDropKind {
    XAOT_CLASS_FIELD_DROP_NONE = 1,
    XAOT_CLASS_FIELD_DROP_VALUE_RELEASE,
    XAOT_CLASS_FIELD_DROP_REF_RELEASE,
} XaotClassFieldDropKind;

typedef enum XaotClassFieldRefKind {
    XAOT_CLASS_FIELD_REF_NONE = 0,
    XAOT_CLASS_FIELD_REF_ARRAY,
    XAOT_CLASS_FIELD_REF_MAP,
    XAOT_CLASS_FIELD_REF_SET,
} XaotClassFieldRefKind;

typedef struct XaotTargetTypeLayout {
    uint32_t size;
    uint32_t align;
} XaotTargetTypeLayout;

/* Physical C ABI facts for the code-generation target.  These values must
 * come from the selected target profile, never from the compiler host. */
typedef struct XaotTargetDataLayout {
    XaotTargetTypeLayout i8;
    XaotTargetTypeLayout i16;
    XaotTargetTypeLayout i32;
    XaotTargetTypeLayout i64;
    XaotTargetTypeLayout f32;
    XaotTargetTypeLayout f64;
    XaotTargetTypeLayout pointer;
    XaotTargetTypeLayout isize;
    XaotTargetTypeLayout usize;
    XaotTargetTypeLayout xr_value;
} XaotTargetDataLayout;

enum {
    XAOT_CLASS_FIELD_UNPROVEN_NONE = 0,
    XAOT_CLASS_FIELD_UNPROVEN_CLASS_LAYOUT = 1,
    XAOT_CLASS_FIELD_UNPROVEN_INTERFACE_LAYOUT = 2,
    XAOT_CLASS_FIELD_UNPROVEN_ENUM_LAYOUT = 3,
    XAOT_CLASS_FIELD_UNPROVEN_FIXED_LAYOUT = 4,
    XAOT_CLASS_FIELD_UNPROVEN_OPTIONAL_LAYOUT = 5,
    XAOT_CLASS_FIELD_UNPROVEN_UNION_LAYOUT = 6,
    XAOT_CLASS_FIELD_UNPROVEN_NESTED_LAYOUT = 7,
    XAOT_CLASS_FIELD_UNPROVEN_TYPE_PARAM = 8,
    XAOT_CLASS_FIELD_UNPROVEN_DYNAMIC_TYPE = 9,
    XAOT_CLASS_FIELD_UNPROVEN_UNIT_VALUE = 10,
    XAOT_CLASS_FIELD_UNPROVEN_NULL_VALUE = 11,
};

typedef struct XaotClassFieldPhysicalLayout {
    uint32_t size;
    uint32_t align;
    uint8_t native_type;
    uint8_t storage_kind;
    uint8_t action;
    uint8_t representation;
    uint8_t ownership;
    uint8_t drop_kind;
    uint8_t ref_kind;
    uint8_t unproven_reason;
} XaotClassFieldPhysicalLayout;

XR_FUNC bool xaot_target_data_layout_init_native(XaotTargetDataLayout *out_layout);
XR_FUNC bool xaot_target_data_layout_init_ilp32(XaotTargetDataLayout *out_layout);
XR_FUNC bool xaot_target_data_layout_init_lp64(XaotTargetDataLayout *out_layout);
XR_FUNC bool xaot_target_data_layout_validate(const XaotTargetDataLayout *layout);

XR_FUNC bool xaot_class_field_physical_layout(const XaotTargetDataLayout *target_layout,
                                              uint8_t semantic_kind,
                                              XaotClassFieldPhysicalLayout *out_layout);

#endif /* XAOT_CLASS_LAYOUT_H */
