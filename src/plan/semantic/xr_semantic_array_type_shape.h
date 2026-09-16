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

#include "xr_semantic_class_shape.h"
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
    XrStableId zero = {{0}};
    const uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
    if (!type || type->kind != XR_KIND_ARRAY || type->builtin_type != XR_TID_NULL ||
        type->child_count != 1 || type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->scalar_rep != XR_SCALAR_REP_NONE ||
        (type->flags & ~(uint8_t) (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_CONST)) != required ||
        type->source_class != XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(type->source_class_identity, zero) || !type->canonical_key)
        return false;
    /* Qualifiers compose independently; matching one selected spelling would
     * reject a const nullable reference even though its carrier is unchanged. */
    char expected[96];
    int length = snprintf(
        expected, sizeof(expected),
        "type-v3:%u:0:%u:%u:%u:0:0:0:0:%u:0:;element:", (unsigned) XR_KIND_ARRAY,
        (unsigned) XR_TID_NULL, (unsigned) ((type->flags & XR_SEM_TYPE_NULLABLE) != 0),
        (unsigned) ((type->flags & XR_SEM_TYPE_CONST) != 0), (unsigned) XR_SCALAR_REP_NONE);
    return length > 0 && (size_t) length < sizeof(expected) &&
           strncmp(type->canonical_key, expected, (size_t) length) == 0;
}

/* A reference parameter uses the same ownership contract whether
 * the callee borrows or consumes the allocation. The declaration is the sole
 * ownership authority: a read-only body borrows, while a body that retains or
 * consumes the value owns and releases it. Target construction, independent
 * verification, and AOT refinement all use this judgement before applying
 * their exact type and target storage checks. */
static inline bool xr_semantic_direct_local_reference_parameter_is_exact(
    const XrSemanticPlan *plan, const XrSemanticParameterRecord *parameter, bool *callee_owns) {
    if (!plan || !parameter || parameter->function >= xr_semantic_plan_function_count(plan) ||
        parameter->value == XR_SEMANTIC_INDEX_NONE || parameter->mode != XR_PARAM_READ ||
        (parameter->ownership != XI_OWN_BORROWED && parameter->ownership != XI_OWN_OWNED) ||
        parameter->transfer_mode != XR_TRANSFER_SHARE ||
        (parameter->flags & ~(XR_SEM_PARAMETER_REQUIRED | XR_SEM_PARAMETER_VARIADIC)) != 0 ||
        parameter->reserved != 0 || parameter->type >= xr_semantic_plan_type_count(plan) ||
        (xr_semantic_plan_type(plan, parameter->type)->flags &
         (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT)) !=
            (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    /* A rest parameter is an Array at the callee boundary. Its declaration
     * retains the same ownership and storage contract as an ordinary Array
     * parameter; only argument collection differs at the call boundary. */
    if ((parameter->flags & XR_SEM_PARAMETER_VARIADIC) != 0 &&
        ((parameter->flags & XR_SEM_PARAMETER_REQUIRED) != 0 ||
         parameter->ordinal + 1u !=
             xr_semantic_plan_function(plan, parameter->function)->parameter_count ||
         !xr_semantic_array_type_row_is_exact(xr_semantic_plan_type(plan, parameter->type))))
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

/* A field read borrows the Array carrier from an exact source-class
 * instance. The class judgement proves the load and receiver declaration;
 * this judgement adds the exact Array result row and unique definition. It
 * does not infer a field declaration or element layout: the frontend already
 * froze the selected field in the result type, while TargetPlan remains the
 * sole owner of element storage. */
static inline bool
xr_semantic_tagged_array_field_read_is_exact(const XrSemanticPlan *plan,
                                             const XrSemanticOperationRecord *operation) {
    return plan && operation &&
           xr_semantic_class_field_read_source_class(plan, operation) != XR_SEMANTIC_INDEX_NONE &&
           xr_semantic_array_type_row_is_exact(
               xr_semantic_plan_type(plan, operation->result_type)) &&
           xr_semantic_unique_value_definition(plan, operation->result_value) == operation;
}

#endif /* XR_SEMANTIC_ARRAY_TYPE_SHAPE_H */
