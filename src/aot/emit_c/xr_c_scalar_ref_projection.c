/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_c_scalar_ref_projection.c - Exact scalar-ref-v1 C projection
 */

#include "xr_c_scalar_ref_projection.h"

#include "../../plan/semantic/xr_semantic_local_addr_shape.h"
#include "../../plan/semantic/xr_semantic_plan.h"
#include "../../runtime/value/xtype.h"

#include <string.h>

static bool scalar_ref_target_rows(const XrTargetPlan *plan,
                                   const XrTargetCallArgumentRecord *argument,
                                   XrCCallArgumentEmissionView *out) {
    uint32_t call_count = 0, slot_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(plan, &call_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
    const XrTargetCallRecord *call =
        argument && argument->call < call_count ? &calls[argument->call] : NULL;
    const XrTargetSlotRecord *caller_slot =
        argument && argument->caller_slot < slot_count ? &slots[argument->caller_slot] : NULL;
    const XrTargetSlotRecord *callee_slot =
        argument && argument->callee_slot < slot_count ? &slots[argument->callee_slot] : NULL;
    const XrTargetMachineRepRecord *caller_register =
        argument ? xr_target_plan_machine_rep(plan, argument->register_rep) : NULL;
    const XrTargetMachineRepRecord *caller_memory =
        argument ? xr_target_plan_machine_rep(plan, argument->memory_rep) : NULL;
    const XrTargetMachineRepRecord *callee_register =
        argument ? xr_target_plan_machine_rep(plan, argument->callee_register_rep) : NULL;
    const XrTargetMachineRepRecord *callee_memory =
        argument ? xr_target_plan_machine_rep(plan, argument->callee_memory_rep) : NULL;
    if (!plan || !argument || !out || !call || !caller_slot || !callee_slot ||
        !caller_register || !caller_memory || !callee_register || !callee_memory ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL ||
        call->target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
        argument->mode != XR_TARGET_CALL_REFERENCE ||
        argument->ownership != XR_TARGET_CALL_BORROW ||
        argument->transfer_mode != XR_TRANSFER_SHARE ||
        argument->flags != XR_TARGET_CALL_ARGUMENT_ADDRESSABLE ||
        argument->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        argument->reserved8[0] != 0 || argument->reserved8[1] != 0 ||
        argument->reserved8[2] != 0 || caller_slot->semantic_value == argument->semantic_value ||
        caller_slot->register_rep != argument->register_rep ||
        caller_slot->memory_rep != argument->memory_rep ||
        caller_slot->function != call->caller_function ||
        callee_slot->semantic_value == argument->semantic_value ||
        callee_slot->function != call->callee_function ||
        callee_slot->role != XR_TARGET_SLOT_PARAMETER ||
        argument->register_rep != argument->callee_register_rep ||
        argument->memory_rep != argument->callee_memory_rep ||
        caller_register->kind != XR_MACHINE_REP_I64 ||
        caller_memory->kind != XR_MACHINE_REP_I64 ||
        callee_register->kind != XR_MACHINE_REP_I64 ||
        callee_memory->kind != XR_MACHINE_REP_I64 ||
        caller_register->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        caller_memory->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        callee_register->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        callee_memory->ownership != XR_TARGET_OWNERSHIP_TRIVIAL)
        return false;
    memset(out, 0, sizeof(*out));
    out->semantic_call_value = call->result_value;
    out->semantic_operand = argument->semantic_operand;
    out->semantic_value = argument->semantic_value;
    out->callee_parameter = argument->callee_parameter;
    out->ordinal = argument->ordinal;
    out->caller_register_kind = caller_register->kind;
    out->caller_memory_kind = caller_memory->kind;
    out->callee_register_kind = callee_register->kind;
    out->callee_memory_kind = callee_memory->kind;
    out->mode = argument->mode;
    out->ownership = argument->ownership;
    out->transfer_mode = argument->transfer_mode;
    out->flags = argument->flags;
    out->array_element_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    out->c_type = "int64_t *";
    return true;
}

static const XrSemanticOperationRecord *unique_result_operation(
    const XrSemanticPlan *semantic, uint32_t result_value) {
    const XrSemanticOperationRecord *match = NULL;
    size_t operation_count = semantic ? xr_semantic_plan_operation_count(semantic) : 0;
    for (size_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate =
            xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->result_value != result_value)
            continue;
        if (match)
            return NULL;
        match = candidate;
    }
    return match;
}

