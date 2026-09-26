/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_source_dependency_call_shape.h - Dependency call declarations
 */
#ifndef XR_SEMANTIC_SOURCE_DEPENDENCY_CALL_SHAPE_H
#define XR_SEMANTIC_SOURCE_DEPENDENCY_CALL_SHAPE_H

#include "xr_semantic_dependency_method_shape.h"
#include "xr_semantic_local_addr_shape.h"
#include "xr_semantic_array_type_shape.h"
#include "xr_semantic_imported_static_method_shape.h"

/* Resolve signature authority, not an executable dispatch decision. Instance
 * methods still require final or complete-graph dispatch authority at the call
 * adapter. All result consumers use the same exact dependency declaration. */
static inline uint32_t xr_semantic_source_dependency_call_function(
    const XrSemanticPlan *caller, const XrSemanticCallTargetRecord *target,
    const XrSemanticPlan *dependency) {
    if (!caller || !target || !dependency || target->function != XR_SEMANTIC_INDEX_NONE ||
        target->reserved[0] != 0 || target->reserved[1] != 0 || target->reserved[2] != 0)
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticDependencyRecord *required =
        xr_semantic_plan_dependency(caller, target->dependency);
    const XrSemanticEntityRecord *module = xr_semantic_plan_unique_module_entity(dependency);
    if (!required || !module || !xr_stable_id_equal(required->module, module->id) ||
        !xr_fingerprint_equal(required->semantic_fingerprint,
                              xr_semantic_plan_fingerprint(dependency)))
        return XR_SEMANTIC_INDEX_NONE;
    if (target->kind == XR_SEM_CALL_TARGET_SOURCE_STATIC_METHOD_DEPENDENCY)
        return xr_semantic_dependency_static_method_is_exact(caller, target, dependency);
    if (target->kind == XR_SEM_CALL_TARGET_SOURCE_METHOD_DEPENDENCY) {
        XrStableId zero = {{0}};
        const XrSemanticSourceMethodRecord *method =
            xr_semantic_dependency_method_is_exact(caller, target, dependency);
        const XrSemanticFunctionRecord *function = method
            ? xr_semantic_plan_function(dependency, method->function) : NULL;
        return method && function && target->source_export == XR_SEMANTIC_INDEX_NONE &&
               xr_stable_id_equal(target->callee_function, zero) &&
               function->source_kind == XR_SEM_SOURCE_FUNCTION_INSTANCE_METHOD &&
               function->source_class == method->source_class &&
               function->parameter_count == method->parameter_count
                   ? method->function : XR_SEMANTIC_INDEX_NONE;
    }
    if (target->kind != XR_SEM_CALL_TARGET_SOURCE_EXPORT ||
        target->callable_type != XR_SEMANTIC_INDEX_NONE)
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticSourceExportRecord *exported =
        xr_semantic_plan_source_export(dependency, target->source_export);
    const XrSemanticFunctionRecord *function =
        exported && exported->kind == XR_SEM_SOURCE_EXPORT_FUNCTION
            ? xr_semantic_plan_function(dependency, exported->function) : NULL;
    return function && xr_stable_id_equal(exported->id, target->export_identity) &&
           xr_stable_id_equal(exported->exported_entity, function->id) &&
           xr_stable_id_equal(target->callee_function, function->id)
               ? exported->function : XR_SEMANTIC_INDEX_NONE;
}

/* An exported ref Array parameter borrows a caller-owned address, not the
 * array value itself. The dependency declaration proves mode and exact type;
 * the caller proves address origin and the call-bounded lifetime. */
static inline bool xr_semantic_source_export_ref_address_is_exact(
    const XrSemanticPlan *caller, const XrSemanticPlan *dependency,
    const XrSemanticCallTargetRecord *target, const XrSemanticOperationRecord *address) {
    if (!target || target->kind != XR_SEM_CALL_TARGET_SOURCE_EXPORT ||
        !xr_semantic_ref_argument_local_addr_is_exact(
            caller, address, address ? address->result_type : XR_SEMANTIC_INDEX_NONE, NULL))
        return false;
    uint32_t function = xr_semantic_source_dependency_call_function(caller, target, dependency);
    const XrSemanticFunctionRecord *callee = xr_semantic_plan_function(dependency, function);
    const XrSemanticOperationRecord *call = xr_semantic_plan_operation(caller, target->operation);
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(caller, address->result_type);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(caller, &operand_count);
    if (!callee || !call || !operands || call->function != address->function ||
        !xr_semantic_array_type_row_is_exact(type) || (type->flags & XR_SEM_TYPE_NULLABLE) != 0 ||
        call->operand_count != (uint32_t) callee->parameter_count + 1u ||
        call->operand_begin > operand_count || call->operand_count > operand_count - call->operand_begin)
        return false;
    for (uint32_t ordinal = 0; ordinal < callee->parameter_count; ordinal++) {
        const XrSemanticOperandRecord *operand = &operands[call->operand_begin + ordinal + 1u];
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(dependency, callee->parameter_begin + ordinal);
        const XrSemanticTypeRecord *parameter_type =
            parameter ? xr_semantic_plan_type(dependency, parameter->type) : NULL;
        if (parameter && parameter_type && parameter->function == function &&
            parameter->ordinal == ordinal && parameter->mode == XR_PARAM_REF &&
            parameter->ownership == XI_OWN_BORROWED && parameter->transfer_mode == XR_TRANSFER_SHARE &&
            xr_stable_id_equal(type->id, parameter_type->id) &&
            operand->value == address->result_value && operand->type == address->result_type &&
            operand->role == XR_SEM_OPERAND_ARGUMENT && operand->parameter == (int16_t) ordinal &&
            operand->parameter_mode == XR_PARAM_REF && operand->access == XR_CALL_ARG_REF &&
            operand->origin == XI_PLACE_ORIGIN_STACK_LOCAL &&
            operand->lifetime == XI_PLACE_LIFETIME_CALL_BOUND && operand->escape == XI_PLACE_ESCAPE_NONE &&
            operand->ownership_action == XR_SEM_OPERAND_BORROW && operand->transfer_mode == XR_TRANSFER_SHARE &&
            operand->flags == (XR_SEM_OPERAND_CALL_CONTRACT | XR_SEM_OPERAND_ADDRESSABLE))
            return true;
    }
    return false;
}

#endif // XR_SEMANTIC_SOURCE_DEPENDENCY_CALL_SHAPE_H
