/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_range_slice_shape.h - Shared exactness judgement for the borrowed
 * view a range slice produces. String slicing is a method call and has its own
 * judgement; this one covers `container[start:end]`, whose result borrows part
 * of a container it did not allocate.
 */

#ifndef XR_SEMANTIC_RANGE_SLICE_SHAPE_H
#define XR_SEMANTIC_RANGE_SLICE_SHAPE_H

#include "xr_semantic_plan.h"
#include "xr_semantic_raw_slice_shape.h"
#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "../../ir/xi_own.h"
#include "../../runtime/value/xtype.h"

/* A borrow view over one exact scalar element. The view itself is a pointer and
 * a length whatever the element is, but the element still has to be exact: its
 * stride is what turns the length into bytes, and a reference-carrying or
 * aggregate element would put a reference-count obligation behind the borrow
 * that no family here discharges. */
static inline bool xr_semantic_slice_view_type_is_exact(const XrSemanticPlan *plan,
                                                        uint32_t type_index,
                                                        uint32_t *out_element_type) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, type_index);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    const uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW;
    const uint8_t allowed = (uint8_t) (required | XR_SEM_TYPE_CONST);
    if (!plan || !type || !children || type->kind != XR_KIND_SLICE ||
        type->builtin_type != XR_TID_NULL || type->scalar_rep != XR_SCALAR_REP_NONE ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 || type->child_count != 1 ||
        type->child_begin >= child_count || (type->flags & required) != required ||
        (type->flags & (uint8_t) ~allowed) != 0)
        return false;
    uint32_t element_type = children[type->child_begin];
    const XrSemanticTypeRecord *element = xr_semantic_plan_type(plan, element_type);
    if (!element || element->builtin_type != XR_TID_NULL || element->flags != 0 ||
        element->child_count != 0 || element->aggregate_extent != 0 ||
        element->aggregate_align != 0 || element->source_class != XR_SEMANTIC_INDEX_NONE)
        return false;
    bool element_is_exact_scalar = false;
    switch (element->kind) {
        case XR_KIND_INT:
            element_is_exact_scalar =
                element->scalar_rep == XR_NATIVE_I8 || element->scalar_rep == XR_NATIVE_U8 ||
                element->scalar_rep == XR_NATIVE_I16 || element->scalar_rep == XR_NATIVE_U16 ||
                element->scalar_rep == XR_NATIVE_I32 || element->scalar_rep == XR_NATIVE_U32 ||
                element->scalar_rep == XR_NATIVE_I64 || element->scalar_rep == XR_NATIVE_U64;
            break;
        case XR_KIND_FLOAT:
            element_is_exact_scalar =
                element->scalar_rep == XR_NATIVE_F32 || element->scalar_rep == XR_NATIVE_F64;
            break;
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
            element_is_exact_scalar = element->scalar_rep == XR_SCALAR_REP_NONE;
            break;
        default:
            element_is_exact_scalar = false;
            break;
    }
    if (!element_is_exact_scalar)
        return false;
    if (out_element_type)
        *out_element_type = element_type;
    return true;
}

/* A ref Slice parameter borrows the native descriptor itself. Its backing
 * allocation remains owned by the caller and is never transferred by this ABI. */
static inline bool xr_semantic_slice_ref_parameter_is_exact(
    const XrSemanticPlan *plan, const XrSemanticParameterRecord *parameter) {
    return plan && parameter && parameter->function < xr_semantic_plan_function_count(plan) &&
           parameter->value != XR_SEMANTIC_INDEX_NONE && parameter->mode == XR_PARAM_REF &&
           parameter->ownership == XI_OWN_BORROWED &&
           parameter->transfer_mode == XR_TRANSFER_SHARE &&
           (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) == 0 && parameter->reserved == 0 &&
           xr_semantic_slice_view_type_is_exact(plan, parameter->type, NULL);
}

/* One judgement for `container[start:end]`. The result borrows the container's
 * own elements, so it allocates nothing and owns nothing, and its two bounds are
 * plain native integers. The container operand is left to the storage family
 * that already bound it: this judgement proves the shape of the view, not which
 * of the three container carriers it was taken from.
 *
 * The builder publishes the view's storage from this judgement, the independent
 * verifier rebuilds from it, and the AOT representation pass re-proves the same
 * relation, so no layer can widen the family on its own. */
/* The bounded roster of operations that hand back a borrowed view. A window, a
 * reinterpretation and a copy each derive one view from another and are views
 * on exactly the terms `container[start:end]` is; listing them here is what
 * keeps "produces a view" from widening into "declares a borrowed result",
 * which many unrelated operations also do. The arity is part of the identity:
 * a row outside the declared count is a different operation. */