static bool scalar_ref_semantic_rows(const XrTargetPlan *plan,
                                     const XrTargetCallArgumentRecord *argument,
                                     XrCScalarRefProjection *out) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(plan);
    uint32_t call_count = 0, slot_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(plan, &call_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
    const XrTargetCallRecord *call =
        argument && argument->call < call_count ? &calls[argument->call] : NULL;
    const XrSemanticFunctionRecord *function =
        call && semantic ? xr_semantic_plan_function(semantic, call->callee_function) : NULL;
    const XrSemanticParameterRecord *parameter =
        argument && semantic
            ? xr_semantic_plan_parameter(semantic, argument->callee_parameter)
            : NULL;
    const XrSemanticTypeRecord *type =
        parameter ? xr_semantic_plan_type(semantic, parameter->type) : NULL;
    const XrSemanticOperationRecord *address =
        argument ? unique_result_operation(semantic, argument->semantic_value) : NULL;
    const XrSemanticOperandRecord *source = NULL;
    if (!semantic || !argument || !out || !call || !function || !parameter || !type ||
        !address || parameter->function != call->callee_function ||
        parameter->ordinal != argument->ordinal || parameter->mode != XR_PARAM_REF ||
        type->kind != XR_KIND_INT || type->scalar_rep != XR_NATIVE_I64 ||
        type->child_count != 0 || type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->flags != 0 || type->builtin_type != XR_TID_NULL ||
        !xr_semantic_ref_argument_local_addr_is_exact(semantic, address, parameter->type,
                                                      &source) ||
        argument->caller_slot >= slot_count ||
        slots[argument->caller_slot].semantic_value != source->value)
        return false;
    out->source_value = source->value;
    memset(&out->function_abi, 0, sizeof(out->function_abi));
    out->function_abi.semantic_function = call->callee_function;
    out->function_abi.semantic_value = parameter->value;
    out->function_abi.ordinal = (uint16_t) (argument->ordinal + 1u);
    out->function_abi.parameter_count = function->parameter_count;
    out->function_abi.target_register_kind = XR_MACHINE_REP_I64;
    out->function_abi.target_memory_kind = XR_MACHINE_REP_I64;
    out->function_abi.slot_class = XR_C_ABI_SLOT_BORROWED_PLACE;
    out->function_abi.boundary_kind = XR_C_ABI_BOUNDARY_NATIVE;
    out->function_abi.rep = XR_C_VALUE_REP_RAW_PTR;
    out->function_abi.pointee_rep = XR_C_VALUE_REP_I64;
    out->function_abi.aggregate_class = XR_C_ABI_AGGREGATE_NONE;
    out->function_abi.c_type = "int64_t *";
    out->function_abi.pointee_c_type = "int64_t";
    return true;
}

XR_FUNC XrCScalarRefProjectionStatus xr_c_scalar_ref_project_argument(
    const XrTargetPlan *plan, const XrTargetCallArgumentRecord *argument,
    XrCScalarRefProjection *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(plan, &call_count);
    const XrTargetCallRecord *call =
        argument && calls && argument->call < call_count ? &calls[argument->call] : NULL;
    bool claim = call &&
                 (call->target_kind == XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
                  call->calling_convention == XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL) &&
                 argument->mode == XR_TARGET_CALL_REFERENCE &&
                 argument->ownership == XR_TARGET_CALL_BORROW &&
                 argument->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE;
    if (!claim)
        return XR_C_SCALAR_REF_NOT_THIS_FAMILY;
    if (!plan || !argument || !out ||
        !scalar_ref_target_rows(plan, argument, &out->call_argument) ||
        !scalar_ref_semantic_rows(plan, argument, out))
        return XR_C_SCALAR_REF_MALFORMED;
    return XR_C_SCALAR_REF_EXACT;
}

