/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_program_semantic_plan.c - PSC-backed SemanticPlan verifier
 */

#include "xi_program_semantic_plan.h"
#include "xi_i64_overflow_semantic_plan.h"
#include "xi_module.h"
#include "xi_ops_gen.h"
#include "xi_own.h"
#include "xi_program_semantic.h"
#include "../plan/semantic/xr_semantic_plan_internal.h"
#include "../plan/semantic/xr_semantic_native_leaf_shape.h"
#include "../plan/semantic/xr_semantic_verify.h"
#include "../plan/target/xr_scalar_call_decision.h"
#include "../plan/target/xr_target_profile.h"
#include "../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>

static bool semantic_scalar_fail(char *error, size_t error_size, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "XR_SEM_0019: %s", detail);
    return false;
}

static uint32_t semantic_function_for_program_row(const XrSemanticPlan *plan,
                                                  uint32_t program_row) {
    uint32_t match = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; plan && i < plan->program_function_binding_count; i++) {
        const XrSemanticProgramFunctionBinding *binding = &plan->program_function_bindings[i];
        if (binding->program_row != program_row)
            continue;
        if (match != XR_SEMANTIC_INDEX_NONE)
            return XR_SEMANTIC_INDEX_NONE;
        match = binding->semantic_function;
    }
    return match;
}

static const XrSemanticProgramFunctionBinding *
semantic_binding_for_program_row(const XrSemanticPlan *plan, uint32_t program_row) {
    const XrSemanticProgramFunctionBinding *match = NULL;
    for (uint32_t i = 0; plan && i < plan->program_function_binding_count; i++) {
        const XrSemanticProgramFunctionBinding *binding = &plan->program_function_bindings[i];
        if (binding->program_row != program_row)
            continue;
        if (match)
            return NULL;
        match = binding;
    }
    return match;
}

static bool semantic_function_is_exact(const XrSemanticPlan *plan, uint32_t index,
                                       uint16_t parameter_count, uint32_t expected_type,
                                       char *error, size_t error_size) {
    const XrSemanticFunctionRecord *function = xr_semantic_plan_function(plan, index);
    if (!function || function->return_type != expected_type || function->parent != 0 ||
        function->parameter_count != parameter_count)
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan function signature is not exact");
    if (function->child_count != 0 || function->capture_count != 0 ||
        function->capability_mask != 0 || function->semantic_effects != 0)
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan function effect shape is not exact");
    if (function->source_class != XR_SEMANTIC_INDEX_NONE ||
        function->source_kind != XR_SEM_SOURCE_FUNCTION_NONE ||
        function->source_member_ordinal != UINT16_MAX)
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan function source binding is not exact");
    if (function->flags != XR_SEM_FUNCTION_NOTHROW || function->is_module_initializer != 0 ||
        function->carries_coroutine_ops != 0 || function->reserved != 0)
        return semantic_scalar_fail(error, error_size, "SemanticPlan function flags are not exact");
    if (function->return_parameter != -1 || function->return_provenance != XR_SEM_RETURN_NONE) {
        if (error && error_size)
            snprintf(error, error_size,
                     "XR_SEM_0019: SemanticPlan function %u return ownership "
                     "is not exact (parameter=%d provenance=%u)",
                     index, function->return_parameter, function->return_provenance);
        return false;
    }
    if (parameter_count == 0)
        return true;
    const XrSemanticParameterRecord *parameter =
        xr_semantic_plan_parameter(plan, function->parameter_begin);
    if (!parameter || parameter->function != index || parameter->ordinal != 0 ||
        parameter->type != expected_type || parameter->mode != XR_PARAM_READ)
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan parameter identity is not exact");
    if (parameter->ownership != XI_OWN_NONE)
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan parameter ownership is not exact");
    if (parameter->transfer_mode != XR_TRANSFER_SHARE)
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan parameter transfer is not exact");
    return (parameter->flags == XR_SEM_PARAMETER_REQUIRED && parameter->reserved == 0) ||
           semantic_scalar_fail(error, error_size, "SemanticPlan parameter flags are not exact");
}

static bool semantic_initializer_is_exact(const XrSemanticPlan *plan, const XiFunc *root,
                                          char *error, size_t error_size) {
    const XrSemanticFunctionRecord *function = xr_semantic_plan_function(plan, 0);
    uint8_t expected_flags =
        root ? (uint8_t) ((root->error_effect_nothrow ? XR_SEM_FUNCTION_NOTHROW : 0u) |
                          (root->contains_unsafe_op ? XR_SEM_FUNCTION_CONTAINS_UNSAFE : 0u) |
                          (root->entry_type == 2 ? XR_SEM_FUNCTION_GENERATOR : 0u) |
                          (root->is_extern ? XR_SEM_FUNCTION_EXTERN : 0u))
             : 0u;
    if (!root || !function || function->parent != XR_SEMANTIC_INDEX_NONE ||
        function->parameter_count != 0 || function->child_count != root->nchildren ||
        function->capture_count != 0)
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan initializer hierarchy is not exact");
    if (function->semantic_effects != root->semantic_effects ||
        function->capability_mask != (root->requires_unsafe_at_call ? 1u : 0u) ||
        function->flags != expected_flags || function->is_module_initializer != 1 ||
        function->carries_coroutine_ops != 0 || function->reserved != 0)
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan initializer effects are not exact");
    if (function->source_class != XR_SEMANTIC_INDEX_NONE ||
        function->source_member_ordinal != UINT16_MAX ||
        function->source_kind != XR_SEM_SOURCE_FUNCTION_NONE)
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan initializer source binding is not exact");
    return (function->return_parameter == -1 &&
            function->return_provenance == XR_SEM_RETURN_NONE) ||
           semantic_scalar_fail(error, error_size,
                                "SemanticPlan initializer return ownership is not exact");
}

static bool semantic_id_is_zero(XrStableId id) {
    uint8_t combined = 0;
    for (uint32_t i = 0; i < sizeof(id.bytes); i++)
        combined |= id.bytes[i];
    return combined == 0;
}

static bool semantic_key_take_literal(const char **cursor, const char *literal) {
    size_t length = literal ? strlen(literal) : 0;
    if (!cursor || !*cursor || !literal || strncmp(*cursor, literal, length) != 0)
        return false;
    *cursor += length;
    return true;
}

static bool semantic_key_take_component(const char **cursor, const char *value) {
    char length_text[32];
    size_t length = value ? strlen(value) : 0;
    int written = snprintf(length_text, sizeof(length_text), "%zu:", length);
    if (written <= 0 || (size_t) written >= sizeof(length_text) ||
        !semantic_key_take_literal(cursor, length_text) ||
        strncmp(*cursor, value ? value : "", length) != 0)
        return false;
    *cursor += length;
    return true;
}

static bool semantic_source_module_key_is_exact(const XrSemanticPlan *plan, const XiFunc *root,
                                                XrStableId expected_module) {
    const XiModule *module = root ? root->module : NULL;
    const char *identity = module ? module->identity : NULL;
    const char *name =
        module && module->name ? module->name : (root && root->name ? root->name : "");
    const XrSemanticEntityRecord *package = NULL;
    const XrSemanticEntityRecord *module_entity = NULL;
    uint32_t package_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; plan && i < plan->entity_count; i++) {
        const XrSemanticEntityRecord *entity = &plan->entities[i];
        if (entity->kind == XR_SEM_ENTITY_PACKAGE) {
            if (package)
                return false;
            package = entity;
            package_index = i;
        } else if (entity->kind == XR_SEM_ENTITY_MODULE) {
            if (module_entity)
                return false;
            module_entity = entity;
        }
    }
    char package_prefix[96];
    char module_prefix[96];
    char package_id[XR_STABLE_ID_BYTES * 2u + 1u];
    int package_prefix_length =
        snprintf(package_prefix, sizeof(package_prefix),
                 "entity-v1:schema=%u:kind=%u:parent=none:authority=", XR_SEMANTIC_SCHEMA_VERSION,
                 (unsigned) XR_SEM_ENTITY_PACKAGE);
    int module_prefix_length =
        snprintf(module_prefix, sizeof(module_prefix),
                 "entity-v1:schema=%u:kind=%u:parent=", XR_SEMANTIC_SCHEMA_VERSION,
                 (unsigned) XR_SEM_ENTITY_MODULE);
    if (!identity || !identity[0] || !package || !module_entity || !package->canonical_key ||
        !module_entity->canonical_key || package_prefix_length <= 0 ||
        (size_t) package_prefix_length >= sizeof(package_prefix) || module_prefix_length <= 0 ||
        (size_t) module_prefix_length >= sizeof(module_prefix) ||
        module_entity->parent != package_index ||
        !xr_stable_id_equal(module_entity->id, expected_module))
        return false;
    const char *package_cursor = package->canonical_key;
    if (!semantic_key_take_literal(&package_cursor, package_prefix) ||
        !semantic_key_take_component(&package_cursor, identity) || package_cursor[0] != '\0')
        return false;
    xr_stable_id_hex(package->id, package_id);
    const char *module_cursor = module_entity->canonical_key;
    return semantic_key_take_literal(&module_cursor, module_prefix) &&
           semantic_key_take_literal(&module_cursor, package_id) &&
           semantic_key_take_literal(&module_cursor, ":name=") &&
           semantic_key_take_component(&module_cursor, name) &&
           semantic_key_take_literal(&module_cursor, ":identity=") &&
           semantic_key_take_component(&module_cursor, identity) && module_cursor[0] == '\0';
}

