/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_heap_literal_shape.h - Exact heap-literal constant authority
 *
 * KEY CONCEPT:
 *   A String or BigInt literal is a constant operation that owns a
 *   reference-capable heap value whose payload is text. The target builder
 *   binds its storage and the target verifier checks that binding, so both
 *   must agree on which operations are such literals -- one statement here
 *   rather than one on each side, which is how the two drift apart.
 */

#ifndef XR_SEMANTIC_HEAP_LITERAL_SHAPE_H
#define XR_SEMANTIC_HEAP_LITERAL_SHAPE_H

#include "xr_semantic_plan.h"
#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"

/* Everything a heap literal must satisfy apart from which payload it carries:
 * a no-operand constant that owns its result, allocates nothing, and whose
 * type is a bare reference-capable ownership root. */
static inline bool xr_semantic_heap_literal_frame_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    const XrSemanticConstantRecord **constant_out, const XrSemanticTypeRecord **type_out) {
    if (!plan || !operation || operation->opcode != XI_CONST || operation->operand_count != 0 ||
        operation->constant >= xr_semantic_plan_constant_count(plan) ||
        operation->allocation_key != NULL ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        operation->return_complete != 1)
        return false;
    for (uint32_t i = 0; i < XR_STABLE_ID_BYTES; i++) {
        if (operation->allocation_id.bytes[i] != 0)
            return false;
    }
    const XrSemanticConstantRecord *constant = xr_semantic_plan_constant(plan, operation->constant);
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, operation->result_type);
    if (!constant || !constant->string || constant->type != operation->result_type || !type ||
        type->child_count != 0 || type->scalar_rep != XR_SCALAR_REP_NONE ||
        type->aggregate_extent != 0 || type->aggregate_align != 0)
        return false;
    const uint8_t forbidden = XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE | XR_SEM_TYPE_BORROW_VIEW |
                              XR_SEM_TYPE_AGGREGATE_EXACT;
    const uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
    if ((type->flags & forbidden) != 0 || (type->flags & required) != required)
        return false;
    if (constant_out)
        *constant_out = constant;
    if (type_out)
        *type_out = type;
    return true;
}

static inline bool xr_semantic_string_literal_is_exact(const XrSemanticPlan *plan,
                                                       const XrSemanticOperationRecord *operation) {
    const XrSemanticConstantRecord *constant = NULL;
    const XrSemanticTypeRecord *type = NULL;
    return xr_semantic_heap_literal_frame_is_exact(plan, operation, &constant, &type) &&
           constant->kind == XR_SEM_CONST_STRING && type->kind == XR_KIND_STRING;
}

static inline bool xr_semantic_bigint_literal_is_exact(const XrSemanticPlan *plan,
                                                       const XrSemanticOperationRecord *operation) {
    const XrSemanticConstantRecord *constant = NULL;
    const XrSemanticTypeRecord *type = NULL;
    return xr_semantic_heap_literal_frame_is_exact(plan, operation, &constant, &type) &&
           constant->kind == XR_SEM_CONST_BIGINT && type->kind == XR_KIND_INSTANCE;
}

/* Arithmetic over BigInts yields another BigInt, which is a heap value like
 * the literals above and takes the same carrier.
 *
 * Which type is BigInt is read from the plan's own constants: a BIGINT
 * constant's type is the BigInt type, by construction. Going through the
 * frozen-builtin roster instead would give the class an id, but that id is
 * also encoded into every canonical type key, so adding one there changes
 * type identity across the whole plan -- measured, it breaks the literal path
 * this predicate sits next to.
 *
 * The operation is named explicitly rather than inferred from "the result type
 * is BigInt": that broader reading also matches RELEASE, whose result_type is
 * the type it frees rather than a value it produces. */
static inline bool
xr_semantic_bigint_arithmetic_is_exact(const XrSemanticPlan *plan,
                                       const XrSemanticOperationRecord *operation) {
    if (!plan || !operation || operation->result_value == XR_SEMANTIC_INDEX_NONE)
        return false;
    switch (operation->opcode) {
        case XI_ADD:
        case XI_SUB:
        case XI_MUL:
        case XI_DIV:
        case XI_MOD:
        case XI_NEG:
            break;
        default:
            return false;
    }
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, operation->result_type);
    if (!type || type->kind != XR_KIND_INSTANCE || (type->flags & XR_SEM_TYPE_NULLABLE) != 0)
        return false;
    size_t constant_count = xr_semantic_plan_constant_count(plan);
    for (size_t i = 0; i < constant_count; i++) {
        const XrSemanticConstantRecord *constant = xr_semantic_plan_constant(plan, (uint32_t) i);
        if (constant && constant->kind == XR_SEM_CONST_BIGINT &&
            constant->type == operation->result_type)
            return true;
    }
    return false;
}

/* Either shape of BigInt value the target plan binds a carrier for. */
static inline bool xr_semantic_bigint_value_is_exact(const XrSemanticPlan *plan,
                                                     const XrSemanticOperationRecord *operation) {
    return xr_semantic_bigint_literal_is_exact(plan, operation) ||
           xr_semantic_bigint_arithmetic_is_exact(plan, operation);
}

#endif /* XR_SEMANTIC_HEAP_LITERAL_SHAPE_H */
