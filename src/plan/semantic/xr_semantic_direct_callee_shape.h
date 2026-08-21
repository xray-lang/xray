/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_direct_callee_shape.h - A shared read that names a local callee
 */

#ifndef XR_SEMANTIC_DIRECT_CALLEE_SHAPE_H
#define XR_SEMANTIC_DIRECT_CALLEE_SHAPE_H

#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "xr_semantic_plan.h"

/* Whether a shared-slot read names one local function exactly, so a call
 * through it can be emitted as a direct call rather than an indirect one.
 *
 * The builder decides it, the plan verifier re-checks it, and the AOT
 * refinement checks it a third time. All three used to carry their own copy of
 * this judgement, and all three carried the same defect: they accept a function
 * type or the compiler's untyped one, then require the type to have no
 * children. A function type states its signature in children -- only the
 * untyped spelling has none -- so the child test narrowed every copy to the
 * untyped case, which is what a module-level function is bound as today. */
static inline bool
xr_semantic_direct_local_callee_type_is_exact(const XrSemanticPlan *plan,
                                              const XrSemanticOperationRecord *operation,
                                              uint32_t target_function) {
    if (!plan || !operation || operation->opcode != XI_GET_SHARED ||
        operation->semantic_immediate < 0 || operation->semantic_immediate > UINT16_MAX ||
        operation->operand_count != 0 || operation->allocation_key ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_GET_SHARED) ||
        operation->effects != xi_generated_op_effects(XI_GET_SHARED) ||
        operation->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        operation->return_complete != 1 || operation->return_parameter != -1 ||
        target_function >= xr_semantic_plan_function_count(plan))
        return false;
    for (uint32_t i = 0; i < XR_STABLE_ID_BYTES; i++)
        if (operation->allocation_id.bytes[i] != 0)
            return false;
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, operation->result_type);
    const XrSemanticFunctionRecord *target = xr_semantic_plan_function(plan, target_function);
    uint32_t lexical_owner = target ? target->parent : XR_SEMANTIC_INDEX_NONE;
    uint32_t caller_ancestor = operation->function;
    for (uint32_t depth = 0;
         caller_ancestor != XR_SEMANTIC_INDEX_NONE && caller_ancestor != lexical_owner &&
         depth < xr_semantic_plan_function_count(plan);
         depth++) {
        const XrSemanticFunctionRecord *ancestor = xr_semantic_plan_function(plan, caller_ancestor);
        caller_ancestor = ancestor ? ancestor->parent : XR_SEMANTIC_INDEX_NONE;
    }
    if (!type || !target || lexical_owner == XR_SEMANTIC_INDEX_NONE ||
        caller_ancestor != lexical_owner ||
        (type->kind != XR_KIND_FUNCTION && type->kind != XR_KIND_UNKNOWN) ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->aggregate_extent != 0 ||
        type->aggregate_align != 0 ||
        /* A function type states its signature in children; the untyped
         * spelling has none. Requiring zero children of both admits only the
         * untyped one, which is what a module-level function used to be bound
         * as. */
        (type->kind == XR_KIND_UNKNOWN && type->child_count != 0) ||
        target->parameter_begin > xr_semantic_plan_parameter_count(plan) ||
        target->parameter_count >
            xr_semantic_plan_parameter_count(plan) - target->parameter_begin ||
        (type->flags & (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE | XR_SEM_TYPE_BORROW_VIEW |
                        XR_SEM_TYPE_AGGREGATE_EXACT)) != 0 ||
        (type->flags & (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT)) !=
            (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT)) {
        return false;
    }
    return true;
}

#endif /* XR_SEMANTIC_DIRECT_CALLEE_SHAPE_H */