static bool semantic_source_module_key_matches_root(const XrSemanticPlan *plan,
                                                    const XiFunc *root) {
    const XrSemanticEntityRecord *module = NULL;
    for (uint32_t i = 0; plan && i < plan->entity_count; i++) {
        const XrSemanticEntityRecord *candidate = &plan->entities[i];
        if (candidate->kind != XR_SEM_ENTITY_MODULE)
            continue;
        if (module)
            return false;
        module = candidate;
    }
    return module && semantic_source_module_key_is_exact(plan, root, module->id);
}

static bool semantic_optional_text_is_exact(const char *frozen, const char *source) {
    if (!source || !source[0])
        return !frozen;
    return frozen && strcmp(frozen, source) == 0;
}

static bool semantic_source_class_is_exact(const XrSemanticPlan *plan, const XiFunc *root,
                                           const XiClassData *aggregate_source,
                                           const XrSemanticSourceClassRecord *source_class) {
    const XiModule *module = root ? root->module : NULL;
    uint8_t expected_flags =
        aggregate_source
            ? (uint8_t) ((aggregate_source->explicit_final ? XR_SEM_SOURCE_CLASS_EXPLICIT_FINAL
                                                           : 0u) |
                         (aggregate_source->needs_runtime_type ? XR_SEM_SOURCE_CLASS_RUNTIME_TYPE
                                                               : 0u) |
                         ((aggregate_source->is_generic_skeleton ||
                           aggregate_source->is_monomorphized ||
                           aggregate_source->mono_type_arg_count != 0)
                              ? XR_SEM_SOURCE_CLASS_GENERIC
                              : 0u))
            : 0u;
    return module && aggregate_source && source_class &&
           semantic_source_module_key_is_exact(plan, root, source_class->module) &&
           source_class->module_path && module->identity &&
           strcmp(source_class->module_path, module->identity) == 0 && source_class->name &&
           aggregate_source->class_name &&
           strcmp(source_class->name, aggregate_source->class_name) == 0 &&
           semantic_optional_text_is_exact(source_class->super_name,
                                           aggregate_source->super_name) &&
           source_class->ordinal == 0 && source_class->method_count == aggregate_source->nmethod &&
           source_class->flags == expected_flags && source_class->reserved == 0;
}

static bool semantic_i64_type_is_exact(const XrSemanticTypeRecord *type) {
    return type && type->kind == XR_KIND_INT && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_NATIVE_I64 && type->flags == 0;
}

static bool semantic_call_is_exact(const XrSemanticPlan *plan,
                                   const XrProgramSemanticCallRecord *program_call, uint32_t caller,
                                   uint32_t callee, uint32_t expected_type, const char *source_file,
                                   char *error, size_t error_size) {
    const XrSemanticProgramCallBinding *binding =
        plan->program_call_binding_count == 1 ? &plan->program_call_bindings[0] : NULL;
    if (!binding || binding->program_row != 0 || binding->target_function != callee ||
        !xr_stable_id_equal(binding->program_call, program_call->id) ||
        !xr_stable_id_equal(binding->callsite, program_call->callsite_identity) ||
        !xr_stable_id_equal(binding->caller_program_function, program_call->caller_function) ||
        !xr_stable_id_equal(binding->callee_program_function, program_call->callee_function))
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan program call binding is not exact");
    uint32_t operation_index = binding->operation;
    const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(plan, operation_index);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const XrSemanticOperandRecord *callee_operand =
        operation && operation->operand_count == 2 && operation->operand_begin <= operand_count &&
                operation->operand_count <= operand_count - operation->operand_begin
            ? &operands[operation->operand_begin]
            : NULL;
    const XrSemanticOperandRecord *argument = callee_operand ? callee_operand + 1 : NULL;
    const XrSemanticTypeRecord *callee_type =
        callee_operand ? xr_semantic_plan_type(plan, callee_operand->type) : NULL;
    const XrSemanticFunctionRecord *caller_function = xr_semantic_plan_function(plan, caller);
    const XrSemanticProgramProvenance *provenance = xr_semantic_plan_program_provenance(plan);
    uint8_t expected_argument_ownership =
        provenance && provenance->program_family ==
                          XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL
            ? XR_SEM_OPERAND_BORROW
            : XR_SEM_OPERAND_CONSUME;
    uint32_t source_discriminator = 1;
    if (operation && operation->source_file) {
        for (uint32_t i = 0; i < operation_index; i++) {
            const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
            if (candidate && candidate->source_file &&
                strcmp(candidate->source_file, operation->source_file) == 0 &&
                candidate->source_start_line == operation->source_start_line &&
                candidate->source_start_column == operation->source_start_column &&
                candidate->source_end_line == operation->source_end_line &&
                candidate->source_end_column == operation->source_end_column)
                source_discriminator++;
        }
    }
    if (!program_call || !operation || !callee_operand || !argument || !caller_function ||
        !callee_type || operation->function != caller || operation->opcode != XI_CALL ||
        operation->result_value < caller_function->value_begin ||
        operation->result_value - caller_function->value_begin >= caller_function->value_count ||
        operation->result_type != expected_type || argument->type != expected_type ||
        !source_file || !operation->source_file ||
        strcmp(operation->source_file, source_file) != 0 ||
        operation->source_line != program_call->locator.start_line ||
        operation->source_start_line != program_call->locator.start_line ||
        operation->source_start_column != program_call->locator.start_column ||
        operation->source_end_line != program_call->locator.end_line ||
        operation->source_end_column != program_call->locator.end_column ||
        operation->source_discriminator != source_discriminator || operation->allocation_key ||
        !semantic_id_is_zero(operation->allocation_id) ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL) ||
        operation->semantic_immediate != 0 || operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE || operation->evidence[0] != 0 ||
        operation->evidence[1] != 0 || operation->evidence[2] != 0 || operation->evidence[3] != 0 ||
        operation->evidence[4] != 0 || operation->evidence[5] != 0 || operation->evidence[6] != 0 ||
        operation->evidence[7] != XR_SEMANTIC_INDEX_NONE || operation->metadata_count != 0 ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        operation->transfer_mode != 0 || operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE || operation->result_alias_operand != -1 ||
        operation->return_parameter != -1 || operation->return_provenance != XR_SEM_RETURN_NONE ||
        operation->return_complete != 0 || operation->view_source_value != XR_SEMANTIC_INDEX_NONE ||
        operation->view_element_type != XR_SEMANTIC_INDEX_NONE ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        operation->view_origin != 0 || operation->view_capability != 0 ||
        operation->view_lifetime != 0 || operation->view_complete != 0 ||
        operation->array_element_storage != XR_ELEM_ANY || operation->reserved_view[0] != 0 ||
        operation->reserved_view[1] != 0 || operation->array_hof_kind != XR_SEM_ARRAY_HOF_NONE ||
        operation->array_result_element_storage != XR_ELEM_ANY ||
        callee_operand->value < caller_function->value_begin ||
        callee_operand->value - caller_function->value_begin >= caller_function->value_count ||
        callee_type->kind != XR_KIND_FUNCTION || callee_operand->role != XR_SEM_OPERAND_CALLEE ||
        callee_operand->parameter != -1 || callee_operand->flags != 0 ||
        callee_operand->transfer_mode != XR_TRANSFER_SHARE ||
        callee_operand->ownership_action != XR_SEM_OPERAND_BORROW ||
        callee_operand->parameter_mode != XR_PARAM_READ ||
        callee_operand->access != XR_CALL_ARG_PLAIN || callee_operand->origin != 0 ||
        callee_operand->lifetime != 0 || callee_operand->escape != 0 ||
        argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != 0 ||
        argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        argument->transfer_mode != XR_TRANSFER_SHARE ||
        argument->ownership_action != expected_argument_ownership ||
        argument->parameter_mode != XR_PARAM_READ || argument->access != XR_CALL_ARG_PLAIN ||
        argument->origin != 0 || argument->lifetime != 0 || argument->escape != 0)
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan call/value rows are not exact");
    if (xr_semantic_plan_call_target_count(plan) != 1)
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan call target inventory is not exact");
    const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(plan, 0);
    return (target && target->operation == operation_index && target->function == callee &&
            target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
            target->dependency == XR_SEMANTIC_INDEX_NONE &&
            target->source_export == XR_SEMANTIC_INDEX_NONE &&
            semantic_id_is_zero(target->export_identity) &&
            semantic_id_is_zero(target->callee_function) &&
            target->callable_type == XR_SEMANTIC_INDEX_NONE && target->reserved[0] == 0 &&
            target->reserved[1] == 0 && target->reserved[2] == 0) ||
           semantic_scalar_fail(error, error_size, "SemanticPlan direct call target is invalid");
}

static bool semantic_program_call_inventory_is_exact(const XrSemanticPlan *plan,
                                                     const XrSemanticProgramCallBinding *binding,
                                                     char *error, size_t error_size) {
    uint32_t call_operations = 0;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(plan); i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(plan, i);
        if (!operation)
            return semantic_scalar_fail(error, error_size,
                                        "SemanticPlan operation inventory is incomplete");
        if (xi_generated_op_class(operation->opcode) != XI_GEN_CLASS_CALL)
            continue;
        call_operations++;
        if (!binding || operation->opcode != XI_CALL || i != binding->operation)
            return semantic_scalar_fail(error, error_size,
                                        "SemanticPlan contains an unbound call operation");
    }
    return call_operations == 1 ||
           semantic_scalar_fail(error, error_size, "SemanticPlan call inventory is not exact");
}

