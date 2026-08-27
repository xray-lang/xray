/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_aot_scalar_ref_v1.c - Direct-local scalar ref refinement authority
 */

#include "xr_aot_scalar_ref_v1.h"

#include "../../ir/xi_ops_gen.h"
#include "../../ir/xi_own.h"
#include "../../plan/semantic/xr_semantic_ids.h"
#include "../../plan/semantic/xr_semantic_local_addr_shape.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>

typedef struct ScalarRefClaim {
    const XrSemanticOperationRecord *call;
    const XrSemanticOperationRecord *address;
    const XrSemanticOperandRecord *source;
    const XrSemanticCallTargetRecord *semantic_target;
    const XrSemanticParameterRecord *parameter;
    uint32_t call_index;
    uint32_t address_index;
    uint32_t semantic_target_index;
    uint32_t parameter_index;
    uint16_t operand_index;
    uint16_t ordinal;
} ScalarRefClaim;

static bool stable_id_is_zero(XrStableId id) {
    for (uint32_t i = 0; i < XR_STABLE_ID_BYTES; i++)
        if (id.bytes[i] != 0)
            return false;
    return true;
}

static bool scalar_ref_identity(XrStableId target, XrStableId parameter,
                                uint16_t ordinal, XrStableId *out) {
    char target_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char parameter_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char key[192];
    XrFingerprint digest;
    xr_stable_id_hex(target, target_hex);
    xr_stable_id_hex(parameter, parameter_hex);
    int written = snprintf(
        key, sizeof(key),
        "xray-target-direct-scalar-ref-argument-v1:first=%s:second=%s:ordinal=%u",
        target_hex, parameter_hex, ordinal);
    return out && written > 0 && (size_t) written < sizeof(key) &&
           xr_stable_id_from_key(key, out, &digest);
}

static bool i64_type_is_exact(const XrSemanticTypeRecord *type) {
    return type && type->kind == XR_KIND_INT &&
           type->scalar_rep == XR_NATIVE_I64 && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->flags == 0 && type->builtin_type == XR_TID_NULL;
}

static bool authority_is_ordinary_and_intact(const XrSemanticPlan *semantic,
                                             const XrTargetPlan *target) {
    return semantic && target && xr_target_plan_is_frozen(target) &&
           xr_target_plan_is_verified(target) &&
           xr_target_plan_fingerprint_is_intact(target) &&
           xr_target_plan_program_module_count(target) == 1 &&
           xr_target_plan_semantic_plan(target) == semantic &&
           xr_target_plan_program_module(target, 0) == semantic;
}

static const XrSemanticParameterRecord *parameter_for_value(
    const XrSemanticPlan *semantic, uint32_t value, uint32_t *out_index) {
    const XrSemanticParameterRecord *match = NULL;
    uint32_t count = (uint32_t) xr_semantic_plan_parameter_count(semantic);
    for (uint32_t i = 0; i < count; i++) {
        const XrSemanticParameterRecord *candidate =
            xr_semantic_plan_parameter(semantic, i);
        if (!candidate || candidate->value != value)
            continue;
        if (match)
            return NULL;
        match = candidate;
        if (out_index)
            *out_index = i;
    }
    return match;
}

static const XrSemanticOperationRecord *operation_for_value(
    const XrSemanticPlan *semantic, uint32_t value, uint32_t *out_index) {
    const XrSemanticOperationRecord *match = NULL;
    uint32_t count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    for (uint32_t i = 0; i < count; i++) {
        const XrSemanticOperationRecord *candidate =
            xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->result_value != value ||
            candidate->opcode == XI_PARAM)
            continue;
        if (match)
            return NULL;
        match = candidate;
        if (out_index)
            *out_index = i;
    }
    return match;
}

static bool parameter_is_exact(const XrSemanticPlan *semantic,
                               const XrSemanticParameterRecord *parameter) {
    const XrSemanticFunctionRecord *function =
        parameter ? xr_semantic_plan_function(semantic, parameter->function) : NULL;
    const XrSemanticTypeRecord *type =
        parameter ? xr_semantic_plan_type(semantic, parameter->type) : NULL;
    return parameter && function && i64_type_is_exact(type) &&
           parameter->ordinal < function->parameter_count &&
           function->parameter_begin + parameter->ordinal <
               xr_semantic_plan_parameter_count(semantic) &&
           xr_semantic_plan_parameter(
               semantic, function->parameter_begin + parameter->ordinal) == parameter &&
           parameter->mode == XR_PARAM_REF && parameter->ownership == XI_OWN_NONE &&
           parameter->transfer_mode == XR_TRANSFER_SHARE &&
           (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) == 0 &&
           parameter->reserved == 0;
}

