/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_plain_ref_aggregate_shape.h - Exact local pointer-free aggregate borrows
 *
 * KEY CONCEPT:
 *   Target layouts and physical pointer geometry are independently recomputed
 *   by each consumer; this judgement never reads producer storage records.
 */
#ifndef XR_TARGET_PLAIN_REF_AGGREGATE_SHAPE_H
#define XR_TARGET_PLAIN_REF_AGGREGATE_SHAPE_H

#include "xr_target_scalar_rep_shape.h"
#include "../semantic/xr_semantic_value_aggregate_shape.h"
#include "../semantic/xr_semantic_local_addr_shape.h"
#include "../semantic/xr_semantic_local_call_target_shape.h"

static inline bool xr_target_plain_ref_field_graph(const XrSemanticPlan *semantic,
                                                   uint32_t type_index) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, type_index);
    uint16_t kind = XR_MACHINE_REP_COUNT;
    if (!type)
        return false;
    if (xr_target_scalar_rep_for_type(type, false, &kind) == 1)
        return (type->kind == XR_KIND_INT || type->kind == XR_KIND_FLOAT ||
                type->kind == XR_KIND_BOOL || type->kind == XR_KIND_RUNE) &&
               type->flags == 0 && type->builtin_type == XR_TID_NULL &&
               type->child_count == 0 && type->aggregate_extent == 0 &&
               type->aggregate_align == 0 && kind != XR_MACHINE_REP_VOID;
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    if (!children || type->child_begin > child_count ||
        type->child_count > child_count - type->child_begin)
        return false;
    /* Only scalar fixed-array lanes are proved. Nested arrays, managed
     * fields and nested reference-bearing structures need their own proof. */
    if (type->kind == XR_KIND_FIXED_ARRAY &&
        type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
        type->scalar_rep == XR_SCALAR_REP_NONE && type->child_count == 1 &&
        type->aggregate_extent > 0 && type->aggregate_extent <= UINT16_MAX) {
        const XrSemanticTypeRecord *element =
            xr_semantic_plan_type(semantic, children[type->child_begin]);
        return element && (element->kind == XR_KIND_INT || element->kind == XR_KIND_FLOAT ||
                           element->kind == XR_KIND_BOOL || element->kind == XR_KIND_RUNE) &&
               xr_target_plain_ref_field_graph(semantic, children[type->child_begin]);
    }
    const uint8_t required = XR_SEM_TYPE_VALUE | XR_SEM_TYPE_REFERENCE_CAPABLE |
                             XR_SEM_TYPE_OWNERSHIP_ROOT | XR_SEM_TYPE_AGGREGATE_EXACT;
    XrSemanticValueAggregateShape shape = {0};
    if (type->kind != XR_KIND_INSTANCE || type->flags != required ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->child_count == 0 ||
        type->aggregate_extent != type->child_count ||
        !xr_semantic_value_aggregate_shape_for_type(semantic, type_index, &shape) ||
        shape.semantic_type != type_index || shape.source_class != type->source_class ||
        shape.field_count != type->child_count)
        return false;
    const XrSemanticSourceClassRecord *declaration =
        xr_semantic_plan_source_class(semantic, shape.source_class);
    if (!declaration || !xr_stable_id_equal(declaration->id, type->source_class_identity))
        return false;
    for (uint16_t i = 0; i < type->child_count; i++) {
        const XrSemanticTypeRecord *field =
            xr_semantic_plan_type(semantic, children[type->child_begin + i]);
        if (!field || field->kind == XR_KIND_INSTANCE ||
            !xr_target_plain_ref_field_graph(semantic, children[type->child_begin + i]))
            return false;
    }
    return true;
}

static inline bool xr_target_plain_ref_load_source_is_exact(
    const XrSemanticOperationRecord *load, const XrSemanticOperandRecord *operand,
    uint32_t function, uint32_t type) {
    return load && operand && load->function == function && load->opcode == XI_PLACE_LOAD &&
        load->operand_count == 1 && load->result_type == type &&
        load->metadata_count == 0 && load->semantic_immediate == 0 &&
        load->auxiliary_kind == XI_AUX_KIND_NONE &&
        load->effects == xi_generated_op_effects(XI_PLACE_LOAD) &&
        load->flags == xi_generated_op_default_flags(XI_PLACE_LOAD) &&
        load->ownership_use == xi_generated_op_own_use(XI_PLACE_LOAD) &&
        load->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
        operand->type == type && operand->role == XR_SEM_OPERAND_VALUE &&
        operand->parameter == -1 && operand->parameter_mode == XR_PARAM_READ &&
        operand->ownership_action == XR_SEM_OPERAND_BORROW &&
        operand->transfer_mode == XR_TRANSFER_SHARE && operand->access == XR_CALL_ARG_PLAIN &&
        operand->origin == XI_PLACE_ORIGIN_NONE && operand->lifetime == XI_PLACE_LIFETIME_NONE &&
        operand->escape == XI_PLACE_ESCAPE_NONE && operand->flags == 0;
}