static bool verify_scalar_semantic_plan(const XiFunc *root, const XrSemanticPlan *plan,
                                        const XrTargetProfile *target_profile, char *error,
                                        size_t error_size) {
    const XiModule *module = root ? root->module : NULL;
    const XrProgramSemanticClosure *closure = module ? module->program_semantic_closure : NULL;
    const XrTargetProfile *plan_profile = module ? module->scalar_target_profile : NULL;
    const XrScalarCallDecision *decision = module ? module->scalar_call_decision : NULL;
    if (!root || !module || module->init != root || !plan || !closure || !decision ||
        !plan_profile || !target_profile)
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan external authority is incomplete");
    XrFingerprint closure_fingerprint = xr_program_semantic_closure_fingerprint(closure);
    XrGenerationClosureId generation = xr_program_semantic_closure_generation_id(closure);
    if (!xr_target_profile_require_exact(plan_profile, target_profile, error, error_size) ||
        !xi_program_semantic_verify(module, plan_profile, error, error_size) ||
        !xr_scalar_call_decision_verify(decision, closure, plan_profile, error, error_size) ||
        !xr_semantic_plan_verify(plan, error, error_size) ||
        xr_semantic_plan_function_count(plan) != 3 ||
        plan->program_provenance.schema != XR_SEMANTIC_PROGRAM_PROVENANCE_SCHEMA_VERSION ||
        plan->program_provenance.program_schema != xr_program_semantic_closure_schema(closure) ||
        plan->program_provenance.function_count != 2 || plan->program_provenance.call_count != 1 ||
        plan->program_function_binding_count != 2 || plan->program_call_binding_count != 1 ||
        !xr_fingerprint_equal(plan->program_provenance.program_fingerprint, closure_fingerprint) ||
        memcmp(plan->program_provenance.generation_identity.bytes, generation.bytes,
               sizeof(generation.bytes)) != 0)
        return semantic_scalar_fail(error, error_size,
                                    "SemanticPlan external authority provenance is invalid");
    const XrProgramSemanticCallRecord *program_call = xr_program_semantic_closure_call(closure, 0);
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
    const XrProgramSemanticFunctionRecord *caller_program_row =
        xr_program_semantic_closure_function(closure, caller_program);
    const XrProgramSemanticFunctionRecord *callee_program_row =
        xr_program_semantic_closure_function(closure, callee_program);
    const XrSemanticFunctionRecord *caller_semantic = xr_semantic_plan_function(plan, caller);
    const XrSemanticFunctionRecord *callee_semantic = xr_semantic_plan_function(plan, callee);
    uint32_t scalar_type = caller_semantic ? caller_semantic->return_type : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticTypeRecord *scalar_type_row = xr_semantic_plan_type(plan, scalar_type);
    if (!program_call || caller_program == XR_SEMANTIC_INDEX_NONE ||
        callee_program == XR_SEMANTIC_INDEX_NONE || caller == XR_SEMANTIC_INDEX_NONE ||
        callee == XR_SEMANTIC_INDEX_NONE || !caller_binding || !callee_binding ||
        !caller_program_row || !callee_program_row || !caller_semantic || !callee_semantic ||
        !semantic_i64_type_is_exact(scalar_type_row) ||
        callee_semantic->return_type != scalar_type ||
        !xr_stable_id_equal(caller_binding->program_function, sealed->caller_function) ||
        !xr_stable_id_equal(callee_binding->program_function, sealed->callee_function) ||
        caller_binding->flags != caller_program_row->flags ||
        callee_binding->flags != callee_program_row->flags || caller == callee ||
        !semantic_function_is_exact(plan, caller, 0, scalar_type, error, error_size) ||
        !semantic_function_is_exact(plan, callee, 1, scalar_type, error, error_size) ||
        !semantic_initializer_is_exact(plan, root, error, error_size) ||
        !semantic_call_is_exact(plan, program_call, caller, callee, scalar_type, module->identity,
                                error, error_size))
        return false;
    for (uint16_t i = 0; i < module->nfuncs; i++) {
        const XiFunc *function = module->functions[i];
        uint32_t semantic = semantic_function_for_program_row(
            plan, function ? function->psc_function_index : XI_PSC_ROW_NONE);
        if (!function || semantic == XR_SEMANTIC_INDEX_NONE ||
            (function->semantic_plan && function->semantic_plan != plan) ||
            (function->semantic_plan && function->semantic_plan_function_index != semantic))
            return semantic_scalar_fail(error, error_size,
                                        "Xi and SemanticPlan function row bindings disagree");
    }
    return semantic_program_call_inventory_is_exact(plan, &plan->program_call_bindings[0], error,
                                                    error_size);
}

static bool semantic_leaf_aggregate_plan_verify(const XiFunc *root, const XrSemanticPlan *plan,
                                                 char *error, size_t error_size) {
    const XiModule *module = root ? root->module : NULL;
    const XrProgramSemanticClosure *closure = module ? module->program_semantic_closure : NULL;
    if (!root || !module || module->init != root || !closure || !plan || module->nfuncs != 2 ||
        root->nchildren != 2 || xr_semantic_plan_function_count(plan) != 3 ||
        xr_semantic_plan_source_class_count(plan) != 1 ||
        xr_program_semantic_closure_family(closure) !=
            XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL ||
        !xi_program_semantic_verify(module, NULL, error, error_size) ||
        !xr_semantic_plan_verify(plan, error, error_size))
        return semantic_scalar_fail(error, error_size,
                                    "leaf aggregate SemanticPlan authorities are incomplete");
    const XrSemanticProgramProvenance *provenance = xr_semantic_plan_program_provenance(plan);
    XrGenerationClosureId generation = xr_program_semantic_closure_generation_id(closure);
    if (!provenance || provenance->program_schema != xr_program_semantic_closure_schema(closure) ||
        provenance->program_family != XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL ||
        provenance->type_count != xr_program_semantic_closure_type_count(closure) ||
        provenance->type_field_count != xr_program_semantic_closure_type_field_count(closure) ||
        provenance->function_count != xr_program_semantic_closure_function_count(closure) ||
        provenance->call_count != xr_program_semantic_closure_call_count(closure) ||
        !xr_fingerprint_equal(provenance->program_fingerprint,
                              xr_program_semantic_closure_fingerprint(closure)) ||
        memcmp(provenance->generation_identity.bytes, generation.bytes, sizeof(generation.bytes)) !=
            0)
        return semantic_scalar_fail(error, error_size,
                                    "leaf aggregate SemanticPlan provenance does not match PSC");
    uint32_t aggregate_program = XI_PSC_ROW_NONE;
    uint32_t aggregate_semantic = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t row = 0; row < provenance->type_count; row++) {
        const XrProgramSemanticTypeRecord *program_type =
            xr_program_semantic_closure_type(closure, row);
        const XrSemanticProgramTypeBinding *binding =
            xr_semantic_plan_program_type_for_row(plan, row);
        const XrSemanticTypeRecord *semantic =
            binding ? xr_semantic_plan_type(plan, binding->semantic_type) : NULL;
        if (!program_type || !binding || !semantic ||
            !xr_stable_id_equal(binding->program_type, program_type->id) ||
            binding->kind != program_type->kind ||
            binding->exact_scalar != program_type->exact_scalar ||
            binding->flags != program_type->flags ||
            binding->field_count != program_type->field_count ||
            (program_type->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR
                 ? !semantic_id_is_zero(binding->source_class_identity)
                 : !xr_stable_id_equal(binding->source_class_identity,
                                       semantic->source_class_identity)))
            return semantic_scalar_fail(
                error, error_size, "leaf aggregate SemanticPlan type binding disagrees with PSC");
        if (program_type->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE) {
            if (aggregate_program != XI_PSC_ROW_NONE)
                return semantic_scalar_fail(error, error_size, "multiple aggregate type bindings");
            aggregate_program = row;
            aggregate_semantic = binding->semantic_type;
        }
        for (uint32_t field = 0; field < program_type->field_count; field++) {
            const XrProgramSemanticTypeFieldRecord *program_field =
                xr_program_semantic_closure_type_field(closure, program_type->field_begin + field);
            const XrSemanticProgramTypeFieldBinding *semantic_field =
                xr_semantic_plan_program_type_field_binding(plan, binding->field_begin + field);
            if (!program_field || !semantic_field || semantic_field->owner_program_row != row ||
                semantic_field->declaration_ordinal != field ||
                !xr_stable_id_equal(semantic_field->program_owner_type, program_type->id) ||
                !xr_stable_id_equal(semantic_field->program_field_type, program_field->field_type))
                return semantic_scalar_fail(
                    error, error_size,
                    "leaf aggregate SemanticPlan field binding disagrees with PSC");
        }
    }
    if (aggregate_program == XI_PSC_ROW_NONE || aggregate_semantic == XR_SEMANTIC_INDEX_NONE)
        return semantic_scalar_fail(error, error_size, "aggregate type binding is missing");
    const XrSemanticProgramTypeBinding *aggregate_binding =
        xr_semantic_plan_program_type_for_row(plan, aggregate_program);
    const XrSemanticTypeRecord *aggregate_type = xr_semantic_plan_type(plan, aggregate_semantic);
    const XiClassData *aggregate_source =
        module->nclasses == 1 && module->classes ? module->classes[0] : NULL;
    const XrSemanticSourceClassRecord *source_class = xr_semantic_plan_source_class(plan, 0);
    if (!aggregate_binding || !aggregate_type || !aggregate_source || !source_class ||
        aggregate_source->psc_type_index != aggregate_program ||
        aggregate_type->source_class != 0 ||
        !xr_stable_id_equal(aggregate_type->source_class_identity, source_class->id) ||
        !xr_stable_id_equal(aggregate_binding->source_class_identity, source_class->id) ||
        !semantic_source_class_is_exact(plan, root, aggregate_source, source_class))
        return semantic_scalar_fail(error, error_size,
                                    "aggregate source declaration identity is not exact");
    for (uint16_t i = 0; i < module->nfuncs; i++) {
        const XiFunc *function = module->functions[i];
        uint32_t semantic_function = semantic_function_for_program_row(
            plan, function ? function->psc_function_index : XI_PSC_ROW_NONE);
        const XrProgramSemanticFunctionRecord *program_function =
            function ? xr_program_semantic_closure_function(closure, function->psc_function_index)
                     : NULL;
        const XrSemanticProgramFunctionBinding *function_binding =
            function ? semantic_binding_for_program_row(plan, function->psc_function_index) : NULL;
        const XrSemanticFunctionRecord *record = xr_semantic_plan_function(plan, semantic_function);
        const XrSemanticParameterRecord *parameter =
            record && record->parameter_count == 1
                ? xr_semantic_plan_parameter(plan, record->parameter_begin)
                : NULL;
        if (!function || !program_function || !function_binding ||
            semantic_function == XR_SEMANTIC_INDEX_NONE || !record)
            return semantic_scalar_fail(error, error_size,
                                        "aggregate function binding inventory is incomplete");
        if (function->psc_return_type_index != aggregate_program ||
            !xr_stable_id_equal(function_binding->program_function, program_function->id) ||
            function_binding->flags != program_function->flags ||
            record->return_type != aggregate_semantic ||
            record->parameter_count != function->nparams)
            return semantic_scalar_fail(error, error_size,
                                        "Xi and SemanticPlan aggregate function bindings disagree");
        if (!semantic_function_is_exact(plan, semantic_function, function->nparams,
                                        aggregate_semantic, error, error_size))
            return false;
        if (function->nparams == 1 &&
            (!function->params || function->params[0]->psc_type_index != aggregate_program ||
             !parameter || parameter->type != aggregate_semantic))
            return semantic_scalar_fail(error, error_size,
                                        "aggregate parameter binding is not exact");
    }
    const XrProgramSemanticCallRecord *program_call = xr_program_semantic_closure_call(closure, 0);
    const XrSemanticProgramCallBinding *call_binding =
        plan->program_call_binding_count == 1 ? &plan->program_call_bindings[0] : NULL;
    if (!program_call || !call_binding ||
        !xr_stable_id_equal(call_binding->program_call, program_call->id) ||
        !xr_stable_id_equal(call_binding->callsite, program_call->callsite_identity) ||
        !xr_stable_id_equal(call_binding->caller_program_function, program_call->caller_function) ||
        !xr_stable_id_equal(call_binding->callee_program_function, program_call->callee_function))
        return semantic_scalar_fail(error, error_size,
                                    "leaf aggregate SemanticPlan call binding is not exact");
    const XrSemanticProgramFunctionBinding *caller_binding = NULL;
    const XrSemanticProgramFunctionBinding *callee_binding = NULL;
    for (uint32_t i = 0; i < plan->program_function_binding_count; i++) {
        const XrSemanticProgramFunctionBinding *binding = &plan->program_function_bindings[i];
        if (xr_stable_id_equal(binding->program_function, program_call->caller_function))
            caller_binding = binding;
        if (xr_stable_id_equal(binding->program_function, program_call->callee_function))
            callee_binding = binding;
    }
    if (!caller_binding || !callee_binding ||
        !semantic_initializer_is_exact(plan, root, error, error_size) ||
        !semantic_call_is_exact(plan, program_call, caller_binding->semantic_function,
                                callee_binding->semantic_function, aggregate_semantic,
                                module->identity, error, error_size))
        return false;
    return semantic_program_call_inventory_is_exact(plan, call_binding, error, error_size);
}

