/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_constructor_callable_shape.h - Source callback body authority
 *
 * KEY CONCEPT:
 *   A declaration's effect bit is not a body proof. A concrete callback can
 *   satisfy a polymorphic constructor parameter only through a frozen closure
 *   definition and an independently total scalar body. This judgement grants
 *   no suspension, visibility, ownership, or generic constraint authority.
 */
#ifndef XR_SEMANTIC_CONSTRUCTOR_CALLABLE_SHAPE_H
#define XR_SEMANTIC_CONSTRUCTOR_CALLABLE_SHAPE_H

#include "xr_semantic_function_callable_shape.h"
#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"

static inline bool xr_semantic_callable_scalar_type(const XrSemanticTypeRecord *type) {
    return type && type->flags == 0 && type->child_count == 0 &&
        (type->kind == XR_KIND_INT || type->kind == XR_KIND_FLOAT ||
         type->kind == XR_KIND_BOOL || type->kind == XR_KIND_RUNE);
}

/* Scalar parameters and scalar constants cannot throw a language error.
 * A single return block has no exception edge whose absence must be trusted.
 * Bodies with calls, memory effects, captures or control flow require a
 * different complete judgement and remain outside this admission. */
static inline bool xr_semantic_source_callable_pure_body_is_exact(
    const XrSemanticPlan *plan, uint32_t function_index) {
    const XrSemanticFunctionRecord *function =
        xr_semantic_plan_function(plan, function_index);
    const XrSemanticTypeRecord *callable = function ?
        xr_semantic_plan_type(plan, function->callable_type) : NULL;
    XrSemanticCallableKeyShape shape = {0};
    if (!function || !callable ||
        !xr_semantic_function_callable_shape_is_exact(plan, function) ||
        !xr_semantic_callable_key_parse(callable->canonical_key, &shape) ||
        shape.throw_effect != XR_FN_EFFECT_NO_THROW || shape.receiver_mode != XR_PARAM_READ ||
        shape.generic_parameter_count != 0 || shape.is_variadic || shape.is_c_abi ||
        shape.view_origin_count != 0 || shape.view_origin_was_elided ||
        function->source_kind != XR_SEM_SOURCE_FUNCTION_NONE || function->capture_count != 0 ||
        function->flags != XR_SEM_FUNCTION_NOTHROW || function->effect_complete != 1u ||
        function->unknown_semantic_effects != 0 || function->effect_unknown_reasons != 0 ||
        function->semantic_effects != 0 || function->capability_mask != 0 ||
        function->is_module_initializer || function->is_external_entry ||
        function->carries_coroutine_ops || function->block_count != 1)
        return false;
    const XrSemanticBlockRecord *block =
        xr_semantic_plan_block(plan, function->block_begin);
    if (!block || block->function != function_index || block->kind != XI_BLOCK_RETURN ||
        block->control_value == XR_SEMANTIC_INDEX_NONE ||
        !xr_semantic_callable_scalar_type(xr_semantic_plan_type(plan, function->return_type)))
        return false;
    uint32_t parameters = 0;
    bool return_value = false;
    for (uint32_t o = 0; o < block->operation_count; o++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan, block->operation_begin + o);
        if (!operation || operation->function != function_index ||
            operation->block != function->block_begin || operation->operand_count != 0 ||
            operation->effects != 0 || operation->metadata_count != 0 ||
            operation->flags != xi_generated_op_default_flags((XiOp) operation->opcode) ||
            operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
            operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
            !xr_semantic_callable_scalar_type(xr_semantic_plan_type(plan, operation->result_type)))
            return false;
        if (operation->opcode == XI_PARAM) {
            if (operation->semantic_immediate < 0 ||
                operation->semantic_immediate >= function->parameter_count)
                return false;
            const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(
                plan, function->parameter_begin + (uint32_t) operation->semantic_immediate);
            if (!parameter || parameter->function != function_index ||
                parameter->ordinal != operation->semantic_immediate ||
                parameter->value != operation->result_value || parameter->type != operation->result_type ||
                parameter->mode != XR_PARAM_READ || parameter->ownership != XI_OWN_NONE ||
                parameter->flags != XR_SEM_PARAMETER_REQUIRED)
                return false;
            parameters++;
        } else if (operation->opcode == XI_CONST) {
            const XrSemanticConstantRecord *constant =
                xr_semantic_plan_constant(plan, operation->constant);
            if (!constant || constant->type != operation->result_type)
                return false;
        } else {
            return false;
        }
        if (operation->result_value == block->control_value)
            return_value = operation->result_type == function->return_type;
    }
    return parameters == function->parameter_count && return_value;
}

/* Compare complete signatures with exactly one formal polymorphic effect
 * field. The caller supplies an actual closure definition, never merely a
 * function-valued type or an arbitrary function record with the same name. */
static inline bool xr_semantic_constructor_callback_admits(
    const XrSemanticPlan *actual_plan, const XrSemanticOperationRecord *closure,
    const XrSemanticPlan *formal_plan, uint32_t formal_type) {
    const XrSemanticTypeRecord *actual = closure ?
        xr_semantic_plan_type(actual_plan, closure->result_type) : NULL;
    const XrSemanticTypeRecord *formal = xr_semantic_plan_type(formal_plan, formal_type);
    const XrSemanticFunctionRecord *function = closure ?
        xr_semantic_plan_function(actual_plan, closure->callable_function) : NULL;
    XrSemanticCallableKeyShape actual_shape = {0}, formal_shape = {0};
    if (!actual || !formal || !function || closure->opcode != XI_CLOSURE_NEW ||
        closure->function != function->parent || closure->operand_count != 0 ||
        closure->result_type != function->callable_type ||
        !xr_semantic_callable_key_parse(actual->canonical_key, &actual_shape) ||
        !xr_semantic_callable_key_parse(formal->canonical_key, &formal_shape) ||
        actual_shape.throw_effect != XR_FN_EFFECT_NO_THROW ||
        formal_shape.throw_effect != XR_FN_EFFECT_POLY ||
        !xr_semantic_source_callable_pure_body_is_exact(actual_plan, closure->callable_function))
        return false;
    size_t actual_prefix = (size_t) (actual_shape.throw_field - actual->canonical_key);
    size_t formal_prefix = (size_t) (formal_shape.throw_field - formal->canonical_key);
    return actual_prefix == formal_prefix &&
        memcmp(actual->canonical_key, formal->canonical_key, actual_prefix) == 0 &&
        strcmp(actual_shape.throw_field + 1, formal_shape.throw_field + 1) == 0;
}

static inline bool xr_semantic_constructor_callback_argument_admits(
    const XrSemanticPlan *caller, const XrSemanticOperationRecord *construction,
    const XrSemanticOperandRecord *argument, const XrSemanticPlan *dependency,
    uint32_t formal_type) {
    if (!caller || !construction || !argument || argument->role != XR_SEM_OPERAND_ARGUMENT)
        return false;
    const XrSemanticOperationRecord *definition = NULL;
    for (uint32_t o = 0; o < xr_semantic_plan_operation_count(caller); o++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(caller, o);
        if (!candidate || candidate->result_value != argument->value)
            continue;
        if (definition || candidate->function != construction->function ||
            candidate->result_type != argument->type)
            return false;
        definition = candidate;
    }
    return xr_semantic_constructor_callback_admits(caller, definition, dependency, formal_type);
}

#endif  // XR_SEMANTIC_CONSTRUCTOR_CALLABLE_SHAPE_H