XR_FUNC XrCScalarRefProjectionStatus xr_c_scalar_ref_project_address(
    const XrTargetPlan *plan, const XrTargetValueRepRecord *binding,
    XrCScalarRefProjection *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(plan, binding->memory_rep) : NULL;
    bool claim = register_rep && memory_rep &&
                 register_rep->kind == XR_MACHINE_REP_RAW_PTR &&
                 memory_rep->kind == XR_MACHINE_REP_RAW_PTR;
    if (!claim)
        return XR_C_SCALAR_REF_NOT_THIS_FAMILY;
    if (!plan || !binding || !out || register_rep->register_bits != 64 ||
        register_rep->memory_size != 8 || register_rep->memory_align != 8 ||
        memory_rep->register_bits != 64 || memory_rep->memory_size != 8 ||
        memory_rep->memory_align != 8 ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        register_rep->root_kind != XR_TARGET_ROOT_NONE ||
        memory_rep->root_kind != XR_TARGET_ROOT_NONE)
        return XR_C_SCALAR_REF_MALFORMED;
    uint32_t argument_count = 0;
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(plan, &argument_count);
    const XrTargetCallArgumentRecord *match = NULL;
    for (uint32_t i = 0; arguments && i < argument_count; i++) {
        if (arguments[i].semantic_value != binding->semantic_value)
            continue;
        if (match)
            return XR_C_SCALAR_REF_MALFORMED;
        match = &arguments[i];
    }
    if (!match || xr_c_scalar_ref_project_argument(plan, match, out) !=
                      XR_C_SCALAR_REF_EXACT)
        return XR_C_SCALAR_REF_MALFORMED;
    return XR_C_SCALAR_REF_EXACT;
}

static bool call_views_equal(const XrCCallArgumentEmissionView *a,
                             const XrCCallArgumentEmissionView *b) {
    return a && b && a->semantic_call_value == b->semantic_call_value &&
           a->semantic_operand == b->semantic_operand &&
           a->semantic_value == b->semantic_value &&
           a->callee_parameter == b->callee_parameter && a->ordinal == b->ordinal &&
           a->caller_register_kind == b->caller_register_kind &&
           a->caller_memory_kind == b->caller_memory_kind &&
           a->callee_register_kind == b->callee_register_kind &&
           a->callee_memory_kind == b->callee_memory_kind && a->mode == b->mode &&
           a->ownership == b->ownership && a->transfer_mode == b->transfer_mode &&
           a->flags == b->flags && a->array_element_storage == b->array_element_storage &&
           a->reserved[0] == b->reserved[0] && a->reserved[1] == b->reserved[1] &&
           a->reserved[2] == b->reserved[2] && a->c_type && b->c_type &&
           strcmp(a->c_type, b->c_type) == 0;
}

static bool abi_views_equal(const XrCFunctionAbiEmissionView *a,
                            const XrCFunctionAbiEmissionView *b) {
    return a && b && a->semantic_function == b->semantic_function &&
           a->semantic_value == b->semantic_value && a->ordinal == b->ordinal &&
           a->parameter_count == b->parameter_count &&
           a->target_register_kind == b->target_register_kind &&
           a->target_memory_kind == b->target_memory_kind &&
           a->slot_class == b->slot_class && a->boundary_kind == b->boundary_kind &&
           a->rep == b->rep && a->pointee_rep == b->pointee_rep &&
           a->aggregate_class == b->aggregate_class && a->c_type && b->c_type &&
           strcmp(a->c_type, b->c_type) == 0 && a->pointee_c_type &&
           b->pointee_c_type && strcmp(a->pointee_c_type, b->pointee_c_type) == 0;
}

XR_FUNC bool xr_c_scalar_ref_projection_views_are_exact(
    const XrCScalarRefProjection *projection,
    const XrCCallArgumentEmissionView *call_arguments, uint32_t call_argument_count,
    const XrCFunctionAbiEmissionView *function_abis, uint32_t function_abi_count) {
    if (!projection)
        return false;
    const XrCCallArgumentEmissionView *call = NULL;
    for (uint32_t i = 0; call_arguments && i < call_argument_count; i++) {
        if (call_arguments[i].semantic_call_value !=
                projection->call_argument.semantic_call_value ||
            call_arguments[i].ordinal != projection->call_argument.ordinal)
            continue;
        if (call)
            return false;
        call = &call_arguments[i];
    }
    const XrCFunctionAbiEmissionView *abi = NULL;
    for (uint32_t i = 0; function_abis && i < function_abi_count; i++) {
        if (function_abis[i].semantic_function !=
                projection->function_abi.semantic_function ||
            function_abis[i].ordinal != projection->function_abi.ordinal)
            continue;
        if (abi)
            return false;
        abi = &function_abis[i];
    }
    return call_views_equal(&projection->call_argument, call) &&
           abi_views_equal(&projection->function_abi, abi);
}