static const XrSemanticOperationRecord *semantic_operation_for_xi_value(
    const XrSemanticPlan *plan, const XiFunc *function, uint32_t semantic_function,
    const XiValue *value, uint32_t *out_index) {
    const XrSemanticFunctionRecord *record =
        xr_semantic_plan_function(plan, semantic_function);
    if (out_index)
        *out_index = XR_SEMANTIC_INDEX_NONE;
    if (!plan || !function || !record || !value || record->block_count != function->nblocks)
        return NULL;
    for (uint32_t block_index = 0; block_index < function->nblocks; block_index++) {
        const XiBlock *block = function->blocks[block_index];
        const XrSemanticBlockRecord *semantic_block =
            xr_semantic_plan_block(plan, record->block_begin + block_index);
        uint32_t phi_count = 0;
        for (const XiPhi *phi = block ? block->phis : NULL; phi; phi = phi->next)
            phi_count++;
        if (!block || !semantic_block ||
            semantic_block->operation_count != phi_count + block->nvalues)
            return NULL;
        for (uint32_t i = 0; i < block->nvalues; i++) {
            if (block->values[i] != value)
                continue;
            uint32_t operation = semantic_block->operation_begin + phi_count + i;
            if (out_index)
                *out_index = operation;
            return xr_semantic_plan_operation(plan, operation);
        }
    }
    return NULL;
}

static bool semantic_product_operation_is_exact(
    const XrSemanticPlan *plan, const XiFunc *function, uint32_t semantic_function,
    const XiValue *value, uint32_t product_type, const uint32_t member_types[6]) {
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *operation = semantic_operation_for_xi_value(
        plan, function, semantic_function, value, &operation_index);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    bool construct = value && value->op == XI_VALUE_PRODUCT_CONSTRUCT;
    uint32_t expected_operands = construct ? 6u : 1u;
    uint32_t ordinal = construct ? 0u : (uint32_t) value->aux_int;
    if (!operation || !operands || (!construct && value->op != XI_VALUE_PRODUCT_PROJECT) ||
        (!construct && (value->aux_int < 0 || value->aux_int >= 6)) ||
        value->nargs != expected_operands || operation->opcode != value->op ||
        operation->function != semantic_function ||
        operation->result_type != (construct ? product_type : member_types[ordinal]) ||
        operation->operand_count != expected_operands ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->semantic_immediate != (construct ? 6 : value->aux_int) ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->effects != xi_generated_op_effects(value->op) ||
        operation->flags != xi_generated_op_default_flags(value->op) ||
        operation->ownership_use != xi_generated_op_own_use(value->op) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->constant != XR_SEMANTIC_INDEX_NONE || operation->allocation_key ||
        !semantic_id_is_zero(operation->allocation_id) ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->return_provenance != XR_SEM_RETURN_NONE ||
        operation->metadata_count != 0 || operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE)
        return false;
    for (uint32_t operand = 0; operand < expected_operands; operand++) {
        const XrSemanticOperationRecord *source = semantic_operation_for_xi_value(
            plan, function, semantic_function, value->args[operand], NULL);
        const XrSemanticOperandRecord *semantic_operand =
            &operands[operation->operand_begin + operand];
        if (!source || semantic_operand->value != source->result_value ||
            semantic_operand->type !=
                (construct ? member_types[operand] : product_type) ||
            semantic_operand->role != XR_SEM_OPERAND_VALUE ||
            semantic_operand->parameter != -1 || semantic_operand->flags != 0 ||
            semantic_operand->ownership_action != XR_SEM_OPERAND_BORROW)
            return false;
    }
    (void) operation_index;
    return true;
}

static bool semantic_product_call_is_exact(
    const XrSemanticPlan *plan, const XiFunc *function, uint32_t semantic_function,
    const XiValue *value, const XrProgramSemanticCallRecord *program_call,
    uint32_t product_type, uint32_t callee_function) {
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *operation = semantic_operation_for_xi_value(
        plan, function, semantic_function, value, &operation_index);
    const XrSemanticProgramCallBinding *binding =
        xr_semantic_plan_program_call_for_operation(plan, operation_index);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const XrSemanticOperandRecord *callee =
        operation && operation->operand_count == 1 &&
                operation->operand_begin < operand_count
            ? &operands[operation->operand_begin]
            : NULL;
    const XrSemanticCallTargetRecord *target = NULL;
    for (uint32_t i = 0; operation && i < plan->call_target_count; i++) {
        const XrSemanticCallTargetRecord *candidate = &plan->call_targets[i];
        if (candidate->operation != operation_index)
            continue;
        if (target)
            return false;
        target = candidate;
    }
    return program_call && operation && binding && callee && target && value->nargs == 1 &&
           binding->program_row == value->psc_call_index &&
           binding->target_function == callee_function && binding->reserved == 0 &&
           xr_stable_id_equal(binding->program_call, program_call->id) &&
           xr_stable_id_equal(binding->callsite, program_call->callsite_identity) &&
           xr_stable_id_equal(binding->caller_program_function,
                              program_call->caller_function) &&
           xr_stable_id_equal(binding->callee_program_function,
                              program_call->callee_function) &&
           semantic_id_is_zero(binding->resolver_binding) &&
           binding->program_dependency == XR_SEMANTIC_INDEX_NONE &&
           operation->opcode == XI_CALL && operation->function == semantic_function &&
           operation->result_type == product_type && operation->operand_count == 1 &&
           operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT &&
           operation->result_alias_operand == -1 && operation->return_parameter == -1 &&
           operation->return_provenance == XR_SEM_RETURN_NONE &&
           operation->auxiliary_kind == XI_AUX_KIND_NONE &&
           operation->effects == xi_generated_op_effects(XI_CALL) &&
           operation->flags == xi_generated_op_default_flags(XI_CALL) &&
           callee->role == XR_SEM_OPERAND_CALLEE && callee->parameter == -1 &&
           callee->flags == 0 && target->function == callee_function &&
           target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
           target->dependency == XR_SEMANTIC_INDEX_NONE &&
           target->source_export == XR_SEMANTIC_INDEX_NONE &&
           target->callable_type == XR_SEMANTIC_INDEX_NONE;
}

