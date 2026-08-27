/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Independent SemanticPlan proof for the sealed i64 overflow family.
 */

#include "xi_i64_overflow_semantic_plan.h"
#include "xi_i64_overflow_program.h"
#include "xi_arc.h"
#include "xi_own.h"
#include "xi_ops_gen.h"
#include "../plan/semantic/xr_semantic_plan_internal.h"
#include "../plan/semantic/xr_semantic_verify.h"
#include "../plan/target/xr_i64_overflow_decision.h"
#include "../plan/target/xr_target_profile.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include "../shared/xr_exact_scalar_registry.h"
#include <stdio.h>
#include <string.h>

static bool fail(char *error, size_t error_size, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "XR_SEM_0019: %s", detail);
    return false;
}

static bool same_id(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static bool zero_id(XrStableId id) {
    static const XrStableId zero = {{0}};
    return same_id(id, zero);
}

static bool exact_i64_type(const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    return type && type->kind == XR_KIND_INT && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->aggregate_extent == 0 &&
           type->aggregate_align == 0 && type->scalar_rep == XR_NATIVE_I64 &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
           same_id(type->source_class_identity, zero) &&
           same_id(type->source_enum_identity, zero) && !type->source_enum_key &&
           type->enum_layout_id == 0 && type->enum_member_count == 0 &&
           type->enum_flags == 0 && type->reserved_enum == 0 && type->flags == 0;
}

static bool exact_bool_type(const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    return type && type->kind == XR_KIND_BOOL && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->aggregate_extent == 0 &&
           type->aggregate_align == 0 && type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
           same_id(type->source_class_identity, zero) &&
           same_id(type->source_enum_identity, zero) && !type->source_enum_key &&
           type->enum_layout_id == 0 && type->enum_member_count == 0 &&
           type->enum_flags == 0 && type->reserved_enum == 0 && type->flags == 0;
}

static const XrSemanticOperationRecord *operation_for_xi_value(
    const XrSemanticPlan *plan, const XrSemanticFunctionRecord *function,
    const XiValue *value, uint32_t *index) {
    const XrSemanticOperationRecord *match = NULL;
    uint32_t global_value = function && value && value->id < function->value_count
                                ? function->value_begin + value->id
                                : XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; plan && i < plan->operation_count; i++) {
        const XrSemanticOperationRecord *candidate = &plan->operations[i];
        if (candidate->function != (uint32_t) (function - plan->functions) ||
            candidate->result_value != global_value)
            continue;
        if (match)
            return NULL;
        match = candidate;
        if (index)
            *index = i;
    }
    return match;
}

