/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_const_variant_shape.h - Two spellings of one type
 */

#ifndef XR_SEMANTIC_CONST_VARIANT_SHAPE_H
#define XR_SEMANTIC_CONST_VARIANT_SHAPE_H

#include "xr_semantic_plan.h"

/* Whether two type indices name the same type, allowing one to be the const
 * spelling of the other.
 *
 * The const qualifier is part of the interned identity, so a type and its const
 * form are two rows with two indices. That is right for the type table and
 * wrong for a judgement asking "is this the type the declaration says", because
 * the language sometimes requires the argument to be const-derived while the
 * parameter that receives it is declared without the qualifier -- `go f(x)`
 * being the case this exists for. Comparing indices there refuses every such
 * call, and the refusal names the argument rather than the qualifier.
 *
 * Everything except the const bit must agree, the children included: a
 * qualifier difference is the only difference admitted, and it is admitted only
 * at the top. */
static inline bool xr_semantic_type_is_const_variant(const XrSemanticPlan *plan, uint32_t left,
                                                     uint32_t right) {
    if (left == right)
        return left != XR_SEMANTIC_INDEX_NONE;
    const XrSemanticTypeRecord *a = xr_semantic_plan_type(plan, left);
    const XrSemanticTypeRecord *b = xr_semantic_plan_type(plan, right);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    if (!a || !b || a->kind != b->kind || a->builtin_type != b->builtin_type ||
        a->scalar_rep != b->scalar_rep || a->child_count != b->child_count ||
        a->aggregate_extent != b->aggregate_extent || a->aggregate_align != b->aggregate_align ||
        a->source_class != b->source_class || a->enum_layout_id != b->enum_layout_id ||
        a->enum_member_count != b->enum_member_count || a->enum_flags != b->enum_flags ||
        a->reserved_enum != b->reserved_enum ||
        !xr_stable_id_equal(a->source_class_identity, b->source_class_identity) ||
        !xr_stable_id_equal(a->source_enum_identity, b->source_enum_identity) ||
        (uint8_t) (a->flags | XR_SEM_TYPE_CONST) != (uint8_t) (b->flags | XR_SEM_TYPE_CONST))
        return false;
    if ((a->source_enum_key == NULL) != (b->source_enum_key == NULL))
        return false;
    if (a->source_enum_key && strcmp(a->source_enum_key, b->source_enum_key) != 0)
        return false;
    if (!children || a->child_count == 0)
        return a->child_count == 0;
    if (a->child_begin >= child_count || b->child_begin >= child_count ||
        a->child_count > child_count - a->child_begin ||
        b->child_count > child_count - b->child_begin)
        return false;
    for (uint16_t i = 0; i < a->child_count; i++) {
        if (children[a->child_begin + i] != children[b->child_begin + i])
            return false;
    }
    return true;
}

#endif /* XR_SEMANTIC_CONST_VARIANT_SHAPE_H */