static inline bool xr_target_plain_ref_parameter_source_is_exact(
    const XrSemanticPlan *semantic, const XrSemanticParameterRecord *parameter) {
    const XrSemanticTypeRecord *type =
        parameter ? xr_semantic_plan_type(semantic, parameter->type) : NULL;
    const XrSemanticFunctionRecord *function =
        parameter ? xr_semantic_plan_function(semantic, parameter->function) : NULL;
    if (!semantic || !parameter || !function || !type || type->kind != XR_KIND_INSTANCE ||
        parameter->mode != XR_PARAM_REF || parameter->ownership != XI_OWN_BORROWED ||
        parameter->transfer_mode != XR_TRANSFER_SHARE ||
        (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) != 0 || parameter->reserved != 0 ||
        parameter->value == XR_SEMANTIC_INDEX_NONE ||
        function->carries_coroutine_ops || function->capture_count || function->is_module_initializer ||
        (function->semantic_effects & (XI_EFFECT_MAY_SUSPEND | XI_EFFECT_MAY_THROW)) ||
        (function->flags & (XR_SEM_FUNCTION_GENERATOR | XR_SEM_FUNCTION_EXTERN)) ||
        parameter->ordinal >= function->parameter_count ||
        xr_semantic_plan_parameter(semantic, function->parameter_begin + parameter->ordinal) != parameter ||
        !xr_target_plain_ref_field_graph(semantic, parameter->type))
        return false;
    const XrSemanticOperationRecord *definition = NULL;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (!operation)
            return false;
        if (operation->function != parameter->function) {
            if (operation->operand_begin > operand_count ||
                operation->operand_count > operand_count - operation->operand_begin)
                return false;
            for (uint16_t n = 0; n < operation->operand_count; n++)
                if (operands[operation->operand_begin + n].value == parameter->value)
                    return false;
            continue;
        }
        if (operation->effects & (XI_EFFECT_MEMORY_WRITE | XI_EFFECT_MAY_SUSPEND))
            return false;
        if (operation->result_value == parameter->value) {
            if (definition || operation->opcode != XI_PARAM ||
                operation->result_type != parameter->type || operation->operand_count != 0 ||
                operation->parameter_mode != parameter->mode ||
                operation->parameter_ownership != parameter->ownership ||
                operation->transfer_mode != parameter->transfer_mode ||
                operation->semantic_immediate != parameter->ordinal)
                return false;
            definition = operation;
        }
        if (operation->operand_begin > operand_count ||
            operation->operand_count > operand_count - operation->operand_begin)
            return false;
        for (uint16_t n = 0; n < operation->operand_count; n++)
            if (operands[operation->operand_begin + n].value == parameter->value &&
                (n != 0 || !xr_target_plain_ref_load_source_is_exact(operation,
                    &operands[operation->operand_begin + n], parameter->function, parameter->type)))
                return false;
    }
    return definition != NULL;
}