static bool operation_is_exact(
    const XiFunc *xi_function, const XiValue *value,
    const XrProgramSemanticCallRecord *program_call,
    const XrI64OverflowDecisionRow *decision, const XrSemanticPlan *plan,
    const XrSemanticFunctionRecord *function, const char *module_identity,
    uint32_t i64_type, bool *seen, char *error, size_t error_size) {
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *operation =
        operation_for_xi_value(plan, function, value, &operation_index);
    const XrSemanticProgramCallBinding *binding =
        xr_semantic_plan_program_call_for_operation(plan, operation_index);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan, &operand_count);
    const XrSemanticOperandRecord *receiver =
        operation && operation->operand_count == 2 &&
                operation->operand_begin <= operand_count &&
                operation->operand_count <= operand_count - operation->operand_begin
            ? &operands[operation->operand_begin]
            : NULL;
    const XrSemanticOperandRecord *argument = receiver ? receiver + 1 : NULL;
    const XrSemanticTypeRecord *result_type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    uint32_t expected_receiver =
        value && value->args && value->args[0] && value->args[0]->id < function->value_count
            ? function->value_begin + value->args[0]->id
            : XR_SEMANTIC_INDEX_NONE;
    uint32_t expected_argument =
        value && value->args && value->args[1] && value->args[1]->id < function->value_count
            ? function->value_begin + value->args[1]->id
            : XR_SEMANTIC_INDEX_NONE;
    XrStableId zero = {{0}};
    if (!xi_function || !value || !program_call || !decision || !operation || !binding ||
        !receiver || !argument || !result_type || seen[decision->program_row])
        return fail(error, error_size,
                    "overflow SemanticPlan operation inventory is incomplete");
    if (operation->function != (uint32_t) (function - plan->functions) ||
        operation->opcode != XI_CALL_METHOD || operation->result_type == i64_type ||
        !exact_bool_type(result_type) || operation->operand_count != 2 ||
        operation->result_value != function->value_begin + value->id ||
        operation->metadata_count != 0 || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->semantic_immediate != ((int64_t) decision->method_symbol << 1) ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) ||
        operation->allocation_key || !same_id(operation->allocation_id, zero) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE)
        return fail(error, error_size,
                    "overflow SemanticPlan operation shape is invalid");
    if (
        operation->source_line != program_call->locator.start_line ||
        !operation->source_file || !module_identity ||
        strcmp(operation->source_file, module_identity) != 0 ||
        operation->source_start_line != program_call->locator.start_line ||
        operation->source_start_column != program_call->locator.start_column ||
        operation->source_end_line != program_call->locator.end_line ||
        operation->source_end_column != program_call->locator.end_column) {
        if (error && error_size)
            snprintf(error, error_size,
                     "XR_SEM_0019: overflow SemanticPlan source locator is invalid "
                     "line=%u/%u span=%u:%u-%u:%u/%u:%u-%u:%u file=%s/%s",
                     operation->source_line, program_call->locator.start_line,
                     operation->source_start_line, operation->source_start_column,
                     operation->source_end_line, operation->source_end_column,
                     program_call->locator.start_line, program_call->locator.start_column,
                     program_call->locator.end_line, program_call->locator.end_column,
                     operation->source_file ? operation->source_file : "",
                     module_identity ? module_identity : "");
        return false;
    }
    if (
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->return_provenance != XR_SEM_RETURN_NONE ||
        operation->return_complete != 0)
        return fail(error, error_size,
                    "overflow SemanticPlan result contract is invalid");
    if (binding->program_row != decision->program_row ||
        !same_id(binding->program_call, decision->program_call) ||
        !same_id(binding->program_call, program_call->id) ||
        !same_id(binding->callsite, decision->callsite) ||
        !same_id(binding->callsite, program_call->callsite_identity) ||
        !same_id(binding->caller_program_function, decision->caller_function) ||
        !same_id(binding->callee_program_function, decision->builtin_identity) ||
        !same_id(binding->callee_program_function, program_call->callee_function) ||
        !zero_id(binding->resolver_binding) ||
        binding->target_function != XR_SEMANTIC_INDEX_NONE ||
        binding->program_dependency != XR_SEMANTIC_INDEX_NONE || binding->reserved != 0)
        return fail(error, error_size,
                    "overflow SemanticPlan program binding is invalid");
    if (receiver->value != expected_receiver || receiver->type != i64_type ||
        receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->transfer_mode != XR_TRANSFER_SHARE ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        receiver->parameter_mode != XR_PARAM_READ || receiver->access != XR_CALL_ARG_PLAIN ||
        receiver->origin != 0 || receiver->lifetime != 0 || receiver->escape != 0 ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT)
        return fail(error, error_size,
                    "overflow SemanticPlan receiver operand is invalid");
    if (argument->value != expected_argument || argument->type != i64_type ||
        argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != 0 ||
        argument->transfer_mode != XR_TRANSFER_SHARE ||
        argument->ownership_action != XR_SEM_OPERAND_BORROW ||
        argument->parameter_mode != XR_PARAM_READ || argument->access != XR_CALL_ARG_PLAIN ||
        argument->origin != 0 || argument->lifetime != 0 || argument->escape != 0 ||
        argument->flags != XR_SEM_OPERAND_CALL_CONTRACT)
        return fail(error, error_size,
                    "overflow SemanticPlan argument operand is invalid");
    seen[decision->program_row] = true;
    return true;
}