static bool semantic_leaf_product_plan_verify(const XiFunc *root,
                                              const XrSemanticPlan *plan,
                                              char *error, size_t error_size) {
    const XiModule *module = root ? root->module : NULL;
    const XrProgramSemanticClosure *closure = module ? module->program_semantic_closure : NULL;
    const XrSemanticProgramProvenance *provenance =
        plan ? xr_semantic_plan_program_provenance(plan) : NULL;
    XrGenerationClosureId generation = closure ? xr_program_semantic_closure_generation_id(closure)
                                               : (XrGenerationClosureId) {{0}};
    if (!root || !module || module->init != root || !closure || !plan || module->nfuncs != 3 ||
        root->nchildren != 3 || xr_semantic_plan_function_count(plan) != 4 ||
        xr_semantic_plan_source_class_count(plan) != 0 ||
        xr_program_semantic_closure_family(closure) !=
            XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL ||
        !xi_program_semantic_verify(module, NULL, error, error_size) ||
        !xr_semantic_plan_verify(plan, error, error_size) || !provenance ||
        provenance->program_family != XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL ||
        provenance->type_count != 3 || provenance->type_field_count != 6 ||
        provenance->function_count != 3 || provenance->call_count != 2 ||
        provenance->module_count != 1 || provenance->dependency_count != 0 ||
        plan->program_type_binding_count != 3 || plan->program_type_field_binding_count != 6 ||
        plan->program_function_binding_count != 3 || plan->program_call_binding_count != 2 ||
        plan->program_dependency_binding_count != 0 ||
        !xr_fingerprint_equal(provenance->program_fingerprint,
                              xr_program_semantic_closure_fingerprint(closure)) ||
        memcmp(provenance->generation_identity.bytes, generation.bytes,
               sizeof(generation.bytes)) != 0 ||
        !semantic_initializer_is_exact(plan, root, error, error_size))
        return semantic_scalar_fail(error, error_size,
                                    "leaf product SemanticPlan authority is incomplete");

    const XrSemanticProgramTypeBinding *product = NULL;
    const XrSemanticProgramTypeBinding *i64 = NULL;
    const XrSemanticProgramTypeBinding *u8 = NULL;
    for (uint32_t row = 0; row < 3; row++) {
        const XrProgramSemanticTypeRecord *program_type =
            xr_program_semantic_closure_type(closure, row);
        const XrSemanticProgramTypeBinding *binding =
            xr_semantic_plan_program_type_for_row(plan, row);
        const XrSemanticTypeRecord *type =
            binding ? xr_semantic_plan_type(plan, binding->semantic_type) : NULL;
        if (!program_type || !binding || !type || binding->program_row != row ||
            !xr_stable_id_equal(binding->program_type, program_type->id) ||
            binding->kind != program_type->kind || binding->flags != program_type->flags)
            return semantic_scalar_fail(error, error_size,
                                        "leaf product type binding disagrees with PSC");
        if (binding->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT) {
            if (product || binding->field_count != 6 || type->kind != XR_KIND_TUPLE ||
                type->child_count != 6 || type->aggregate_extent != 6 ||
                type->flags != XR_SEM_TYPE_VALUE ||
                !semantic_id_is_zero(binding->source_class_identity))
                return semantic_scalar_fail(error, error_size,
                                            "leaf product type shape is not exact");
            product = binding;
        } else if (binding->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR &&
                   binding->exact_scalar == XR_EXACT_SCALAR_I64 &&
                   semantic_i64_type_is_exact(type)) {
            if (i64)
                return semantic_scalar_fail(error, error_size, "duplicate i64 product member");
            i64 = binding;
        } else if (binding->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR &&
                   binding->exact_scalar == XR_EXACT_SCALAR_U8 && type->kind == XR_KIND_INT &&
                   type->scalar_rep == XR_NATIVE_U8 && type->child_count == 0 &&
                   type->flags == 0) {
            if (u8)
                return semantic_scalar_fail(error, error_size, "duplicate u8 product member");
            u8 = binding;
        } else {
            return semantic_scalar_fail(error, error_size,
                                        "leaf product scalar inventory is not exact");
        }
    }
    if (!product || !i64 || !u8)
        return semantic_scalar_fail(error, error_size, "leaf product types are incomplete");
    uint32_t member_types[6] = {i64->semantic_type, i64->semantic_type, u8->semantic_type,
                                i64->semantic_type, i64->semantic_type, i64->semantic_type};
    for (uint32_t ordinal = 0; ordinal < 6; ordinal++) {
        const XrSemanticProgramTypeFieldBinding *field =
            xr_semantic_plan_program_type_field_binding(plan, product->field_begin + ordinal);
        const XrSemanticProgramTypeBinding *member = ordinal == 2 ? u8 : i64;
        if (!field || field->owner_program_row != product->program_row ||
            field->field_program_row != member->program_row ||
            field->semantic_field_type != member->semantic_type ||
            field->declaration_ordinal != ordinal ||
            !xr_stable_id_equal(field->program_owner_type, product->program_type) ||
            !xr_stable_id_equal(field->program_field_type, member->program_type))
            return semantic_scalar_fail(error, error_size,
                                        "leaf product member ordinal is not exact");
    }

    uint32_t construct_count = 0, project_count = 0, call_count = 0;
    uint32_t callee_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t entry_count = 0;
    for (uint16_t f = 0; f < module->nfuncs; f++) {
        const XiFunc *function = module->functions[f];
        const XrSemanticProgramFunctionBinding *binding =
            function ? semantic_binding_for_program_row(plan, function->psc_function_index) : NULL;
        if (!binding)
            return semantic_scalar_fail(error, error_size,
                                        "leaf product function binding is missing");
        if (binding->flags == 0) {
            if (callee_function != XR_SEMANTIC_INDEX_NONE)
                return semantic_scalar_fail(error, error_size,
                                            "leaf product callee is ambiguous");
            callee_function = binding->semantic_function;
        } else if (binding->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) {
            entry_count++;
        } else {
            return semantic_scalar_fail(error, error_size,
                                        "leaf product function role is invalid");
        }
    }
    if (callee_function == XR_SEMANTIC_INDEX_NONE || entry_count != 2)
        return semantic_scalar_fail(error, error_size,
                                    "leaf product function roles are incomplete");
    for (uint16_t f = 0; f < module->nfuncs; f++) {
        const XiFunc *function = module->functions[f];
        const XrProgramSemanticFunctionRecord *program_function =
            function ? xr_program_semantic_closure_function(closure,
                                                             function->psc_function_index)
                     : NULL;
        const XrSemanticProgramFunctionBinding *binding =
            function ? semantic_binding_for_program_row(plan, function->psc_function_index) : NULL;
        uint32_t semantic_function = binding ? binding->semantic_function : XR_SEMANTIC_INDEX_NONE;
        if (!function || !program_function || !binding || function->nparams != 0 ||
            function->psc_return_type_index != product->program_row ||
            !xr_stable_id_equal(binding->program_function, program_function->id) ||
            binding->flags != program_function->flags ||
            !semantic_function_is_exact(plan, semantic_function, 0, product->semantic_type,
                                        error, error_size))
            return false;
        for (uint32_t b = 0; b < function->nblocks; b++) {
            const XiBlock *block = function->blocks[b];
            for (uint32_t v = 0; block && v < block->nvalues; v++) {
                const XiValue *value = block->values[v];
                if (value->op == XI_VALUE_PRODUCT_CONSTRUCT) {
                    if (!semantic_product_operation_is_exact(
                            plan, function, semantic_function, value, product->semantic_type,
                            member_types))
                        return semantic_scalar_fail(error, error_size,
                                                    "product construct proof is not exact");
                    construct_count++;
                } else if (value->op == XI_VALUE_PRODUCT_PROJECT) {
                    if (!semantic_product_operation_is_exact(
                            plan, function, semantic_function, value, product->semantic_type,
                            member_types))
                        return semantic_scalar_fail(error, error_size,
                                                    "product project proof is not exact");
                    project_count++;
                } else if (value->op == XI_CALL) {
                    const XrProgramSemanticCallRecord *program_call =
                        xr_program_semantic_closure_call(closure, value->psc_call_index);
                    if (callee_function == XR_SEMANTIC_INDEX_NONE || !program_call ||
                        !semantic_product_call_is_exact(
                            plan, function, semantic_function, value, program_call,
                            product->semantic_type, callee_function))
                        return semantic_scalar_fail(error, error_size,
                                                    "leaf product call proof is not exact");
                    call_count++;
                } else if (value->op == XI_TUPLE_NEW || value->op == XI_TUPLE_GET) {
                    return semantic_scalar_fail(error, error_size,
                                                "managed tuple authority entered leaf product");
                }
            }
        }
    }
    return (callee_function != XR_SEMANTIC_INDEX_NONE && construct_count == 3 &&
            project_count == 12 && call_count == 2 && plan->call_target_count == 2) ||
           semantic_scalar_fail(error, error_size,
                                "leaf product proof inventory is not exact");
}

static bool semantic_detached_source_module_is_exact(
    const XiModule *module, const XrProgramSemanticClosure *closure) {
    const XrProgramSemanticModuleRecord *program_module =
        closure && xr_program_semantic_closure_module_count(closure) == 1
            ? xr_program_semantic_closure_module(closure, 0)
            : NULL;
    const XrProgramSemanticModuleInput *source =
        module && module->source_semantic_module_present ? &module->source_semantic_module : NULL;
    return program_module && source &&
           xr_stable_id_equal(program_module->module_identity, source->module_identity) &&
           xr_fingerprint_equal(program_module->module_authority_fingerprint,
                                source->module_authority_fingerprint) &&
           xr_fingerprint_equal(program_module->source_fingerprint, source->source_fingerprint) &&
           xr_fingerprint_equal(program_module->export_fingerprint, source->export_fingerprint);
}

