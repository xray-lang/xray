/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_scalar_semantic_plan.c - PSC-backed scalar SemanticPlan verifier
 */

#include "xi_scalar_semantic_plan.h"
#include "xi_module.h"
#include "xi_ops_gen.h"
#include "xi_own.h"
#include "xi_scalar_program.h"
#include "../plan/semantic/xr_semantic_array_member_shape.h"
#include "../plan/semantic/xr_semantic_plan_internal.h"
#include "../plan/semantic/xr_semantic_verify.h"
#include "../plan/target/xr_scalar_call_decision.h"
#include "../plan/target/xr_target_profile.h"
#include <stdio.h>
#include <string.h>

static bool semantic_scalar_fail(char *error, size_t error_size,
                                 const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "XR_SEM_0019: %s", detail);
    return false;
}

static uint32_t semantic_function_for_program_row(
    const XrSemanticPlan *plan, uint32_t program_row) {
    uint32_t match = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; plan &&
                         i < plan->program_function_binding_count; i++) {
        const XrSemanticProgramFunctionBinding *binding =
            &plan->program_function_bindings[i];
        if (binding->program_row != program_row)
            continue;
        if (match != XR_SEMANTIC_INDEX_NONE)
            return XR_SEMANTIC_INDEX_NONE;
        match = binding->semantic_function;
    }
    return match;
}

static const XrSemanticProgramFunctionBinding *
semantic_binding_for_program_row(const XrSemanticPlan *plan,
                                 uint32_t program_row) {
    const XrSemanticProgramFunctionBinding *match = NULL;
    for (uint32_t i = 0; plan &&
                         i < plan->program_function_binding_count; i++) {
        const XrSemanticProgramFunctionBinding *binding =
            &plan->program_function_bindings[i];
        if (binding->program_row != program_row)
            continue;
        if (match)
            return NULL;
        match = binding;
    }
    return match;
}

static bool semantic_function_is_exact(
    const XrSemanticPlan *plan, uint32_t index, uint16_t parameter_count,
    char *error, size_t error_size) {
    const XrSemanticFunctionRecord *function =
        xr_semantic_plan_function(plan, index);
    const XrSemanticTypeRecord *result =
        function ? xr_semantic_plan_type(plan, function->return_type) : NULL;
    if (!function || !xr_semantic_array_member_i64_type_is_exact(result) ||
        function->parent != 0 || function->parameter_count != parameter_count ||
        function->child_count != 0 || function->capture_count != 0 ||
        function->capability_mask != 0 || function->semantic_effects != 0 ||
        function->source_class != XR_SEMANTIC_INDEX_NONE ||
        function->source_kind != XR_SEM_SOURCE_FUNCTION_NONE ||
        function->source_member_ordinal != UINT16_MAX ||
        function->flags != XR_SEM_FUNCTION_NOTHROW ||
        function->is_module_initializer != 0 ||
        function->carries_coroutine_ops != 0)
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan function row is not sealed i64");
    if (parameter_count == 0)
        return true;
    const XrSemanticParameterRecord *parameter =
        xr_semantic_plan_parameter(plan, function->parameter_begin);
    const XrSemanticTypeRecord *parameter_type =
        parameter ? xr_semantic_plan_type(plan, parameter->type) : NULL;
    return (parameter && parameter->function == index &&
            parameter->ordinal == 0 &&
            xr_semantic_array_member_i64_type_is_exact(parameter_type) &&
            parameter->mode == XR_PARAM_READ &&
            parameter->ownership == XI_OWN_NONE &&
            parameter->transfer_mode == XR_TRANSFER_SHARE) ||
           semantic_scalar_fail(error, error_size,
                                "SemanticPlan scalar parameter row is invalid");
}

static bool semantic_call_is_exact(
    const XrSemanticPlan *plan,
    const XrProgramSemanticCallRecord *program_call, uint32_t caller,
    uint32_t callee, char *error, size_t error_size) {
    const XrSemanticProgramCallBinding *binding =
        plan->program_call_binding_count == 1
            ? &plan->program_call_bindings[0]
            : NULL;
    if (!binding || binding->program_row != 0 ||
        binding->target_function != callee ||
        !xr_stable_id_equal(binding->program_call, program_call->id) ||
        !xr_stable_id_equal(binding->callsite,
                            program_call->callsite_identity) ||
        !xr_stable_id_equal(binding->caller_program_function,
                            program_call->caller_function) ||
        !xr_stable_id_equal(binding->callee_program_function,
                            program_call->callee_function))
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan program call binding is not exact");
    uint32_t operation_index = binding->operation;
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(plan, operation_index);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan, &operand_count);
    const XrSemanticTypeRecord *result =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    const XrSemanticOperandRecord *callee_operand =
        operation && operation->operand_count == 2 &&
                operation->operand_begin <= operand_count &&
                operation->operand_count <= operand_count - operation->operand_begin
            ? &operands[operation->operand_begin]
            : NULL;
    const XrSemanticOperandRecord *argument =
        callee_operand ? callee_operand + 1 : NULL;
    const XrSemanticTypeRecord *argument_type =
        argument ? xr_semantic_plan_type(plan, argument->type) : NULL;
    if (!program_call || !operation || !callee_operand || !argument ||
        operation->function != caller || operation->opcode != XI_CALL ||
        !xr_semantic_array_member_i64_type_is_exact(result) ||
        !xr_semantic_array_member_i64_type_is_exact(argument_type) ||
        operation->source_start_line != program_call->locator.start_line ||
        operation->source_start_column != program_call->locator.start_column ||
        operation->source_end_line != program_call->locator.end_line ||
        operation->source_end_column != program_call->locator.end_column ||
        operation->effects != xi_generated_op_effects(XI_CALL) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL) ||
        operation->semantic_immediate != 0 ||
        operation->metadata_count != 0 ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        callee_operand->role != XR_SEM_OPERAND_CALLEE ||
        callee_operand->parameter != -1 || callee_operand->flags != 0 ||
        argument->role != XR_SEM_OPERAND_ARGUMENT ||
        argument->parameter != 0 ||
        argument->flags != XR_SEM_OPERAND_CALL_CONTRACT)
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan call/value rows are not exact");
    if (xr_semantic_plan_call_target_count(plan) != 1)
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan call target inventory is not exact");
    const XrSemanticCallTargetRecord *target =
        xr_semantic_plan_call_target(plan, 0);
    return (target && target->operation == operation_index &&
            target->function == callee &&
            target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
            target->dependency == XR_SEMANTIC_INDEX_NONE &&
            target->source_export == XR_SEMANTIC_INDEX_NONE &&
            target->callable_type == XR_SEMANTIC_INDEX_NONE) ||
           semantic_scalar_fail(error, error_size,
                                "SemanticPlan direct call target is invalid");
}