static inline bool xr_semantic_slice_view_producer_arity(uint16_t opcode, uint16_t *out_arity) {
    switch (opcode) {
        case XI_SLICE:
        case XI_SLICE_WINDOW:
            *out_arity = 3u;
            return true;
        case XI_SLICE_COPY:
        case XI_BYTE_SLICE_COPY:
            *out_arity = 2u;
            return true;
        case XI_SLICE_REINTERPRET:
        case XI_SLICE_AS_BYTES:
            *out_arity = 1u;
            return true;
        default:
            return false;
    }
}

/* Whether the two trailing operands of a range form are its exact native
 * bounds. Only the two three-operand producers carry them. */
static inline bool xr_semantic_slice_bounds_are_exact(const XrSemanticPlan *plan,
                                                      const XrSemanticOperandRecord *operands,
                                                      uint32_t begin) {
    for (uint16_t i = 1; i < 3u; i++) {
        const XrSemanticOperandRecord *bound = &operands[begin + i];
        const XrSemanticTypeRecord *bound_type = xr_semantic_plan_type(plan, bound->type);
        if (bound->role != XR_SEM_OPERAND_VALUE || bound->parameter != -1 ||
            bound->ownership_action != XR_SEM_OPERAND_BORROW || !bound_type ||
            bound_type->kind != XR_KIND_INT || bound_type->scalar_rep != XR_NATIVE_I64 ||
            bound_type->flags != 0 || bound_type->child_count != 0 ||
            bound_type->builtin_type != XR_TID_NULL)
            return false;
    }
    return true;
}

/* A returned view retains the callee's declared parameter root. The descriptor
 * is a value; its pointee remains borrowed from the exact call argument. */
static inline bool xr_semantic_direct_local_slice_result_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *out_element_type) {
    uint32_t element = XR_SEMANTIC_INDEX_NONE;
    if (!plan || !operation || operation->opcode != XI_CALL ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        !xr_semantic_slice_view_type_is_exact(plan, operation->result_type, &element))
        return false;
    const XrSemanticCallTargetRecord *target = NULL;
    for (uint32_t i = 0; i < xr_semantic_plan_call_target_count(plan); ++i) {
        const XrSemanticCallTargetRecord *candidate = xr_semantic_plan_call_target(plan, i);
        if (!candidate || xr_semantic_plan_operation(plan, candidate->operation) != operation)
            continue;
        if (target || candidate->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL)
            return false;
        target = candidate;
    }
    const XrSemanticFunctionRecord *callee =
        target ? xr_semantic_plan_function(plan, target->function) : NULL;
    if (!callee || callee->return_type != operation->result_type ||
        callee->return_provenance != XR_SEM_RETURN_BORROWED_PARAM ||
        callee->return_parameter < 0 || callee->return_parameter >= callee->parameter_count ||
        operation->return_provenance != callee->return_provenance ||
        operation->return_parameter != callee->return_parameter || !operation->return_complete ||
        operation->operand_count != (uint32_t) callee->parameter_count + 1u)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!operands || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin)
        return false;
    uint32_t ordinal = (uint32_t) callee->return_parameter;
    const XrSemanticParameterRecord *parameter =
        xr_semantic_plan_parameter(plan, callee->parameter_begin + ordinal);
    const XrSemanticOperandRecord *root = &operands[operation->operand_begin + ordinal + 1u];
    uint32_t parameter_element = XR_SEMANTIC_INDEX_NONE;
    uint32_t argument_element = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticTypeRecord *result = xr_semantic_plan_type(plan, operation->result_type);
    const XrSemanticTypeRecord *argument = xr_semantic_plan_type(plan, root->type);
    const XrSemanticTypeRecord *declared =
        parameter ? xr_semantic_plan_type(plan, parameter->type) : NULL;
    /* CALL may consume the non-owning descriptor, never its borrowed root. */
    if (!parameter || parameter->function != target->function || parameter->mode != XR_PARAM_READ ||
        (parameter->ownership != XI_OWN_BORROWED && parameter->ownership != XI_OWN_OWNED) ||
        parameter->transfer_mode != XR_TRANSFER_SHARE ||
        root->role != XR_SEM_OPERAND_ARGUMENT || root->parameter != (int16_t) ordinal ||
        root->parameter_mode != XR_PARAM_READ ||
        (root->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0 ||
        root->ownership_action != (parameter->ownership == XI_OWN_OWNED
                                       ? XR_SEM_OPERAND_CONSUME : XR_SEM_OPERAND_BORROW) ||
        root->transfer_mode != XR_TRANSFER_SHARE || root->access != XR_CALL_ARG_PLAIN ||
        !xr_semantic_slice_view_type_is_exact(plan, parameter->type, &parameter_element) ||
        !xr_semantic_slice_view_type_is_exact(plan, root->type, &argument_element) ||
        element != parameter_element || element != argument_element ||
        (((argument->flags | declared->flags) & XR_SEM_TYPE_CONST) &&
         !(result->flags & XR_SEM_TYPE_CONST)))
        return false;
    if (out_element_type)
        *out_element_type = element;
    return true;
}

