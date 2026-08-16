/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_shared_read_shape.h - Shared judgement for a read of a shared cell
 *
 * KEY CONCEPT:
 *   A variable bound at module scope lives in a shared cell, so every later
 *   mention of it is a read that borrows whatever the cell holds. The read has
 *   one shape whatever it reads: no operands, no constant, no callee, no
 *   intrinsic, and a borrowed result. Which container that shape produces is
 *   the result type's business, and each caller proves that separately.
 *
 *   Both the shape and the "this value is defined exactly once" question are
 *   asked by the target builder, the target verifier and the AOT
 *   representation oracle, so they ask them here rather than each spelling out
 *   its own copy and drifting apart in what they admit.
 */

#ifndef XR_SEMANTIC_SHARED_READ_SHAPE_H
#define XR_SEMANTIC_SHARED_READ_SHAPE_H

#include "xr_semantic_plan.h"
#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include <stdint.h>

/* The borrowed read of a value held in a shared cell, re-derived from the
 * frozen rows. It states nothing about the container it reads and nothing about
 * a call boundary; a caller that needs either proves it on top of this. */
static inline bool
xr_semantic_shared_read_operation_is_exact(const XrSemanticOperationRecord *operation) {
    return operation && operation->opcode == XI_GET_SHARED && operation->operand_count == 0 &&
           operation->effects == xi_generated_op_effects(XI_GET_SHARED) &&
           operation->flags == xi_generated_op_default_flags(XI_GET_SHARED) &&
           operation->ownership_use == xi_generated_op_own_use(XI_GET_SHARED) &&
           operation->result_ownership == xi_generated_op_result_ownership(XI_GET_SHARED) &&
           operation->result_alias_operand == -1 &&
           operation->intrinsic_kind == XR_SEM_INTRINSIC_NONE && operation->metadata_count == 0 &&
           operation->auxiliary_kind == XI_AUX_KIND_NONE &&
           operation->constant == XR_SEMANTIC_INDEX_NONE &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
           operation->semantic_immediate <= UINT32_MAX;
}

/* The one operation that defines a value, or NULL when the plan has none or
 * more than one. A layer that binds storage to a value needs that binding to
 * describe the value's whole life, which a second definition would contradict. */
static inline const XrSemanticOperationRecord *
xr_semantic_unique_value_definition(const XrSemanticPlan *plan, uint32_t semantic_value) {
    const XrSemanticOperationRecord *definition = NULL;
    uint32_t count = (uint32_t) xr_semantic_plan_operation_count(plan);
    if (semantic_value == XR_SEMANTIC_INDEX_NONE)
        return NULL;
    for (uint32_t i = 0; i < count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->result_value != semantic_value)
            continue;
        if (definition)
            return NULL;
        definition = candidate;
    }
    return definition;
}

#endif  // XR_SEMANTIC_SHARED_READ_SHAPE_H