static bool parameter_is_scalar_ref(const XrSemanticPlan *semantic,
                                    const XrSemanticParameterRecord *parameter) {
    return parameter && parameter->mode == XR_PARAM_REF &&
           i64_type_is_exact(xr_semantic_plan_type(semantic, parameter->type));
}

static XrAotScalarRefV1Status semantic_claim(
    const XrSemanticPlan *semantic, uint32_t operation_index,
    uint16_t operand_index, uint32_t source_value, ScalarRefClaim *out) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(semantic, operation_index);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    if (!operation || !operands ||
        (operation->opcode != XI_CALL && operation->opcode != XI_TAIL_CALL) ||
        operand_index == 0 || operand_index >= operation->operand_count ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin)
        return XR_AOT_SCALAR_REF_V1_UNRELATED;
    const XrSemanticCallTargetRecord *target = NULL;
    uint32_t target_index = XR_SEMANTIC_INDEX_NONE;
    uint32_t target_count =
        (uint32_t) xr_semantic_plan_call_target_count(semantic);
    for (uint32_t i = 0; i < target_count; i++) {
        const XrSemanticCallTargetRecord *candidate =
            xr_semantic_plan_call_target(semantic, i);
        if (!candidate || candidate->operation != operation_index)
            continue;
        if (target)
            return XR_AOT_SCALAR_REF_V1_INVALID;
        target = candidate;
        target_index = i;
    }
    uint16_t ordinal = (uint16_t) (operand_index - 1u);
    const XrSemanticFunctionRecord *callee =
        target ? xr_semantic_plan_function(semantic, target->function) : NULL;
    uint32_t parameter_index =
        callee && ordinal < callee->parameter_count
            ? callee->parameter_begin + ordinal
            : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticParameterRecord *parameter =
        xr_semantic_plan_parameter(semantic, parameter_index);
    if (!target || target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL || !callee ||
        !parameter_is_scalar_ref(semantic, parameter))
        return XR_AOT_SCALAR_REF_V1_UNRELATED;
    const XrSemanticOperandRecord *argument =
        &operands[operation->operand_begin + operand_index];
    uint32_t address_index = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *address =
        operation_for_value(semantic, source_value, &address_index);
    const XrSemanticOperandRecord *source = NULL;
    bool exact = parameter_is_exact(semantic, parameter) &&
                 target->dependency == XR_SEMANTIC_INDEX_NONE &&
                 target->source_export == XR_SEMANTIC_INDEX_NONE &&
                 stable_id_is_zero(target->export_identity) &&
                 stable_id_is_zero(target->callee_function) &&
                 argument->value == source_value &&
                 argument->type == parameter->type &&
                 argument->role == XR_SEM_OPERAND_ARGUMENT &&
                 argument->parameter == (int16_t) ordinal &&
                 argument->parameter_mode == XR_PARAM_REF &&
                 argument->access == XR_CALL_ARG_REF &&
                 argument->origin != XI_PLACE_ORIGIN_NONE &&
                 argument->lifetime == XI_PLACE_LIFETIME_CALL_BOUND &&
                 argument->escape == XI_PLACE_ESCAPE_NONE &&
                 argument->ownership_action == XR_SEM_OPERAND_BORROW &&
                 argument->transfer_mode == XR_TRANSFER_SHARE &&
                 argument->flags ==
                     (XR_SEM_OPERAND_CALL_CONTRACT |
                      XR_SEM_OPERAND_ADDRESSABLE) &&
                 address && address->function == operation->function &&
                 address->result_value == source_value &&
                 xr_semantic_ref_argument_local_addr_is_exact(
                     semantic, address, parameter->type, &source);
    if (!exact)
        return XR_AOT_SCALAR_REF_V1_INVALID;
    if (out)
        *out = (ScalarRefClaim) {
            .call = operation,
            .address = address,
            .source = source,
            .semantic_target = target,
            .parameter = parameter,
            .call_index = operation_index,
            .address_index = address_index,
            .semantic_target_index = target_index,
            .parameter_index = parameter_index,
            .operand_index = operand_index,
            .ordinal = ordinal,
        };
    return XR_AOT_SCALAR_REF_V1_EXACT;
}

