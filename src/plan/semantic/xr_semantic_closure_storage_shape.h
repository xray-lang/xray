/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Frozen callable storage for owned closures and copied local captures.
 */
#ifndef XR_SEMANTIC_CLOSURE_STORAGE_SHAPE_H
#define XR_SEMANTIC_CLOSURE_STORAGE_SHAPE_H
#include "xr_semantic_dynamic_value_shape.h"
#include "xr_semantic_allocation_shape.h"

static inline bool xr_semantic_owned_closure_is_exact(const XrSemanticPlan *plan,
                                           const XrSemanticOperationRecord *operation) {
    if (!plan || !operation ||
        (operation->opcode != XI_CLOSURE_NEW &&
         (operation->opcode != XI_STACK_ALLOC || operation->semantic_immediate != XI_CLOSURE_NEW)) ||
        operation->callable_function >= xr_semantic_plan_function_count(plan) ||
        !xr_semantic_allocation_identity_is_canonical(operation) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED)
        return false;
    const XrSemanticFunctionRecord *callee =
        xr_semantic_plan_function(plan, operation->callable_function);
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, operation->result_type);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    bool typed_function = type && type->kind == XR_KIND_FUNCTION;
    bool opaque_closure = type && type->kind == XR_KIND_UNKNOWN && type->child_count == 0;
    if (!callee || !type || callee->parent != operation->function ||
        (!typed_function && !opaque_closure) || type->aggregate_extent != 0 ||
        type->aggregate_align != 0 ||
        (type->flags & (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE | XR_SEM_TYPE_BORROW_VIEW |
                        XR_SEM_TYPE_AGGREGATE_EXACT)) != 0 ||
        (type->flags & (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT)) !=
            (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        callee->parameter_count == UINT16_MAX ||
        (typed_function && type->child_count != (uint32_t) callee->parameter_count + 1u) ||
        type->child_begin > child_count || type->child_count > child_count - type->child_begin ||
        callee->parameter_begin > xr_semantic_plan_parameter_count(plan) ||
        callee->parameter_count > xr_semantic_plan_parameter_count(plan) - callee->parameter_begin)
        return false;
    for (uint32_t i = 0; i < callee->parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(plan, callee->parameter_begin + i);
        if (!parameter || parameter->function != operation->callable_function ||
            parameter->ordinal != i ||
            (typed_function && children[type->child_begin + i] != parameter->type))
            return false;
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (operation->operand_count != callee->capture_count ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        (operation->operand_count != 0 && !operands) ||
        callee->capture_begin > xr_semantic_plan_capture_count(plan) ||
        callee->capture_count > xr_semantic_plan_capture_count(plan) - callee->capture_begin)
        return false;
    for (uint32_t i = 0; i < callee->capture_count; ++i) {
        const XrSemanticCaptureRecord *capture =
            xr_semantic_plan_capture(plan, callee->capture_begin + i);
        const XrSemanticOperandRecord *operand = &operands[operation->operand_begin + i];
        if (!capture || operand->value != capture->source_value || operand->type != capture->source_type ||
            operand->role != XR_SEM_OPERAND_VALUE || operand->parameter != -1 ||
            operand->ownership_action != XR_SEM_OPERAND_CONSUME ||
            operand->transfer_mode != XR_TRANSFER_SHARE || operand->flags != 0 ||
            capture->function != operation->callable_function ||
            capture->ordinal != i || capture->source_function != operation->function ||
            capture->source != XR_SEM_CAPTURE_LOCAL_VALUE || capture->kind != XR_SEM_CAPTURE_BY_COPY ||
            capture->flags != 0 || capture->reserved[0] != 0 || capture->type != capture->source_type ||
            capture->source_capture != XR_SEMANTIC_INDEX_NONE ||
            !xr_semantic_reference_phi_input_is_exact(
                plan, capture->source_value, capture->source_type, operation->function))
            return false;
    }
    return opaque_closure ||
           children[type->child_begin + callee->parameter_count] == callee->return_type;
}

#endif