static bool verify_xi_calls(
    const XiFunc *xi_function, const XrProgramSemanticClosure *closure,
    const XrI64OverflowDecisionTable *table, const XrSemanticPlan *plan,
    const XrSemanticFunctionRecord *function, const char *module_identity,
    uint32_t i64_type, bool *seen, uint32_t *count, char *error,
    size_t error_size) {
    for (uint32_t b = 0; xi_function && b < xi_function->nblocks; b++) {
        const XiBlock *block = xi_function->blocks ? xi_function->blocks[b] : NULL;
        for (uint32_t i = 0; block && i < block->nvalues; i++) {
            const XiValue *value = block->values[i];
            if (!value || value->psc_call_index == XI_PSC_ROW_NONE)
                continue;
            const XrProgramSemanticCallRecord *call =
                xr_program_semantic_closure_call(closure, value->psc_call_index);
            const XrI64OverflowDecisionRow *decision =
                xr_i64_overflow_decision_for_program_row(table,
                                                          value->psc_call_index);
            if (!operation_is_exact(xi_function, value, call, decision, plan, function,
                                    module_identity, i64_type, seen, error, error_size))
                return false;
            (*count)++;
        }
    }
    return true;
}

bool xi_i64_overflow_semantic_plan_verify(
    const XiFunc *root, const XrSemanticPlan *plan,
    const XrTargetProfile *target_profile, char *error, size_t error_size) {
    const XiModule *module = root ? root->module : NULL;
    const XrProgramSemanticClosure *closure =
        module ? module->program_semantic_closure : NULL;
    const XrI64OverflowDecisionTable *table =
        module ? module->i64_overflow_decisions : NULL;
    const XrTargetProfile *retained_profile =
        module ? module->scalar_target_profile : NULL;
    const XrSemanticProgramProvenance *provenance =
        xr_semantic_plan_program_provenance(plan);
    XrFingerprint closure_fingerprint =
        closure ? xr_program_semantic_closure_fingerprint(closure) : (XrFingerprint) {{0}};
    XrGenerationClosureId generation =
        closure ? xr_program_semantic_closure_generation_id(closure)
                : (XrGenerationClosureId) {{0}};
    if (!root || !module || module->init != root || !plan || !closure || !table ||
        !retained_profile || !target_profile || !provenance ||
        !xr_target_profile_require_exact(retained_profile, target_profile, NULL, 0) ||
        !xi_i64_overflow_program_verify(module, target_profile, NULL, 0) ||
        !xr_i64_overflow_decision_verify(table, closure, target_profile, NULL, 0) ||
        !xr_semantic_plan_verify(plan, error, error_size) ||
        xr_program_semantic_closure_family(closure) !=
            XR_PROGRAM_SEMANTIC_FAMILY_I64_OVERFLOW_PREDICATE ||
        provenance->schema != XR_SEMANTIC_PROGRAM_PROVENANCE_SCHEMA_VERSION ||
        provenance->program_schema != xr_program_semantic_closure_schema(closure) ||
        provenance->program_family != XR_PROGRAM_SEMANTIC_FAMILY_I64_OVERFLOW_PREDICATE ||
        provenance->type_count != 1 || provenance->type_field_count != 0 ||
        provenance->function_count != 1 || provenance->call_count != table->row_count ||
        provenance->module_count != 1 || provenance->dependency_count != 0 ||
        provenance->program_module_row != 0 ||
        provenance->program_dependency_binding_count != 0 ||
        !xr_fingerprint_equal(provenance->program_fingerprint, closure_fingerprint) ||
        memcmp(provenance->generation_identity.bytes, generation.bytes,
               sizeof(generation.bytes)) != 0 ||
        plan->program_type_binding_count != 1 ||
        plan->program_type_field_binding_count != 0 ||
        plan->program_function_binding_count != 1 ||
        plan->program_call_binding_count != table->row_count ||
        plan->program_dependency_binding_count != 0 || plan->dependency_count != 0 ||
        plan->call_target_count != 0 || plan->function_count != 2)
        return fail(error, error_size, "overflow SemanticPlan provenance is incomplete");

    const XrProgramSemanticTypeRecord *program_i64 =
        xr_program_semantic_closure_type(closure, 0);
    const XrSemanticProgramTypeBinding *type_binding =
        xr_semantic_plan_program_type_for_row(plan, 0);
    const XrSemanticTypeRecord *semantic_i64 =
        type_binding ? xr_semantic_plan_type(plan, type_binding->semantic_type) : NULL;
    const XrProgramSemanticFunctionRecord *program_function =
        xr_program_semantic_closure_function(closure, 0);
    const XrSemanticProgramFunctionBinding *function_binding =
        plan->program_function_bindings;
    const XrSemanticFunctionRecord *semantic_function =
        function_binding && function_binding->semantic_function < plan->function_count
            ? &plan->functions[function_binding->semantic_function]
            : NULL;
    const XiFunc *xi_function = module->nfuncs == 1 ? module->functions[0] : NULL;
    if (!program_i64 || !type_binding || !semantic_i64 || !program_function ||
        !function_binding || !semantic_function || !xi_function ||
        program_i64->kind != XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR ||
        program_i64->exact_scalar != XR_EXACT_SCALAR_I64 ||
        type_binding->program_row != 0 || type_binding->semantic_type >= plan->type_count ||
        type_binding->kind != XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR ||
        type_binding->exact_scalar != XR_EXACT_SCALAR_I64 || type_binding->field_count != 0 ||
        !same_id(type_binding->program_type, program_i64->id) ||
        !exact_i64_type(semantic_i64) || function_binding->program_row != 0 ||
        function_binding->flags != XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY ||
        !same_id(function_binding->program_function, program_function->id) ||
        semantic_function->parent != 0 || semantic_function->parameter_count != 2 ||
        semantic_function->return_type != type_binding->semantic_type ||
        semantic_function->parameter_begin > plan->parameter_count ||
        semantic_function->parameter_count >
            plan->parameter_count - semantic_function->parameter_begin)
        return fail(error, error_size, "overflow SemanticPlan function/type rows are invalid");
    for (uint32_t i = 0; i < 2; i++) {
        const XrSemanticParameterRecord *parameter =
            &plan->parameters[semantic_function->parameter_begin + i];
        if (parameter->function != function_binding->semantic_function ||
            parameter->ordinal != i || parameter->type != type_binding->semantic_type ||
            parameter->mode != XR_PARAM_READ || parameter->ownership != XI_OWN_NONE ||
            parameter->transfer_mode != XR_TRANSFER_SHARE ||
            parameter->flags != XR_SEM_PARAMETER_REQUIRED ||
            parameter->reserved != 0)
            return fail(error, error_size,
                        "overflow SemanticPlan parameter rows are invalid");
    }

    uint32_t semantic_calls = 0;
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &plan->operations[i];
        if (xi_generated_op_class(operation->opcode) != XI_GEN_CLASS_CALL)
            continue;
        if (operation->opcode != XI_CALL_METHOD ||
            operation->function != function_binding->semantic_function)
            return fail(error, error_size,
                        "overflow SemanticPlan contains an unbound call operation");
        semantic_calls++;
    }
    if (semantic_calls != table->row_count)
        return fail(error, error_size,
                    "overflow SemanticPlan call inventory is incomplete");

    bool *seen = (bool *) xr_calloc(table->row_count, sizeof(*seen));
    uint32_t count = 0;
    if (!seen)
        return fail(error, error_size, "overflow SemanticPlan verification allocation failed");
    bool ok = verify_xi_calls(xi_function, closure, table, plan, semantic_function,
                              module->identity, type_binding->semantic_type, seen,
                              &count, error, error_size);
    if (!ok) {
        xr_free(seen);
        return false;
    }
    for (uint32_t i = 0; ok && i < table->row_count; i++)
        ok = seen[i];
    xr_free(seen);
    return (ok && count == table->row_count) ||
           fail(error, error_size, "overflow SemanticPlan call coverage is incomplete");
}
