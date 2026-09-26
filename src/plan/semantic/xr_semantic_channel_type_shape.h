/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_channel_type_shape.h - Exact managed Channel type identity
 */
#ifndef XR_SEMANTIC_CHANNEL_TYPE_SHAPE_H
#define XR_SEMANTIC_CHANNEL_TYPE_SHAPE_H

#include "xr_semantic_plan.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>

static inline bool xr_semantic_channel_type_row_is_exact(
    const XrSemanticPlan *plan, const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    uint32_t child_count = 0;
    const uint32_t *children = plan ? xr_semantic_plan_type_children(plan, &child_count) : NULL;
    const uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
    if (!plan || !type || !children || type->kind != XR_KIND_CHANNEL ||
        type->builtin_type != XR_TID_NULL || type->scalar_rep != XR_SCALAR_REP_NONE ||
        type->child_count != 1 || type->child_begin >= child_count ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        (type->flags & ~(uint8_t) XR_SEM_TYPE_CONST) != required ||
        type->source_class != XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(type->source_class_identity, zero) ||
        !xr_stable_id_equal(type->source_enum_identity, zero) || type->source_enum_key ||
        type->enum_layout_id || type->enum_member_count || type->enum_flags || type->reserved_enum ||
        !type->canonical_key)
        return false;
    const XrSemanticTypeRecord *element = xr_semantic_plan_type(plan, children[type->child_begin]);
    if (!element || !element->canonical_key)
        return false;
    char prefix[96];
    int length = snprintf(prefix, sizeof(prefix),
        "type-v3:%u:0:0:0:%u:0:0:0:0:%u:0:;element:", (unsigned) XR_KIND_CHANNEL,
        (unsigned) ((type->flags & XR_SEM_TYPE_CONST) != 0), (unsigned) XR_SCALAR_REP_NONE);
    return length > 0 && (size_t) length < sizeof(prefix) &&
           strncmp(type->canonical_key, prefix, (size_t) length) == 0 &&
           strcmp(type->canonical_key + length, element->canonical_key) == 0;
}

#endif /* XR_SEMANTIC_CHANNEL_TYPE_SHAPE_H */
