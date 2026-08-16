/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_native_module_shape.h - Values that cross a native module boundary
 */

#ifndef XR_SEMANTIC_NATIVE_MODULE_SHAPE_H
#define XR_SEMANTIC_NATIVE_MODULE_SHAPE_H

#include "xr_semantic_plan.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>

/* The exact unaliased raw pointer header SemanticPlan emits for `Ptr<T>` and
 * `MutPtr<T>`. A raw pointer is an address-width integer at the value level and
 * is invisible to the collector, so it carries no reference obligation across
 * any boundary. The canonical key stays the authority for the Ptr/MutPtr
 * distinction, and no Xi type is consulted once the plan is frozen. */
static inline bool xr_semantic_raw_pointer_type_is_exact(const XrSemanticTypeRecord *type) {
    unsigned kind = 0, semantic_type = 0, builtin_type = 0;
    unsigned nullable = 0, is_const = 0, is_value = 0, is_literal = 0;
    unsigned cycle_candidate = 0, pointer_mutable = 0, scalar_rep = 0;
    size_t alias_length = 0;
    int consumed = 0;
    XrStableId zero = {{0}};
    return type && type->canonical_key &&
           sscanf(type->canonical_key, "type-v3:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%zu:%n", &kind,
                  &semantic_type, &builtin_type, &nullable, &is_const, &is_value, &is_literal,
                  &cycle_candidate, &pointer_mutable, &scalar_rep, &alias_length,
                  &consumed) == 11 &&
           consumed > 0 && (size_t) consumed == strlen(type->canonical_key) &&
           kind == XR_KIND_POINTER && semantic_type == 0 && builtin_type == XR_TID_NULL &&
           nullable == 0 && is_const == 0 && is_value == 0 && is_literal == 0 &&
           cycle_candidate == 0 && pointer_mutable <= 1 && scalar_rep == XR_SCALAR_REP_NONE &&
           alias_length == 0 && type->kind == XR_KIND_POINTER &&
           type->builtin_type == XR_TID_NULL && type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 && type->enum_layout_id == 0 &&
           type->enum_member_count == 0 && type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->flags == 0 && type->enum_flags == 0 && type->reserved_enum == 0 &&
           !type->source_enum_key && xr_stable_id_equal(type->source_enum_identity, zero);
}

/* One value crossing the boundary of a native stdlib module member.
 *
 * The generated direct shim carries every argument in and the result out as a
 * single plain tagged value, so the two positions ask the same question of a
 * type: is it one machine scalar carrying no reference the caller would have to
 * account for. That is the same family the machine-representation classifier
 * names -- integers with a native width, the two floats, bool, rune, and the
 * exact raw pointer header -- and this judgement stays deliberately in step
 * with it, because a type the backend can put in a register is exactly a type
 * this boundary can carry.
 *
 * The one asymmetry is unit, spelled `()` in a member signature: a member that
 * reports nothing returns it, and the machine classifier names that void. No
 * argument can be unit -- there is no expression producing one to pass -- so
 * admitting it in argument position would widen the family over a shape no
 * callsite can build. `is_result` is that distinction and the only one.
 *
 * This judgement had five identical copies -- SemanticPlan builder and
 * verifier, TargetPlan builder and verifier, AOT refinement -- each admitting
 * only int, float and bool in both positions. That is what kept `mem`, `os` and
 * `sys` members outside the family: the ones returning `()` and the whole
 * pointer-taking half of `mem`. The copies agreed with each other and all five
 * were wrong in the same place, so no layer could report the gap the others
 * had. One definition, parameterized by position, is what makes the two
 * positions differ where they should and stay identical where they must. */
static inline bool
xr_semantic_native_module_boundary_type_is_exact(const XrSemanticTypeRecord *type, bool is_result) {
    if (!type)
        return false;
    if (type->kind == XR_KIND_POINTER)
        return xr_semantic_raw_pointer_type_is_exact(type);
    if (type->builtin_type != XR_TID_NULL || type->flags != 0 || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->source_class != XR_SEMANTIC_INDEX_NONE)
        return false;
    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
            return type->scalar_rep != XR_SCALAR_REP_NONE;
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
            return type->scalar_rep == XR_SCALAR_REP_NONE;
        case XR_KIND_UNIT:
            return is_result && type->scalar_rep == XR_SCALAR_REP_NONE;
        default:
            return false;
    }
}

#endif /* XR_SEMANTIC_NATIVE_MODULE_SHAPE_H */