static inline bool xr_semantic_range_slice_is_exact(const XrSemanticPlan *plan,
                                                    const XrSemanticOperationRecord *operation,
                                                    uint32_t *out_element_type) {
    XrStableId zero = {{0}};
    uint32_t operand_count = 0;
    uint16_t expected_arity = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const XrSemanticFunctionRecord *function =
        operation ? xr_semantic_plan_function(plan, operation->function) : NULL;
    if (!plan || !operation || !operands || !function ||
        !xr_semantic_slice_view_producer_arity(operation->opcode, &expected_arity) ||
        operation->operand_count != expected_arity || operation->operand_begin >= operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE || operation->metadata_count != 0 ||
        operation->auxiliary_kind != 0 || operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(operation->opcode) ||
        operation->flags != xi_generated_op_default_flags(operation->opcode) ||
        operation->ownership_use != xi_generated_op_own_use(operation->opcode) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_ownership != xi_generated_op_result_ownership(operation->opcode) ||
        operation->transfer_mode != 0 || operation->parameter_mode != 0 ||
        operation->parameter_ownership != 0 || operation->return_parameter != -1 ||
        operation->allocation_key || !xr_stable_id_equal(operation->allocation_id, zero) ||
        operation->result_value < function->value_begin ||
        operation->result_value >= function->value_begin + function->value_count ||
        !xr_semantic_slice_view_type_is_exact(plan, operation->result_type, out_element_type))
        return false;
    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    if (source->role != XR_SEM_OPERAND_VALUE || source->parameter != -1 ||
        source->ownership_action != XR_SEM_OPERAND_BORROW)
        return false;
    /* A reinterpretation names its target element in the immediate; the range
     * forms carry none, and their two bounds have to be exact instead. */
    if (expected_arity == 3u)
        return operation->semantic_immediate == 0 &&
               xr_semantic_slice_bounds_are_exact(plan, operands, operation->operand_begin);
    for (uint16_t i = 1; i < expected_arity; i++) {
        const XrSemanticOperandRecord *rest = &operands[operation->operand_begin + i];
        if (rest->role != XR_SEM_OPERAND_VALUE || rest->parameter != -1 ||
            rest->ownership_action != XR_SEM_OPERAND_BORROW)
            return false;
    }
    return true;
}

static inline bool xr_semantic_slice_ref_load_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *out_element_type) {
    uint32_t count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &count);
    if (!operation || !operands || operation->opcode != XI_PLACE_LOAD ||
        operation->operand_count != 1 || operation->operand_begin >= count ||
        operation->effects != xi_generated_op_effects(XI_PLACE_LOAD) ||
        operation->flags != xi_generated_op_default_flags(XI_PLACE_LOAD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_PLACE_LOAD) ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_PLACE_LOAD) ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE || operation->metadata_count != 0 ||
        operation->semantic_immediate != 0 || operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE || operation->allocation_key ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        !xr_semantic_slice_view_type_is_exact(plan, operation->result_type, out_element_type))
        return false;
    const XrSemanticOperandRecord *place = &operands[operation->operand_begin];
    if (place->type != operation->result_type || place->role != XR_SEM_OPERAND_VALUE ||
        place->parameter != -1 || place->ownership_action != XR_SEM_OPERAND_BORROW ||
        place->access != XR_CALL_ARG_PLAIN || place->flags != 0 ||
        place->parameter_mode != XR_PARAM_READ || place->transfer_mode != XR_TRANSFER_SHARE ||
        place->origin != XI_PLACE_ORIGIN_NONE || place->lifetime != XI_PLACE_LIFETIME_NONE ||
        place->escape != XI_PLACE_ESCAPE_NONE)
        return false;
    for (uint32_t i = 0; i < xr_semantic_plan_parameter_count(plan); i++) {
        const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(plan, i);
        if (parameter && parameter->value == place->value &&
            parameter->type == place->type && parameter->function == operation->function &&
            xr_semantic_slice_ref_parameter_is_exact(plan, parameter))
            return true;
    }
    return false;
}

static inline bool xr_semantic_slice_view_result_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *out_element_type) {
    return xr_semantic_slice_ref_load_is_exact(plan, operation, out_element_type) ||
           xr_semantic_range_slice_is_exact(plan, operation, out_element_type) ||
           xr_semantic_direct_local_slice_result_is_exact(plan, operation, out_element_type) ||
           (xr_semantic_raw_slice_is_exact(plan, operation) &&
            xr_semantic_slice_view_type_is_exact(plan, operation->result_type, out_element_type));
}

#endif  // XR_SEMANTIC_RANGE_SLICE_SHAPE_H
