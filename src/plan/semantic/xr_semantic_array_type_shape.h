/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_array_type_shape.h - The exact member-Array type row
 *
 * KEY CONCEPT:
 *   Array allocation, Array members and the higher-order forms all rest on the
 *   same question: is this type row exactly the compiler-owned Array shape?
 *   The builder and the verifier each used to spell it out, and the two
 *   spellings must agree -- a row one accepts and the other refuses fails as a
 *   plan-level refusal with nothing in it pointing at the disagreement.
 */

#ifndef XR_SEMANTIC_ARRAY_TYPE_SHAPE_H
#define XR_SEMANTIC_ARRAY_TYPE_SHAPE_H

#include "xr_semantic_plan.h"
#include "xr_semantic_shared_read_shape.h"
#include "../../ir/xi_own.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>

/* Nullability is a fact about the reference, not about what the array holds:
 * a non-null `Array<T>?` lays its elements out exactly as `Array<T>` does, and
 * a null one has no elements at all. Every answer these rows carry -- element
 * storage above all -- reads the same either way, so the flag is admitted.
 * The canonical key encodes it as a field, so it is matched in both spellings
 * rather than by skipping past it. */
static inline bool xr_semantic_array_type_row_is_exact(const XrSemanticTypeRecord *type) {
    char expected[96];
    char expected_nullable[96];
    int length = snprintf(expected, sizeof(expected),
                          "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;element:", (unsigned) XR_KIND_ARRAY,
                          (unsigned) XR_TID_NULL, (unsigned) XR_SCALAR_REP_NONE);
    int nullable_length =
        snprintf(expected_nullable, sizeof(expected_nullable),
                 "type-v3:%u:0:%u:1:0:0:0:0:0:%u:0:;element:", (unsigned) XR_KIND_ARRAY,
                 (unsigned) XR_TID_NULL, (unsigned) XR_SCALAR_REP_NONE);
    /* The key encodes const in its fifth field, so admitting the flag without
     * admitting the spelling would still refuse a constant array. */
    char expected_const[96];
    int const_length =
        snprintf(expected_const, sizeof(expected_const),
                 "type-v3:%u:0:%u:0:1:0:0:0:0:%u:0:;element:", (unsigned) XR_KIND_ARRAY,
                 (unsigned) XR_TID_NULL, (unsigned) XR_SCALAR_REP_NONE);
    XrStableId zero = {{0}};
    const uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
    if (!type || length <= 0 || (size_t) length >= sizeof(expected) || nullable_length <= 0 ||
        (size_t) nullable_length >= sizeof(expected_nullable))
        return false;
    if (type->kind != XR_KIND_ARRAY || type->builtin_type != XR_TID_NULL ||
        type->child_count != 1 || type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->scalar_rep != XR_SCALAR_REP_NONE ||
        /* `const` states that the binding cannot be reassigned; it does not
         * change how the array is held, and the canonical key does not encode
         * it. Admitting only nullable here left `const xs = ...` outside every
         * Array family, so a constant array had no storage anywhere. */
        (type->flags & ~(uint8_t) (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_CONST)) != required ||
        type->source_class != XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(type->source_class_identity, zero) || !type->canonical_key)
        return false;
    if (const_length <= 0 || (size_t) const_length >= sizeof(expected_const))
        return false;
    if (type->flags & XR_SEM_TYPE_NULLABLE)
        return strncmp(type->canonical_key, expected_nullable, (size_t) nullable_length) == 0;
    if (type->flags & XR_SEM_TYPE_CONST)
        return strncmp(type->canonical_key, expected_const, (size_t) const_length) == 0;
    return strncmp(type->canonical_key, expected, (size_t) length) == 0;
}

/* An Array parameter handed over by value uses the same tagged carrier whether
 * the callee borrows or consumes the allocation. The declaration is the sole
 * ownership authority: a read-only body borrows, while a body that retains or
 * consumes the value owns and releases it. Target construction, independent
 * verification, and AOT refinement all use this judgement before applying
 * their target-specific element-storage checks. */
static inline bool xr_semantic_direct_local_array_value_parameter_is_exact(
    const XrSemanticPlan *plan, const XrSemanticParameterRecord *parameter, bool *callee_owns) {
    if (!plan || !parameter || parameter->function >= xr_semantic_plan_function_count(plan) ||
        parameter->value == XR_SEMANTIC_INDEX_NONE || parameter->mode != XR_PARAM_READ ||
        (parameter->ownership != XI_OWN_BORROWED && parameter->ownership != XI_OWN_OWNED) ||
        parameter->transfer_mode != XR_TRANSFER_SHARE ||
        (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) != 0 || parameter->reserved != 0 ||
        !xr_semantic_array_type_row_is_exact(xr_semantic_plan_type(plan, parameter->type)))
        return false;
    if (callee_owns)
        *callee_owns = parameter->ownership == XI_OWN_OWNED;
    return true;
}

/* A borrowed read of an Array held in a shared cell. The shared operation
 * proves the carrier and ownership shape; the exact Array row proves that this
 * judgement cannot accidentally claim another reference-capable value. The
 * unique definition requirement makes a storage row describe the value's
 * whole life rather than one of several competing producers. Element storage
 * stays a TargetPlan question because it is target layout authority, not a
 * SemanticPlan fact. */
static inline bool
xr_semantic_tagged_array_shared_read_is_exact(const XrSemanticPlan *plan,
                                              const XrSemanticOperationRecord *operation) {
    return plan && operation && xr_semantic_shared_read_operation_is_exact(operation) &&
           xr_semantic_array_type_row_is_exact(
               xr_semantic_plan_type(plan, operation->result_type)) &&
           xr_semantic_unique_value_definition(plan, operation->result_value) == operation;
}

#endif /* XR_SEMANTIC_ARRAY_TYPE_SHAPE_H */