bool xi_scalar_semantic_plan_verify(
    const XiFunc *root, const XrSemanticPlan *plan,
    const XrTargetProfile *target_profile, char *error, size_t error_size) {
    const XiModule *module = root ? root->module : NULL;
    const XrProgramSemanticClosure *closure =
        module ? module->program_semantic_closure : NULL;
    const XrTargetProfile *plan_profile =
        module ? module->scalar_target_profile : NULL;
    const XrScalarCallDecision *decision =
        module ? module->scalar_call_decision : NULL;
    if (!root || !module || module->init != root || !plan || !closure ||
        !decision || !plan_profile || !target_profile)
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan external authority is incomplete");
    XrFingerprint closure_fingerprint =
        xr_program_semantic_closure_fingerprint(closure);
    XrGenerationClosureId generation =
        xr_program_semantic_closure_generation_id(closure);
    if (!xr_target_profile_require_exact(plan_profile, target_profile, error,
                                         error_size) ||
        !xi_scalar_program_verify(module, plan_profile, error, error_size) ||
        !xr_scalar_call_decision_verify(decision, closure, plan_profile,
                                        error, error_size) ||
        !xr_semantic_plan_verify(plan, error, error_size) ||
        xr_semantic_plan_function_count(plan) != 3 ||
        plan->program_provenance.schema !=
            XR_SEMANTIC_PROGRAM_PROVENANCE_SCHEMA_VERSION ||
        plan->program_provenance.program_schema !=
            xr_program_semantic_closure_schema(closure) ||
        plan->program_provenance.function_count != 2 ||
        plan->program_provenance.call_count != 1 ||
        plan->program_function_binding_count != 2 ||
        plan->program_call_binding_count != 1 ||
        !xr_fingerprint_equal(plan->program_provenance.program_fingerprint,
                              closure_fingerprint) ||
        memcmp(plan->program_provenance.generation_identity.bytes,
               generation.bytes, sizeof(generation.bytes)) != 0)
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan external authority provenance is invalid");
    const XrProgramSemanticCallRecord *program_call =
        xr_program_semantic_closure_call(closure, 0);
    const XrScalarCallDecision *sealed = decision;
    uint32_t caller_program = XR_SEMANTIC_INDEX_NONE;
    uint32_t callee_program = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < 2; i++) {
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(closure, i);
        if (row && xr_stable_id_equal(row->id, sealed->caller_function))
            caller_program = i;
        if (row && xr_stable_id_equal(row->id, sealed->callee_function))
            callee_program = i;
    }
    uint32_t caller = semantic_function_for_program_row(plan, caller_program);
    uint32_t callee = semantic_function_for_program_row(plan, callee_program);
    const XrSemanticProgramFunctionBinding *caller_binding =
        semantic_binding_for_program_row(plan, caller_program);
    const XrSemanticProgramFunctionBinding *callee_binding =
        semantic_binding_for_program_row(plan, callee_program);
    if (!program_call || caller_program == XR_SEMANTIC_INDEX_NONE ||
        callee_program == XR_SEMANTIC_INDEX_NONE ||
        caller == XR_SEMANTIC_INDEX_NONE || callee == XR_SEMANTIC_INDEX_NONE ||
        !caller_binding || !callee_binding ||
        !xr_stable_id_equal(caller_binding->program_function,
                            sealed->caller_function) ||
        !xr_stable_id_equal(callee_binding->program_function,
                            sealed->callee_function) ||
        caller == callee || !semantic_function_is_exact(plan, caller, 0, error,
                                                        error_size) ||
        !semantic_function_is_exact(plan, callee, 1, error, error_size) ||
        !semantic_call_is_exact(plan, program_call, caller, callee, error,
                                error_size))
        return false;
    for (uint16_t i = 0; i < module->nfuncs; i++) {
        const XiFunc *function = module->functions[i];
        uint32_t semantic = semantic_function_for_program_row(
            plan, function ? function->psc_function_index : XI_PSC_ROW_NONE);
        if (!function || semantic == XR_SEMANTIC_INDEX_NONE ||
            (function->semantic_plan && function->semantic_plan != plan) ||
            (function->semantic_plan &&
             function->semantic_plan_function_index != semantic))
            return semantic_scalar_fail(
                error, error_size,
                "Xi and SemanticPlan function row bindings disagree");
    }
    return true;
}