static inline bool xr_target_plain_ref_call_source_is_exact(
    const XrSemanticPlan *semantic, const XrSemanticCallTargetRecord *target,
    uint16_t ordinal, const XrSemanticOperandRecord **out_source) {
    if (out_source)
        *out_source = NULL;
    const XrSemanticOperationRecord *call =
        target ? xr_semantic_plan_operation(semantic, target->operation) : NULL;
    const XrSemanticFunctionRecord *callee =
        target ? xr_semantic_plan_function(semantic, target->function) : NULL;
    const XrSemanticFunctionRecord *caller =
        call ? xr_semantic_plan_function(semantic, call->function) : NULL;
    const XrSemanticParameterRecord *parameter = callee && ordinal < callee->parameter_count
        ? xr_semantic_plan_parameter(semantic, callee->parameter_begin + ordinal) : NULL;
    uint32_t count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &count);
    if (!semantic || !target || target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL ||
        !call || call->opcode != XI_CALL || !callee || !caller || !operands ||
        caller->carries_coroutine_ops || (caller->semantic_effects & XI_EFFECT_MAY_SUSPEND) ||
        !xr_semantic_call_target_names_local_function(target, call,
            (uint32_t) xr_semantic_plan_function_count(semantic)) ||
        !xr_target_plain_ref_parameter_source_is_exact(semantic, parameter) ||
        call->operand_count != (uint32_t) callee->parameter_count + 1u ||
        call->operand_begin > count || call->operand_count > count - call->operand_begin)
        return false;
    const XrSemanticOperandRecord *operand = &operands[call->operand_begin + ordinal + 1u];
    if (operand->type != parameter->type || operand->role != XR_SEM_OPERAND_ARGUMENT ||
        operand->parameter != (int16_t) ordinal || operand->parameter_mode != XR_PARAM_REF ||
        operand->access != XR_CALL_ARG_REF || operand->origin != XI_PLACE_ORIGIN_STACK_LOCAL ||
        operand->lifetime != XI_PLACE_LIFETIME_CALL_BOUND || operand->escape != XI_PLACE_ESCAPE_NONE ||
        operand->ownership_action != XR_SEM_OPERAND_BORROW || operand->transfer_mode != XR_TRANSFER_SHARE ||
        operand->flags != (XR_SEM_OPERAND_CALL_CONTRACT | XR_SEM_OPERAND_ADDRESSABLE))
        return false;
    const XrSemanticOperationRecord *address = NULL;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *definition = xr_semantic_plan_operation(semantic, i);
        if (definition && definition->result_value == operand->value) {
            if (address || definition->function != call->function)
                return false;
            address = definition;
        }
    }
    if (!xr_semantic_ref_argument_local_addr_is_exact(semantic, address, parameter->type, out_source))
        return false;
    /* A loaded value is a pointer-free copy. The original local address can
     * reach only proved calls and loads; return, store and forwarding reject. */
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *use = xr_semantic_plan_operation(semantic, i);
        if (!use || use->operand_begin > count || use->operand_count > count - use->operand_begin)
            return false;
        for (uint16_t n = 0; n < use->operand_count; n++) {
            const XrSemanticOperandRecord *used = &operands[use->operand_begin + n];
            if (used->value != address->result_value)
                continue;
            if (n == 0 && xr_target_plain_ref_load_source_is_exact(use, used, call->function, parameter->type))
                continue;
            if (use->opcode != XI_CALL || use->function != call->function || n == 0 ||
                used->type != parameter->type || used->role != XR_SEM_OPERAND_ARGUMENT ||
                used->parameter != (int16_t) (n - 1u) || used->parameter_mode != XR_PARAM_REF ||
                used->access != XR_CALL_ARG_REF || used->origin != XI_PLACE_ORIGIN_STACK_LOCAL ||
                used->lifetime != XI_PLACE_LIFETIME_CALL_BOUND || used->escape != XI_PLACE_ESCAPE_NONE ||
                used->ownership_action != XR_SEM_OPERAND_BORROW || used->transfer_mode != XR_TRANSFER_SHARE ||
                used->flags != (XR_SEM_OPERAND_CALL_CONTRACT | XR_SEM_OPERAND_ADDRESSABLE))
                return false;
            const XrSemanticCallTargetRecord *use_target = NULL;
            for (uint32_t c = 0; c < xr_semantic_plan_call_target_count(semantic); c++) {
                const XrSemanticCallTargetRecord *candidate = xr_semantic_plan_call_target(semantic, c);
                if (candidate && candidate->operation == i) {
                    if (use_target)
                        return false;
                    use_target = candidate;
                }
            }
            const XrSemanticFunctionRecord *use_callee = use_target
                ? xr_semantic_plan_function(semantic, use_target->function) : NULL;
            const XrSemanticParameterRecord *use_parameter = use_callee && n - 1u < use_callee->parameter_count
                ? xr_semantic_plan_parameter(semantic, use_callee->parameter_begin + n - 1u) : NULL;
            if (!use_target || use_target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL ||
                !use_callee || !use_parameter || use->operand_count != use_callee->parameter_count + 1u ||
                use_parameter->type != parameter->type ||
                !xr_semantic_call_target_names_local_function(use_target, use,
                    (uint32_t) xr_semantic_plan_function_count(semantic)) ||
                !xr_target_plain_ref_parameter_source_is_exact(semantic, use_parameter))
                return false;
        }
    }
    return true;
}

static inline bool xr_target_plain_ref_address_source_is_exact(
    const XrSemanticPlan *semantic, const XrSemanticOperationRecord *address) {
    if (!address || address->opcode != XI_LOCAL_ADDR)
        return false;
    uint32_t count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &count);
    if (!operands)
        return false;
    for (uint32_t i = 0; i < xr_semantic_plan_call_target_count(semantic); i++) {
        const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(semantic, i);
        const XrSemanticOperationRecord *call =
            target ? xr_semantic_plan_operation(semantic, target->operation) : NULL;
        const XrSemanticFunctionRecord *callee =
            target ? xr_semantic_plan_function(semantic, target->function) : NULL;
        if (!call || !callee || call->operand_begin > count ||
            call->operand_count > count - call->operand_begin)
            continue;
        for (uint16_t ordinal = 0; ordinal < callee->parameter_count; ordinal++)
            if (call->operand_count > (uint32_t) ordinal + 1u &&
                operands[call->operand_begin + ordinal + 1u].value == address->result_value &&
                xr_target_plain_ref_call_source_is_exact(semantic, target, ordinal, NULL))
                return true;
    }
    return false;
}

#endif // XR_TARGET_PLAIN_REF_AGGREGATE_SHAPE_H