bool xi_program_semantic_plan_verify_detached_leaf_authority(
    const XiFunc *root, const XrSemanticPlan *plan, char *error, size_t error_size) {
    const XiModule *module = root ? root->module : NULL;
    const XrProgramSemanticClosure *closure = module ? module->program_semantic_closure : NULL;
    if (error && error_size)
        error[0] = '\0';
    if (!root || !root->semantic_snapshot_detached || !module || module->init != root)
        return semantic_scalar_fail(error, error_size,
                                    "detached leaf Xi root authority is incomplete");
    if (!closure || !xr_program_semantic_closure_is_frozen(closure) ||
        !xr_program_semantic_closure_is_verified(closure))
        return semantic_scalar_fail(error, error_size,
                                    "detached leaf PSC state is incomplete");
    if (!xr_program_semantic_closure_verify(closure, error, error_size))
        return false;
    uint32_t family = xr_program_semantic_closure_family(closure);
    if (xr_program_semantic_closure_schema(closure) !=
            XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION ||
        (family != XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL &&
         family != XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL))
        return semantic_scalar_fail(error, error_size,
                                    "detached leaf PSC schema or family is not exact");
    if (!semantic_detached_source_module_is_exact(module, closure))
        return semantic_scalar_fail(error, error_size,
                                    "detached leaf source-module authority disagrees with PSC");
    if (!plan)
        return semantic_scalar_fail(error, error_size,
                                    "detached leaf SemanticPlan is missing");
    if (!xr_semantic_plan_verify(plan, error, error_size))
        return false;
    if (family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL)
        return semantic_leaf_product_plan_verify(root, plan, error, error_size);

    size_t function_count = xr_program_semantic_closure_function_count(closure);
    if (function_count > UINT16_MAX || module->nfuncs != function_count ||
        root->nchildren != function_count || root->psc_function_index != XI_PSC_ROW_NONE ||
        root->psc_return_type_index != XI_PSC_ROW_NONE ||
        xr_semantic_plan_function_count(plan) != function_count + 1)
        return semantic_scalar_fail(error, error_size,
                                    "detached leaf Xi module inventory is not exact");
    for (uint16_t i = 0; i < module->nfuncs; i++) {
        const XiFunc *function = module->functions ? module->functions[i] : NULL;
        uint16_t child_matches = 0;
        for (uint16_t child = 0; child < root->nchildren; child++)
            if (root->children && root->children[child] == function)
                child_matches++;
        if (!function || function->parent_func != root || function->semantic_snapshot_detached == 0 ||
            child_matches != 1 ||
            !xr_program_semantic_closure_function(closure, function->psc_function_index))
            return semantic_scalar_fail(error, error_size,
                                        "detached leaf Xi function inventory is not exact");
        for (uint16_t previous = 0; previous < i; previous++)
            if (module->functions[previous] &&
                module->functions[previous]->psc_function_index == function->psc_function_index)
                return semantic_scalar_fail(error, error_size,
                                            "detached leaf Xi function rows are duplicated");
    }

    const XrSemanticProgramProvenance *provenance = xr_semantic_plan_program_provenance(plan);
    XrGenerationClosureId generation = xr_program_semantic_closure_generation_id(closure);
    if (!provenance ||
        provenance->schema != XR_SEMANTIC_PROGRAM_PROVENANCE_SCHEMA_VERSION ||
        provenance->program_schema != xr_program_semantic_closure_schema(closure) ||
        provenance->program_family !=
            XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL ||
        provenance->type_count != xr_program_semantic_closure_type_count(closure) ||
        provenance->type_field_count != xr_program_semantic_closure_type_field_count(closure) ||
        provenance->function_count != function_count ||
        provenance->call_count != xr_program_semantic_closure_call_count(closure) ||
        xr_semantic_plan_program_type_binding_count(plan) != provenance->type_count ||
        xr_semantic_plan_program_type_field_binding_count(plan) != provenance->type_field_count ||
        xr_semantic_plan_program_function_binding_count(plan) != provenance->function_count ||
        xr_semantic_plan_program_call_binding_count(plan) != provenance->call_count ||
        !xr_fingerprint_equal(provenance->program_fingerprint,
                              xr_program_semantic_closure_fingerprint(closure)) ||
        memcmp(provenance->generation_identity.bytes, generation.bytes, sizeof(generation.bytes)) !=
            0)
        return semantic_scalar_fail(error, error_size,
                                    "detached leaf SemanticPlan provenance disagrees with PSC");
    return true;
}

static bool semantic_graph_call_inventory_is_exact(
    const XrSemanticPlan *plan, const XrSemanticProgramCallBinding *binding, uint32_t expected,
    char *error, size_t error_size) {
    uint32_t count = 0;
    for (uint32_t i = 0; plan && i < plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &plan->operations[i];
        if (xi_generated_op_class(operation->opcode) != XI_GEN_CLASS_CALL)
            continue;
        count++;
        if (!binding || expected != 1 || operation->opcode != XI_CALL ||
            binding->operation != i)
            return semantic_scalar_fail(error, error_size,
                                        "graph SemanticPlan has an unbound call operation");
    }
    return count == expected ||
           semantic_scalar_fail(error, error_size,
                                "graph SemanticPlan call inventory is not exact");
}

static bool semantic_graph_partition_plan_verify(const XiFunc *root, const XrSemanticPlan *plan,
                                                 char *error, size_t error_size) {
    const XiModule *module = root ? root->module : NULL;
    const XrProgramSemanticClosure *closure = module ? module->program_semantic_closure : NULL;
    const XrSemanticProgramProvenance *provenance =
        plan ? xr_semantic_plan_program_provenance(plan) : NULL;
    const XrProgramSemanticModuleRecord *program_module =
        closure && module && module->psc_module_index <
                                 xr_program_semantic_closure_module_count(closure)
            ? xr_program_semantic_closure_module(closure, module->psc_module_index)
            : NULL;
    XrGenerationClosureId generation = closure ? xr_program_semantic_closure_generation_id(closure)
                                               : (XrGenerationClosureId) {{0}};
    if (!root || !module || module->init != root || !plan || !closure || !provenance ||
        !program_module || module->nfuncs != 1 || !module->functions ||
        !root->children || module->functions[0] != root->children[0] ||
        !xi_program_semantic_verify_partition(module, NULL, error, error_size) ||
        !xr_semantic_plan_verify(plan, error, error_size) ||
        provenance->schema != XR_SEMANTIC_PROGRAM_PROVENANCE_SCHEMA_VERSION ||
        provenance->program_schema != xr_program_semantic_closure_schema(closure) ||
        provenance->program_family !=
            XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL ||
        provenance->type_count != xr_program_semantic_closure_type_count(closure) ||
        provenance->type_field_count != xr_program_semantic_closure_type_field_count(closure) ||
        provenance->function_count != xr_program_semantic_closure_function_count(closure) ||
        provenance->call_count != xr_program_semantic_closure_call_count(closure) ||
        provenance->module_count != xr_program_semantic_closure_module_count(closure) ||
        provenance->dependency_count != xr_program_semantic_closure_dependency_count(closure) ||
        provenance->program_module_row != module->psc_module_index ||
        !xr_stable_id_equal(provenance->program_module, program_module->module_identity) ||
        !xr_fingerprint_equal(provenance->program_fingerprint,
                              xr_program_semantic_closure_fingerprint(closure)) ||
        memcmp(provenance->generation_identity.bytes, generation.bytes, sizeof(generation.bytes)) !=
            0 ||
        plan->program_type_binding_count != 1 ||
        plan->program_function_binding_count != 1 || plan->function_count != 2)
        return semantic_scalar_fail(error, error_size,
                                    "graph SemanticPlan partition authority is incomplete");

    const XrProgramSemanticTypeRecord *program_type =
        xr_program_semantic_closure_type(closure, 0);
    const XrSemanticProgramTypeBinding *type_binding =
        xr_semantic_plan_program_type_for_row(plan, 0);
    const XrSemanticTypeRecord *semantic_type =
        type_binding ? xr_semantic_plan_type(plan, type_binding->semantic_type) : NULL;
    const XiFunc *local = module->functions[0];
    const XrProgramSemanticFunctionRecord *program_function =
        xr_program_semantic_closure_function(closure, local->psc_function_index);
    const XrSemanticProgramFunctionBinding *function_binding =
        semantic_binding_for_program_row(plan, local->psc_function_index);
    uint32_t semantic_function = function_binding ? function_binding->semantic_function
                                                  : XR_SEMANTIC_INDEX_NONE;
    if (!program_type || !type_binding || !semantic_i64_type_is_exact(semantic_type) ||
        type_binding->program_row != 0 ||
        !xr_stable_id_equal(type_binding->program_type, program_type->id) ||
        type_binding->kind != XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR ||
        type_binding->exact_scalar != XR_EXACT_SCALAR_I64 || !program_function ||
        !function_binding ||
        !xr_stable_id_equal(function_binding->program_function, program_function->id) ||
        function_binding->flags != program_function->flags ||
        local->psc_return_type_index != 0 ||
        !semantic_source_module_key_matches_root(plan, root) ||
        (root->semantic_plan &&
         (root->semantic_plan != plan || root->semantic_plan_function_index != 0)) ||
        (local->semantic_plan && local->semantic_plan != plan) ||
        (local->semantic_plan && local->semantic_plan_function_index != semantic_function) ||
        !semantic_initializer_is_exact(plan, root, error, error_size) ||
        !semantic_function_is_exact(plan, semantic_function, local->nparams,
                                    type_binding->semantic_type, error, error_size))
        return false;

    bool entry = program_function->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY;
    uint32_t expected_calls = entry ? 1u : 0u;
    if ((!entry && program_function->flags != XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED) ||
        plan->program_call_binding_count != expected_calls ||
        plan->program_dependency_binding_count != expected_calls ||
        plan->dependency_count != expected_calls ||
        !semantic_graph_call_inventory_is_exact(
            plan, expected_calls ? &plan->program_call_bindings[0] : NULL, expected_calls, error,
            error_size))
        return semantic_scalar_fail(error, error_size,
                                    "graph SemanticPlan local role is not exact");
    if (!entry)
        return plan->source_export_count == 1 ||
               semantic_scalar_fail(error, error_size,
                                    "graph producer export authority is incomplete");

    const XrProgramSemanticDependencyRecord *program_dependency =
        xr_program_semantic_closure_dependency(closure, 0);
    const XrProgramSemanticCallRecord *program_call =
        xr_program_semantic_closure_call(closure, 0);
    const XrSemanticProgramDependencyBinding *dependency =
        &plan->program_dependency_bindings[0];
    const XrSemanticProgramCallBinding *call = &plan->program_call_bindings[0];
    const XrSemanticOperationRecord *operation =
        call->operation < plan->operation_count ? &plan->operations[call->operation] : NULL;
    const XrSemanticCallTargetRecord *target = NULL;
    for (uint32_t i = 0; operation && i < plan->call_target_count; i++) {
        const XrSemanticCallTargetRecord *candidate = &plan->call_targets[i];
        if (candidate->operation != call->operation ||
            candidate->kind != XR_SEM_CALL_TARGET_SOURCE_EXPORT)
            continue;
        if (target)
            return semantic_scalar_fail(error, error_size,
                                        "graph SemanticPlan source target is ambiguous");
        target = candidate;
    }
    if (!program_dependency || !program_call || dependency->program_row != 0 ||
        dependency->semantic_dependency != 0 || dependency->reserved != 0 ||
        !xr_stable_id_equal(dependency->resolver_binding,
                            program_dependency->resolver_binding) ||
        call->program_row != 0 || call->program_dependency != 0 ||
        call->target_function != XR_SEMANTIC_INDEX_NONE || call->reserved != 0 ||
        !xr_stable_id_equal(call->program_call, program_call->id) ||
        !xr_stable_id_equal(call->callsite, program_call->callsite_identity) ||
        !xr_stable_id_equal(call->caller_program_function, program_call->caller_function) ||
        !xr_stable_id_equal(call->callee_program_function, program_call->callee_function) ||
        !xr_stable_id_equal(call->resolver_binding, program_call->resolver_binding) ||
        !xr_stable_id_equal(call->resolver_binding, dependency->resolver_binding) || !operation ||
        operation->opcode != XI_CALL || operation->function != semantic_function ||
        operation->result_type != type_binding->semantic_type ||
        !operation->source_file ||
        operation->source_start_line != program_call->locator.start_line ||
        operation->source_start_column != program_call->locator.start_column ||
        operation->source_end_line != program_call->locator.end_line ||
        operation->source_end_column != program_call->locator.end_column || !target ||
        target->dependency != dependency->semantic_dependency)
        return semantic_scalar_fail(error, error_size,
                                    "graph SemanticPlan external call join is not exact");
    return true;
}