static bool value_slot_is_exact(
    const XrTargetPlan *target, uint32_t semantic_value, uint32_t function,
    uint32_t semantic_operation, uint8_t role, uint16_t machine_kind) {
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(target, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(target, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(target, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(target, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && slots && binding->slot < slot_count
            ? &slots[binding->slot]
            : NULL;
    return binding && register_rep && memory_rep && slot &&
           binding->semantic_value == semantic_value &&
           register_rep->id == binding->register_rep &&
           memory_rep->id == binding->memory_rep &&
           register_rep->kind == machine_kind && memory_rep->kind == machine_kind &&
           register_rep->root_kind == XR_TARGET_ROOT_NONE &&
           memory_rep->root_kind == XR_TARGET_ROOT_NONE &&
           register_rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
           memory_rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
           slot->id == binding->slot && slot->function == function &&
           slot->semantic_value == semantic_value &&
           slot->semantic_operation == semantic_operation && slot->role == role &&
           slot->register_rep == binding->register_rep &&
           slot->memory_rep == binding->memory_rep &&
           slot->root_kind == XR_TARGET_ROOT_NONE &&
           slot->ownership == XR_TARGET_OWNERSHIP_TRIVIAL && slot->reserved == 0 &&
           slot->size == memory_rep->memory_size &&
           slot->align == memory_rep->memory_align;
}

static bool source_value_slot_is_exact(
    const XrSemanticPlan *semantic, const XrTargetPlan *target,
    uint32_t semantic_value, uint32_t function, uint32_t type,
    const XrTargetValueRepRecord **out) {
    const XrSemanticParameterRecord *parameter =
        parameter_for_value(semantic, semantic_value, NULL);
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *operation =
        operation_for_value(semantic, semantic_value, &operation_index);
    uint8_t role = parameter ? XR_TARGET_SLOT_PARAMETER : XR_TARGET_SLOT_TEMPORARY;
    uint32_t semantic_operation = parameter ? XR_SEMANTIC_INDEX_NONE : operation_index;
    bool exact_source =
        (parameter && !operation && parameter->function == function &&
         parameter->type == type && parameter->mode == XR_PARAM_READ &&
         parameter->ownership == XI_OWN_NONE &&
         parameter->transfer_mode == XR_TRANSFER_SHARE) ||
        (!parameter && operation && operation->function == function &&
         operation->result_type == type && operation->result_value == semantic_value);
    const XrTargetValueRepRecord *binding =
        exact_source ? xr_target_plan_value_rep(target, semantic_value) : NULL;
    if (!exact_source ||
        !value_slot_is_exact(target, semantic_value, function, semantic_operation, role,
                             XR_MACHINE_REP_I64))
        return false;
    if (out)
        *out = binding;
    return true;
}

static bool target_argument_is_exact(const XrSemanticPlan *semantic,
                                     const XrTargetPlan *target,
                                     const ScalarRefClaim *claim) {
    uint32_t call_count = 0, argument_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target, &call_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(target, &argument_count);
    const XrTargetCallRecord *call = NULL;
    uint32_t target_call_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        if (calls[i].semantic_operation != claim->call_index)
            continue;
        if (call)
            return false;
        call = &calls[i];
        target_call_index = i;
    }
    const XrTargetCallArgumentRecord *argument =
        call && arguments && call->argument_begin <= argument_count &&
                call->argument_count <= argument_count - call->argument_begin &&
                claim->ordinal < call->argument_count
            ? &arguments[call->argument_begin + claim->ordinal]
            : NULL;
    const XrTargetValueRepRecord *caller = NULL;
    const XrTargetValueRepRecord *callee =
        xr_target_plan_value_rep(target, claim->parameter->value);
    const XrTargetValueRepRecord *address =
        xr_target_plan_value_rep(target, claim->address->result_value);
    if (!call || !argument || call->id != target_call_index ||
        call->semantic_call_target != claim->semantic_target_index ||
        call->caller_function != claim->call->function ||
        call->callee_function != claim->parameter->function ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL ||
        call->target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
        call->adapter_count != 0 ||
        !source_value_slot_is_exact(semantic, target, claim->source->value,
                                    call->caller_function, claim->source->type,
                                    &caller) ||
        !value_slot_is_exact(target, claim->parameter->value,
                             call->callee_function, XR_SEMANTIC_INDEX_NONE,
                             XR_TARGET_SLOT_PARAMETER, XR_MACHINE_REP_I64) ||
        !value_slot_is_exact(target, claim->address->result_value,
                             call->caller_function,
                             claim->address_index,
                             XR_TARGET_SLOT_TEMPORARY, XR_MACHINE_REP_RAW_PTR))
        return false;
    XrStableId expected;
    return address && address->slot != caller->slot &&
           scalar_ref_identity(claim->semantic_target->id, claim->parameter->id,
                               claim->ordinal, &expected) &&
           xr_stable_id_equal(argument->identity, expected) &&
           argument->call == call->id &&
           argument->semantic_operand ==
               claim->call->operand_begin + claim->operand_index &&
           argument->semantic_value == claim->address->result_value &&
           argument->callee_parameter == claim->parameter_index &&
           argument->caller_slot == caller->slot &&
           argument->callee_slot == callee->slot &&
           argument->register_rep == caller->register_rep &&
           argument->memory_rep == caller->memory_rep &&
           argument->callee_register_rep == callee->register_rep &&
           argument->callee_memory_rep == callee->memory_rep &&
           argument->ordinal == claim->ordinal &&
           argument->mode == XR_TARGET_CALL_REFERENCE &&
           argument->ownership == XR_TARGET_CALL_BORROW &&
           argument->transfer_mode == XR_TRANSFER_SHARE &&
           argument->flags == XR_TARGET_CALL_ARGUMENT_ADDRESSABLE &&
           argument->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
           argument->reserved8[0] == 0 && argument->reserved8[1] == 0 &&
           argument->reserved8[2] == 0;
}

static bool parameter_calls_are_exact(const XrSemanticPlan *semantic,
                                      const XrTargetPlan *target,
                                      uint32_t parameter_index) {
    const XrSemanticParameterRecord *parameter =
        xr_semantic_plan_parameter(semantic, parameter_index);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t matches = 0;
    uint32_t target_count =
        (uint32_t) xr_semantic_plan_call_target_count(semantic);
    for (uint32_t i = 0; parameter && operands && i < target_count; i++) {
        const XrSemanticCallTargetRecord *semantic_target =
            xr_semantic_plan_call_target(semantic, i);
        if (!semantic_target || semantic_target->function != parameter->function)
            continue;
        const XrSemanticOperationRecord *call =
            xr_semantic_plan_operation(semantic, semantic_target->operation);
        uint16_t operand_index = (uint16_t) (parameter->ordinal + 1u);
        uint32_t value =
            call && operand_index < call->operand_count &&
                    call->operand_begin <= operand_count &&
                    call->operand_count <= operand_count - call->operand_begin
                ? operands[call->operand_begin + operand_index].value
                : XR_SEMANTIC_INDEX_NONE;
        ScalarRefClaim claim = {0};
        if (semantic_claim(semantic, semantic_target->operation, operand_index,
                           value, &claim) != XR_AOT_SCALAR_REF_V1_EXACT ||
            claim.semantic_target_index != i ||
            claim.parameter_index != parameter_index ||
            !target_argument_is_exact(semantic, target, &claim))
            return false;
        matches++;
    }
    return matches != 0;
}

XR_FUNC XrAotScalarRefV1Status xr_aot_scalar_ref_v1_parameter_status(
    const XrSemanticPlan *semantic, const XrTargetPlan *target,
    uint32_t semantic_value) {
    if (!authority_is_ordinary_and_intact(semantic, target))
        return XR_AOT_SCALAR_REF_V1_UNRELATED;
    uint32_t parameter_index = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticParameterRecord *parameter =
        parameter_for_value(semantic, semantic_value, &parameter_index);
    if (!parameter_is_scalar_ref(semantic, parameter))
        return XR_AOT_SCALAR_REF_V1_UNRELATED;
    return parameter_is_exact(semantic, parameter) &&
                   parameter_calls_are_exact(semantic, target, parameter_index)
               ? XR_AOT_SCALAR_REF_V1_EXACT
               : XR_AOT_SCALAR_REF_V1_INVALID;
}

XR_FUNC XrAotScalarRefV1Status xr_aot_scalar_ref_v1_call_use_status(
    const XrSemanticPlan *semantic, const XrTargetPlan *target,
    uint32_t operation_index, uint16_t operand_index, uint32_t source_value) {
    if (!authority_is_ordinary_and_intact(semantic, target))
        return XR_AOT_SCALAR_REF_V1_UNRELATED;
    ScalarRefClaim claim = {0};
    XrAotScalarRefV1Status status =
        semantic_claim(semantic, operation_index, operand_index, source_value,
                       &claim);
    return status != XR_AOT_SCALAR_REF_V1_EXACT
               ? status
               : target_argument_is_exact(semantic, target, &claim)
                     ? XR_AOT_SCALAR_REF_V1_EXACT
                     : XR_AOT_SCALAR_REF_V1_INVALID;
}

XR_FUNC XrAotScalarRefV1Status xr_aot_scalar_ref_v1_local_addr_status(
    const XrSemanticPlan *semantic, const XrTargetPlan *target,
    uint32_t operation_index, uint32_t *source_value) {
    if (source_value)
        *source_value = XR_SEMANTIC_INDEX_NONE;
    if (!authority_is_ordinary_and_intact(semantic, target))
        return XR_AOT_SCALAR_REF_V1_UNRELATED;
    const XrSemanticOperationRecord *address =
        xr_semantic_plan_operation(semantic, operation_index);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    if (!address || !operands || address->opcode != XI_LOCAL_ADDR ||
        address->operand_count != 1 || address->operand_begin >= operand_count)
        return XR_AOT_SCALAR_REF_V1_UNRELATED;
    uint32_t matches = 0;
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(semantic);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *call =
            xr_semantic_plan_operation(semantic, i);
        if (!call || (call->opcode != XI_CALL && call->opcode != XI_TAIL_CALL) ||
            call->operand_begin > operand_count ||
            call->operand_count > operand_count - call->operand_begin)
            continue;
        for (uint16_t operand = 1; operand < call->operand_count; operand++) {
            if (operands[call->operand_begin + operand].value !=
                address->result_value)
                continue;
            XrAotScalarRefV1Status status = xr_aot_scalar_ref_v1_call_use_status(
                semantic, target, i, operand, address->result_value);
            if (status == XR_AOT_SCALAR_REF_V1_INVALID)
                return status;
            if (status == XR_AOT_SCALAR_REF_V1_EXACT)
                matches++;
        }
    }
    if (matches == 0)
        return XR_AOT_SCALAR_REF_V1_UNRELATED;
    if (source_value)
        *source_value = operands[address->operand_begin].value;
    return XR_AOT_SCALAR_REF_V1_EXACT;
}

XR_FUNC XrAotScalarRefV1Status xr_aot_scalar_ref_v1_place_use_status(
    const XrSemanticPlan *semantic, const XrTargetPlan *target,
    uint32_t operation_index, uint16_t operand_index, uint32_t source_value) {
    if (!authority_is_ordinary_and_intact(semantic, target))
        return XR_AOT_SCALAR_REF_V1_UNRELATED;
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(semantic, operation_index);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    if (!operation || !operands ||
        (operation->opcode != XI_PLACE_LOAD &&
         operation->opcode != XI_PLACE_STORE) ||
        operation->operand_count !=
            (operation->opcode == XI_PLACE_LOAD ? 1u : 2u) ||
        operand_index >= operation->operand_count ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin)
        return XR_AOT_SCALAR_REF_V1_UNRELATED;
    const XrSemanticOperandRecord *place =
        &operands[operation->operand_begin];
    uint32_t parameter_index = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticParameterRecord *parameter =
        parameter_for_value(semantic, place->value, &parameter_index);
    uint32_t address_index = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *address =
        operation_for_value(semantic, place->value, &address_index);
    XrAotScalarRefV1Status owner = XR_AOT_SCALAR_REF_V1_UNRELATED;
    uint32_t owner_type = XR_SEMANTIC_INDEX_NONE;
    uint32_t owner_function = XR_SEMANTIC_INDEX_NONE;
    if (parameter_is_scalar_ref(semantic, parameter)) {
        owner = parameter_is_exact(semantic, parameter) &&
                        parameter_calls_are_exact(semantic, target, parameter_index)
                    ? XR_AOT_SCALAR_REF_V1_EXACT
                    : XR_AOT_SCALAR_REF_V1_INVALID;
        owner_type = parameter->type;
        owner_function = parameter->function;
    } else if (address && address->opcode == XI_LOCAL_ADDR) {
        owner = xr_aot_scalar_ref_v1_local_addr_status(
            semantic, target, address_index, NULL);
        owner_type = address->result_type;
        owner_function = address->function;
    }
    if (owner == XR_AOT_SCALAR_REF_V1_UNRELATED)
        return XR_AOT_SCALAR_REF_V1_UNRELATED;
    bool exact = owner == XR_AOT_SCALAR_REF_V1_EXACT &&
                 operation->function == owner_function &&
                 operation->effects == xi_generated_op_effects(operation->opcode) &&
                 operation->flags == xi_generated_op_default_flags(operation->opcode) &&
                 operation->ownership_use == xi_generated_op_own_use(operation->opcode) &&
                 operation->result_ownership ==
                     xi_generated_op_result_ownership(operation->opcode) &&
                 operation->result_alias_operand == -1 &&
                 operation->return_parameter == -1 &&
                 operation->intrinsic_kind == XR_SEM_INTRINSIC_NONE &&
                 operation->metadata_count == 0 &&
                 operation->auxiliary_kind == XI_AUX_KIND_NONE &&
                 operation->semantic_immediate == 0 &&
                 operation->constant == XR_SEMANTIC_INDEX_NONE &&
                 operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
                 operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
                 operation->allocation_key == NULL &&
                 stable_id_is_zero(operation->allocation_id) &&
                 place->type == owner_type &&
                 place->role == XR_SEM_OPERAND_VALUE && place->parameter == -1 &&
                 place->parameter_mode == XR_PARAM_READ &&
                 place->transfer_mode == XR_TRANSFER_SHARE &&
                 place->access == XR_CALL_ARG_PLAIN &&
                 place->origin == XI_PLACE_ORIGIN_NONE &&
                 place->lifetime == XI_PLACE_LIFETIME_NONE &&
                 place->escape == XI_PLACE_ESCAPE_NONE && place->flags == 0 &&
                 place->ownership_action == XR_SEM_OPERAND_BORROW;
    if (exact && operation->opcode == XI_PLACE_LOAD)
        exact = operation->result_type == owner_type &&
                value_slot_is_exact(target, operation->result_value,
                                    operation->function, operation_index,
                                    XR_TARGET_SLOT_TEMPORARY, XR_MACHINE_REP_I64);
    if (exact && operation->opcode == XI_PLACE_STORE) {
        const XrSemanticOperandRecord *stored = place + 1;
        exact = stored->type == owner_type &&
                stored->role == XR_SEM_OPERAND_VALUE && stored->parameter == -1 &&
                stored->parameter_mode == XR_PARAM_READ &&
                stored->transfer_mode == XR_TRANSFER_SHARE &&
                stored->access == XR_CALL_ARG_PLAIN &&
                stored->origin == XI_PLACE_ORIGIN_NONE &&
                stored->lifetime == XI_PLACE_LIFETIME_NONE &&
                stored->escape == XI_PLACE_ESCAPE_NONE && stored->flags == 0 &&
                stored->ownership_action == XR_SEM_OPERAND_CONSUME &&
                source_value_slot_is_exact(
                    semantic, target, stored->value, operation->function,
                    stored->type, NULL);
    }
    if (!exact)
        return XR_AOT_SCALAR_REF_V1_INVALID;
    uint32_t expected =
        operand_index == 0 ? place->value : (place + 1)->value;
    return source_value == expected ? XR_AOT_SCALAR_REF_V1_EXACT
                                    : XR_AOT_SCALAR_REF_V1_INVALID;
}