static const XiFunc *private_leaf_local_function(const XiModule *module) {
    const XiFunc *match = NULL;
    for (uint16_t i = 0; module && i < module->nfuncs; i++) {
        const XiFunc *candidate = module->functions ? module->functions[i] : NULL;
        if (!candidate || candidate->psc_function_index == XI_PSC_ROW_NONE)
            continue;
        if (match)
            return NULL;
        match = candidate;
    }
    return match;
}

static const XrSemanticSourceExportRecord *private_leaf_semantic_export(
    const XrSemanticPlan *plan, uint32_t function) {
    const XrSemanticSourceExportRecord *match = NULL;
    for (uint32_t i = 0; plan && i < plan->source_export_count; i++) {
        const XrSemanticSourceExportRecord *candidate = &plan->source_exports[i];
        if (candidate->kind != XR_SEM_SOURCE_EXPORT_FUNCTION ||
            candidate->function != function)
            continue;
        if (match)
            return NULL;
        match = candidate;
    }
    return match;
}

static bool semantic_private_leaf_call_inventory_is_exact(
    const XrSemanticPlan *plan, uint32_t function,
    const XrSemanticProgramCallBinding *binding, char *error, size_t error_size) {
    uint32_t count = 0;
    for (uint32_t i = 0; plan && i < plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &plan->operations[i];
        if (operation->function != function ||
            xi_generated_op_class(operation->opcode) != XI_GEN_CLASS_CALL)
            continue;
        count++;
        if (!binding || operation->opcode != XI_CALL || binding->operation != i)
            return semantic_scalar_fail(
                error, error_size,
                "private-leaf SemanticPlan contains an unbound call operation");
    }
    return count == 1 ||
           semantic_scalar_fail(error, error_size,
                                "private-leaf SemanticPlan call inventory is not exact");
}

static bool semantic_private_leaf_partition_plan_verify(const XiFunc *root,
                                                        const XrSemanticPlan *plan, char *error,
                                                        size_t error_size) {
    const XiModule *module = root ? root->module : NULL;
    const XrProgramSemanticClosure *closure = module ? module->program_semantic_closure : NULL;
    const XrSemanticProgramProvenance *provenance =
        plan ? xr_semantic_plan_program_provenance(plan) : NULL;
    const XrProgramSemanticModuleRecord *program_module =
        closure && module && module->psc_module_index <
                                 xr_program_semantic_closure_module_count(closure)
            ? xr_program_semantic_closure_module(closure, module->psc_module_index)
            : NULL;
    XrGenerationClosureId generation = closure ? xr_program_semantic_closure_generation_id(closure)
                                               : (XrGenerationClosureId) {{0}};
    if (!root || !module || module->init != root || !plan || !closure || !provenance ||
        !program_module || !xi_program_semantic_verify_partition(module, NULL, error, error_size) ||
        !xr_semantic_plan_verify(plan, error, error_size) ||
        provenance->schema != XR_SEMANTIC_PROGRAM_PROVENANCE_SCHEMA_VERSION ||
        provenance->program_schema != xr_program_semantic_closure_schema(closure) ||
        provenance->program_family !=
            XR_PROGRAM_SEMANTIC_FAMILY_SOURCE_MODULE_SCALAR_PRIVATE_LEAF_CALL ||
        provenance->type_count != 1 || provenance->type_field_count != 0 ||
        provenance->function_count != 2 || provenance->call_count != 2 ||
        provenance->module_count != 2 || provenance->dependency_count != 1 ||
        provenance->program_module_row != module->psc_module_index ||
        !xr_stable_id_equal(provenance->program_module, program_module->module_identity) ||
        !xr_fingerprint_equal(provenance->program_fingerprint,
                              xr_program_semantic_closure_fingerprint(closure)) ||
        memcmp(provenance->generation_identity.bytes, generation.bytes, sizeof(generation.bytes)) !=
            0 ||
        plan->program_type_binding_count != 1 ||
        plan->program_function_binding_count != 1 || plan->program_call_binding_count != 1)
        return semantic_scalar_fail(error, error_size,
                                    "private-leaf SemanticPlan authority is incomplete");

    const XiFunc *local = private_leaf_local_function(module);
    const XrProgramSemanticFunctionRecord *program_function =
        local ? xr_program_semantic_closure_function(closure, local->psc_function_index) : NULL;
    const XrSemanticProgramFunctionBinding *function_binding =
        local ? semantic_binding_for_program_row(plan, local->psc_function_index) : NULL;
    uint32_t semantic_function = function_binding ? function_binding->semantic_function
                                                  : XR_SEMANTIC_INDEX_NONE;
    const XiValue *xi_call = NULL;
    uint32_t call_row_index = XI_PSC_ROW_NONE;
    for (uint32_t i = 0; local && i < xr_program_semantic_closure_call_count(closure); i++) {
        const XiValue *candidate = xi_program_semantic_call_for_row(local, i);
        if (!candidate)
            continue;
        if (xi_call)
            return semantic_scalar_fail(error, error_size,
                                        "private-leaf Xi call binding is ambiguous");
        xi_call = candidate;
        call_row_index = i;
    }
    const XrProgramSemanticCallRecord *program_call =
        call_row_index != XI_PSC_ROW_NONE
            ? xr_program_semantic_closure_call(closure, call_row_index)
            : NULL;
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *operation =
        semantic_operation_for_xi_value(plan, local, semantic_function, xi_call, &operation_index);
    const XrSemanticProgramCallBinding *call_binding =
        xr_semantic_plan_program_call_for_operation(plan, operation_index);
    const XrProgramSemanticTypeRecord *program_type =
        xr_program_semantic_closure_type(closure, 0);
    const XrSemanticProgramTypeBinding *type_binding =
        xr_semantic_plan_program_type_for_row(plan, 0);
    const XrSemanticTypeRecord *semantic_type =
        type_binding ? xr_semantic_plan_type(plan, type_binding->semantic_type) : NULL;
    if (!local || !program_function || !function_binding || !xi_call || !program_call ||
        !operation || !call_binding || !program_type || !type_binding ||
        !semantic_i64_type_is_exact(semantic_type) || type_binding->program_row != 0 ||
        !xr_stable_id_equal(type_binding->program_type, program_type->id) ||
        !xr_stable_id_equal(function_binding->program_function, program_function->id) ||
        function_binding->flags != program_function->flags || local->psc_return_type_index != 0 ||
        call_binding->program_row != call_row_index || call_binding->operation != operation_index ||
        !xr_stable_id_equal(call_binding->program_call, program_call->id) ||
        !xr_stable_id_equal(call_binding->callsite, program_call->callsite_identity) ||
        !xr_stable_id_equal(call_binding->caller_program_function,
                            program_call->caller_function) ||
        !xr_stable_id_equal(call_binding->callee_program_function,
                            program_call->callee_function) ||
        operation->opcode != XI_CALL || operation->function != semantic_function ||
        operation->result_type != type_binding->semantic_type ||
        !semantic_source_module_key_matches_root(plan, root) ||
        !semantic_initializer_is_exact(plan, root, error, error_size) ||
        !semantic_function_is_exact(plan, semantic_function, 0, type_binding->semantic_type, error,
                                    error_size) ||
        !semantic_private_leaf_call_inventory_is_exact(
            plan, semantic_function, call_binding, error, error_size))
        return false;

    bool entry = program_function->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY;
    if (entry) {
        const XrProgramSemanticDependencyRecord *program_dependency =
            xr_program_semantic_closure_dependency(closure, 0);
        const XrSemanticProgramDependencyBinding *dependency =
            plan->program_dependency_binding_count == 1 ? &plan->program_dependency_bindings[0]
                                                        : NULL;
        const XrSemanticCallTargetRecord *target = NULL;
        for (uint32_t i = 0; i < plan->call_target_count; i++) {
            const XrSemanticCallTargetRecord *candidate = &plan->call_targets[i];
            if (candidate->operation != operation_index ||
                candidate->kind != XR_SEM_CALL_TARGET_SOURCE_EXPORT)
                continue;
            if (target)
                return semantic_scalar_fail(error, error_size,
                                            "private-leaf source target is ambiguous");
            target = candidate;
        }
        if (!program_dependency || !dependency || !target || plan->dependency_count != 1 ||
            call_binding->program_dependency != 0 ||
            call_binding->target_function != XR_SEMANTIC_INDEX_NONE ||
            dependency->program_row != 0 || dependency->semantic_dependency != 0 ||
            target->dependency != 0 ||
            !xr_stable_id_equal(dependency->resolver_binding,
                                program_dependency->resolver_binding) ||
            !xr_stable_id_equal(call_binding->resolver_binding,
                                program_call->resolver_binding))
            return semantic_scalar_fail(error, error_size,
                                        "private-leaf source call join is not exact");
        return true;
    }
    XrStableId native_identity = {{0}};
    const XrSemanticSourceExportRecord *source_export =
        private_leaf_semantic_export(plan, semantic_function);
    if (program_function->flags != XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED || !source_export ||
        plan->dependency_count != 0 || plan->program_dependency_binding_count != 0 ||
        call_binding->program_dependency != XR_SEMANTIC_INDEX_NONE ||
        call_binding->target_function != XR_SEMANTIC_INDEX_NONE ||
        !semantic_id_is_zero(call_binding->resolver_binding) ||
        !xr_semantic_native_target_leaf_call_is_exact(plan, operation, NULL, &native_identity) ||
        !xr_stable_id_equal(native_identity, program_call->callee_function))
        return semantic_scalar_fail(error, error_size,
                                    "private native leaf SemanticPlan join is not exact");
    return true;
}

bool xi_program_semantic_plan_verify_module_set(XiModule *const *modules, uint32_t module_count,
                                                uint32_t entry_index, char *error,
                                                size_t error_size) {
    if (!xi_program_semantic_verify_module_set(modules, module_count, entry_index, NULL, error,
                                               error_size))
        return false;
    const XiModule *entry_module =
        modules && entry_index < module_count ? modules[entry_index] : NULL;
    const XrProgramSemanticClosure *closure =
        entry_module ? entry_module->program_semantic_closure : NULL;
    bool graph = closure && xr_program_semantic_closure_family(closure) ==
                              XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL;
    bool private_leaf = closure && xr_program_semantic_closure_family(closure) ==
                                     XR_PROGRAM_SEMANTIC_FAMILY_SOURCE_MODULE_SCALAR_PRIVATE_LEAF_CALL;
    if (!closure || (!graph && !private_leaf) || module_count != 2)
        return semantic_scalar_fail(error, error_size,
                                    "graph SemanticPlan module set is incomplete");
    if (private_leaf) {
        const XrSemanticPlan *entry_plan = entry_module->init->semantic_plan;
        const XrSemanticPlan *producer_plan = NULL;
        for (uint32_t i = 0; i < module_count; i++) {
            const XiModule *module = modules[i];
            if (!module || !module->init || !module->init->semantic_plan ||
                !semantic_private_leaf_partition_plan_verify(
                    module->init, module->init->semantic_plan, error, error_size))
                return false;
            if (i != entry_index)
                producer_plan = module->init->semantic_plan;
        }
        const XrSemanticPlan *dependencies[1] = {producer_plan};
        if (!entry_plan || !producer_plan || !xr_semantic_plan_verify_module_set(
                                                  entry_plan, dependencies, 1, error, error_size))
            return false;
        const XrSemanticProgramCallBinding *entry_call =
            entry_plan->program_call_binding_count == 1
                ? &entry_plan->program_call_bindings[0]
                : NULL;
        const XrSemanticProgramFunctionBinding *producer_function =
            producer_plan->program_function_binding_count == 1
                ? &producer_plan->program_function_bindings[0]
                : NULL;
        return (entry_call && producer_function &&
                xr_stable_id_equal(entry_call->callee_program_function,
                                   producer_function->program_function)) ||
               semantic_scalar_fail(error, error_size,
                                    "private-leaf SemanticPlan module-set join is not exact");
    }
    const XrProgramSemanticCallRecord *program_call =
        xr_program_semantic_closure_call(closure, 0);
    const XrProgramSemanticDependencyRecord *program_dependency =
        xr_program_semantic_closure_dependency(closure, 0);
    const XrSemanticPlan *entry_plan = entry_module->init->semantic_plan;
    const XrSemanticPlan *producer_plan = NULL;
    for (uint32_t i = 0; i < module_count; i++) {
        const XiModule *module = modules[i];
        if (!module || !module->init || !module->init->semantic_plan ||
            !semantic_graph_partition_plan_verify(module->init, module->init->semantic_plan, error,
                                                  error_size))
            return false;
        if (i != entry_index)
            producer_plan = module->init->semantic_plan;
    }
    const XrSemanticPlan *dependencies[1] = {producer_plan};
    if (!entry_plan || !producer_plan || !xr_semantic_plan_verify_module_set(
                                                 entry_plan, dependencies, 1, error, error_size))
        return false;
    const XrSemanticProgramCallBinding *call =
        entry_plan->program_call_binding_count == 1 ? &entry_plan->program_call_bindings[0] : NULL;
    const XrSemanticProgramDependencyBinding *dependency =
        entry_plan->program_dependency_binding_count == 1
            ? &entry_plan->program_dependency_bindings[0]
            : NULL;
    const XrSemanticProgramFunctionBinding *producer_function =
        producer_plan->program_function_binding_count == 1
            ? &producer_plan->program_function_bindings[0]
            : NULL;
    return (program_call && program_dependency && call && dependency && producer_function &&
            xr_stable_id_equal(call->caller_program_function, program_call->caller_function) &&
            xr_stable_id_equal(call->callee_program_function, program_call->callee_function) &&
            xr_stable_id_equal(producer_function->program_function,
                               program_call->callee_function) &&
            xr_stable_id_equal(dependency->resolver_binding,
                               program_dependency->resolver_binding)) ||
           semantic_scalar_fail(error, error_size,
                                "graph SemanticPlan module-set PSC join is not exact");
}

bool xi_program_semantic_plan_verify(const XiFunc *root, const XrSemanticPlan *plan,
                                     const XrTargetProfile *target_profile, char *error,
                                     size_t error_size) {
    const XiModule *module = root ? root->module : NULL;
    XrProgramSemanticFamily family =
        module && module->program_semantic_closure
            ? xr_program_semantic_closure_family(module->program_semantic_closure)
            : 0;
    if (family == XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_DIRECT_CALL)
        return verify_scalar_semantic_plan(root, plan, target_profile, error, error_size);
    if (family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL) {
        if (target_profile)
            return semantic_scalar_fail(error, error_size,
                                        "aggregate SemanticPlan cannot consume a target profile");
        return semantic_leaf_aggregate_plan_verify(root, plan, error, error_size);
    }
    if (family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL) {
        if (target_profile)
            return semantic_scalar_fail(error, error_size,
                                        "product SemanticPlan cannot consume a target profile");
        return semantic_leaf_product_plan_verify(root, plan, error, error_size);
    }
    if (family == XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL) {
        if (target_profile)
            return semantic_scalar_fail(error, error_size,
                                        "graph SemanticPlan cannot consume a target profile");
        return semantic_graph_partition_plan_verify(root, plan, error, error_size);
    }
    if (family == XR_PROGRAM_SEMANTIC_FAMILY_SOURCE_MODULE_SCALAR_PRIVATE_LEAF_CALL) {
        if (target_profile)
            return semantic_scalar_fail(error, error_size,
                                        "private-leaf SemanticPlan cannot consume target facts");
        return semantic_private_leaf_partition_plan_verify(root, plan, error, error_size);
    }
    if (family == XR_PROGRAM_SEMANTIC_FAMILY_I64_OVERFLOW_PREDICATE)
        return xi_i64_overflow_semantic_plan_verify(root, plan, target_profile,
                                                    error, error_size);
    return semantic_scalar_fail(error, error_size, "SemanticPlan program family is unsupported");
}
