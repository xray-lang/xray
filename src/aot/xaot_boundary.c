/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_boundary.c - explicit AOT representation boundary plan
 */

#include "xaot_boundary.h"
#include "xaot_bundle.h"
#include "emit_c/xr_c_program_emission.h"
#include "refine/xr_aot_scalar_value.h"
#include "xr_target_aggregate_c_projection.h"
#include "../plan/semantic/xr_program_semantic_closure.h"
#include "../plan/semantic/xr_semantic_native_leaf_shape.h"
#include "../ir/xi_module.h"
#include <stdio.h>
#include <string.h>

static const XiImportRef *module_import_ref_for_value(const XaotBundle *bundle,
                                                      const XiFunc *current, const XiValue *value);
static const XiClassData *resolve_imported_class(const XaotBundle *bundle, const XiImportRef *ref,
                                                 const XiModule **owner_out);
static const XiModule *bundle_module_for_func(const XaotBundle *bundle, const XiFunc *func);

static XaotDirectI64TargetStatus direct_i64_error(char *errbuf, size_t errbuf_len,
                                                  const char *message) {
    if (errbuf && errbuf_len > 0)
        snprintf(errbuf, errbuf_len, "%s", message ? message : "invalid direct-i64 TargetPlan");
    return XAOT_DIRECT_I64_TARGET_INVALID;
}

static void direct_i64_find_function(const XiFunc *function, const XrSemanticPlan *semantic,
                                     uint32_t semantic_function, const XiFunc **match,
                                     uint32_t *match_count) {
    if (!function || !match || !match_count)
        return;
    if (function->semantic_plan == semantic &&
        function->semantic_plan_function_index == semantic_function) {
        *match = function;
        (*match_count)++;
    }
    for (uint16_t i = 0; i < function->nchildren; i++)
        direct_i64_find_function(function->children[i], semantic, semantic_function, match,
                                 match_count);
}

static bool direct_i64_machine_rep(const XrTargetPlan *target, uint16_t rep) {
    const XrTargetMachineRepRecord *machine = xr_target_plan_machine_rep(target, rep);
    return machine && machine->kind == XR_MACHINE_REP_I64 &&
           machine->ownership == XR_TARGET_OWNERSHIP_TRIVIAL;
}

/* A verified native target leaf is intentionally outside the direct-local
 * family consumed by XaotDirectI64TargetView.  Recognize that exclusion from
 * frozen identities before the legacy direct-local shape checks run; a live
 * Xi drift still fails closed instead of falling through to name-based AOT
 * dispatch. */
static XaotDirectI64TargetStatus direct_i64_exclude_native_target_leaf(
    const XaotBundle *bundle, const XiFunc *caller, const XiValue *call, const XrTargetPlan *target,
    const XrTargetFunctionRecord *caller_row, bool *excluded, char *errbuf, size_t errbuf_len) {
    if (excluded)
        *excluded = false;
    if (!bundle || !caller || !call || !target || !caller_row || !excluded)
        return direct_i64_error(errbuf, errbuf_len,
                                "native target leaf exclusion input is incomplete");

    uint32_t partition = UINT32_MAX;
    const XrSemanticPlan *semantic =
        xaot_bundle_program_semantic_for_func(bundle, caller, &partition);
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    if (!semantic ||
        !xr_aot_scalar_program_semantic_value_id(target, partition, caller_row->id, caller, call,
                                                 &semantic_function, &semantic_value, NULL, 0))
        return XAOT_DIRECT_I64_TARGET_UNCOVERED;

    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target, &call_count);
    const XrTargetCallRecord *match = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        const XrTargetCallRecord *candidate = &calls[i];
        if (candidate->caller_function != caller_row->id ||
            candidate->result_value != semantic_value ||
            candidate->target_kind != XR_TARGET_CALL_TARGET_NATIVE_TARGET_LEAF_SCALAR ||
            candidate->calling_convention != XR_TARGET_CALL_CONVENTION_NATIVE_TARGET_LEAF_SCALAR)
            continue;
        if (match)
            return direct_i64_error(errbuf, errbuf_len,
                                    "native target leaf TargetPlan call is ambiguous");
        match = candidate;
    }
    if (!match)
        return XAOT_DIRECT_I64_TARGET_UNCOVERED;

    const XrSemanticOperationRecord *operation =
        semantic ? xr_semantic_plan_operation(semantic, match->semantic_operation) : NULL;
    const XrStdlibDefEntry *entry = NULL;
    XrStableId native_identity = {{0}};
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        semantic ? xr_semantic_plan_operands(semantic, &operand_count) : NULL;
    uint32_t callee_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t callee_value = XR_SEMANTIC_INDEX_NONE;
    const XiValue *callee = call->nargs == 1 && call->args ? call->args[0] : NULL;
    const XiImportRef *ref = callee && callee->op == XI_IMPORT_REF && callee->aux
                                 ? (const XiImportRef *) callee->aux
                                 : NULL;
    bool exact =
        semantic && operation && operands && semantic_function == caller_row->semantic_function &&
        operation->function == semantic_function && operation->result_value == semantic_value &&
        xr_semantic_native_target_leaf_call_is_exact(semantic, operation, &entry,
                                                     &native_identity) &&
        operation->operand_begin < operand_count && call->op == XI_CALL && call->nargs == 1 &&
        callee && ref && ref->module_path && ref->member_name && entry &&
        strcmp(ref->module_path, entry->module) == 0 &&
        strcmp(ref->member_name, entry->name) == 0 &&
        xr_aot_scalar_program_semantic_value_id(target, partition, caller_row->id, caller, callee,
                                                &callee_function, &callee_value, NULL, 0) &&
        callee_function == semantic_function &&
        callee_value == operands[operation->operand_begin].value &&
        match->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
        match->callee_function == XR_SEMANTIC_INDEX_NONE && match->argument_count == 0 &&
        match->adapter_count == 0 && match->native_leaf == entry->target_leaf &&
        xr_stable_id_equal(match->native_callee_identity, native_identity);
    if (!exact)
        return direct_i64_error(errbuf, errbuf_len, "native target leaf live binding is inexact");

    *excluded = true;
    return XAOT_DIRECT_I64_TARGET_UNCOVERED;
}

XR_FUNC XaotDirectI64TargetStatus xaot_boundary_direct_i64_function_status(
    const XaotBundle *bundle, const XiFunc *function, const XrTargetPlan **target_out,
    const XrTargetFunctionRecord **function_out, char *errbuf, size_t errbuf_len) {
    if (target_out)
        *target_out = NULL;
    if (function_out)
        *function_out = NULL;
    if (!bundle || !function || !function->semantic_plan ||
        function->semantic_plan_function_index == XR_SEMANTIC_INDEX_NONE)
        return XAOT_DIRECT_I64_TARGET_UNCOVERED;

    const XrSemanticPlan *semantic = xaot_bundle_program_semantic_for_func(bundle, function, NULL);
    const XrTargetPlan *target = semantic ? xaot_bundle_program_target_plan(bundle) : NULL;
    if (!target || semantic != function->semantic_plan ||
        xr_target_plan_completed_family_mask(target) != XR_TARGET_REQUIRED_FAMILIES ||
        !xr_target_plan_is_verified(target) || !xr_target_plan_fingerprint_is_intact(target))
        return direct_i64_error(errbuf, errbuf_len,
                                "direct-i64 function has corrupt TargetPlan authority");

    uint32_t function_count = 0;
    const XrTargetFunctionRecord *functions = xr_target_plan_functions(target, &function_count);
    uint32_t index = XR_SEMANTIC_INDEX_NONE;
    if (!xr_target_plan_find_function(target, semantic, function->semantic_plan_function_index,
                                      &index))
        return direct_i64_error(errbuf, errbuf_len,
                                "direct-i64 function has no canonical program function row");
    if (!functions || index >= function_count || functions[index].id != index ||
        functions[index].semantic_function != function->semantic_plan_function_index)
        return direct_i64_error(errbuf, errbuf_len,
                                "direct-i64 function has no exact TargetPlan function row");
    if (xr_target_plan_function_execution_family_mask(target, index) !=
        XR_TARGET_EXECUTION_SCALAR_I64_CLOSED)
        return XAOT_DIRECT_I64_TARGET_UNCOVERED;

    if (target_out)
        *target_out = target;
    if (function_out)
        *function_out = &functions[index];
    return XAOT_DIRECT_I64_TARGET_FOUND;
}

XR_FUNC XaotDirectI64TargetStatus xaot_boundary_direct_i64_abi_status(
    const XaotBundle *bundle, const XiFunc *function, XrCAbiBoundaryKind *boundary_out,
    char *errbuf, size_t errbuf_len) {
    if (boundary_out)
        *boundary_out = XR_C_ABI_BOUNDARY_INVALID;
    uint32_t partition = UINT32_MAX;
    const XrSemanticPlan *semantic =
        xaot_bundle_program_semantic_for_func(bundle, function, &partition);
    const XrTargetPlan *program_target = semantic ? xaot_bundle_program_target_plan(bundle) : NULL;
    uint32_t program_graph_count = 0;
    const XrTargetProgramGraphRecord *program_graphs =
        program_target ? xr_target_plan_program_graphs(program_target, &program_graph_count) : NULL;
    if (program_graph_count != 0u) {
        XrCProgramDirectI64EmissionBinding binding = {0};
        XrCFunctionAbiEmissionView return_abi = {0};
        char error[256] = {0};
        uint32_t function_count = 0;
        const XrTargetFunctionRecord *functions =
            xr_target_plan_functions(program_target, &function_count);
        bool bound =
            program_graphs && program_graph_count == 1u && bundle && bundle->modules &&
            xr_c_program_direct_i64_emission_bind(program_target, bundle->modules, bundle->nmodules,
                                                  &binding, error, sizeof(error));
        const XrCProgramXiFunctionBinding *function_binding =
            bound ? xr_c_program_direct_i64_function_binding(&binding, function) : NULL;
        if (!bound) {
            xr_c_program_direct_i64_emission_release(&binding);
            return direct_i64_error(
                errbuf, errbuf_len,
                error[0] ? error : "program direct-i64 ABI has no exact global binding");
        }
        if (!function_binding) {
            xr_c_program_direct_i64_emission_release(&binding);
            return XAOT_DIRECT_I64_TARGET_UNCOVERED;
        }
        bool exact =
            function_binding && functions && function_binding->target_partition == partition &&
            function_binding->target_function < function_count &&
            functions[function_binding->target_function].id == function_binding->target_function &&
            functions[function_binding->target_function].semantic_function ==
                function_binding->semantic_function &&
            xr_c_program_direct_i64_function_abi_view(&binding, function, 0u, &return_abi) &&
            return_abi.semantic_function == function_binding->semantic_function;
        xr_c_program_direct_i64_emission_release(&binding);
        if (!exact)
            return direct_i64_error(errbuf, errbuf_len,
                                    "program direct-i64 ABI has no exact function binding");
        if (boundary_out)
            *boundary_out = return_abi.boundary_kind;
        return XAOT_DIRECT_I64_TARGET_FOUND;
    }

    const XrTargetPlan *target = NULL;
    const XrTargetFunctionRecord *function_row = NULL;
    XaotDirectI64TargetStatus status = xaot_boundary_direct_i64_function_status(
        bundle, function, &target, &function_row, errbuf, errbuf_len);
    if (status != XAOT_DIRECT_I64_TARGET_FOUND)
        return status;

    /* A function can relinquish its legacy ABI only when every inbound edge
     * is itself an executable closed-i64 call row.  A module initializer may
     * call an otherwise closed helper while also performing unsupported work;
     * that helper keeps its legacy ABI until the initializer family migrates. */
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target, &call_count);
    for (uint32_t i = 0; calls && i < call_count; i++) {
        if (calls[i].callee_function != function_row->id)
            continue;
        if (calls[i].target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
            calls[i].calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL ||
            xr_target_plan_function_execution_family_mask(target, calls[i].caller_function) !=
                XR_TARGET_EXECUTION_SCALAR_I64_CLOSED)
            return XAOT_DIRECT_I64_TARGET_UNCOVERED;
        uint32_t instruction_count = 0;
        const XrTargetInstructionRecord *instructions = xr_target_plan_function_instructions(
            target, calls[i].caller_function, &instruction_count);
        uint32_t matches = 0;
        for (uint32_t instruction = 0; instructions && instruction < instruction_count;
             instruction++) {
            if (instructions[instruction].opcode == XR_TARGET_INSTRUCTION_CALL_DIRECT_I64 &&
                instructions[instruction].immediate_bits == calls[i].id)
                matches++;
        }
        if (matches != 1)
            return direct_i64_error(errbuf, errbuf_len,
                                    "direct-i64 inbound instruction authority is inexact");
    }
    if (boundary_out)
        *boundary_out = XR_C_ABI_BOUNDARY_NATIVE;
    return XAOT_DIRECT_I64_TARGET_FOUND;
}

XR_FUNC XaotDirectI64TargetStatus xaot_boundary_direct_i64_call_view(
    const XaotBundle *bundle, const XiFunc *caller, const XiValue *call,
    XaotDirectI64TargetView *out, char *errbuf, size_t errbuf_len) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!bundle || !caller || !call || !out)
        return direct_i64_error(errbuf, errbuf_len, "direct-i64 call view input is incomplete");

    const XrTargetPlan *target = NULL;
    const XrTargetFunctionRecord *caller_row = NULL;
    XaotDirectI64TargetStatus status = xaot_boundary_direct_i64_function_status(
        bundle, caller, &target, &caller_row, errbuf, errbuf_len);
    if (status != XAOT_DIRECT_I64_TARGET_FOUND)
        return status;
    bool native_target_leaf = false;
    status = direct_i64_exclude_native_target_leaf(bundle, caller, call, target, caller_row,
                                                   &native_target_leaf, errbuf, errbuf_len);
    if (status == XAOT_DIRECT_I64_TARGET_INVALID || native_target_leaf)
        return status;
    uint32_t program_graph_count = 0;
    const XrTargetProgramGraphRecord *program_graphs =
        xr_target_plan_program_graphs(target, &program_graph_count);
    if (program_graph_count != 0u) {
        XrCProgramDirectI64EmissionBinding binding = {0};
        char error[256] = {0};
        bool bound = program_graphs && program_graph_count == 1u && bundle->modules &&
                     xr_c_program_direct_i64_emission_bind(
                         target, bundle->modules, bundle->nmodules, &binding, error, sizeof(error));
        bool exact = bound && xr_c_program_direct_i64_call_is_exact(&binding, caller, call) &&
                     caller_row == binding.caller_target_row;
        if (!exact) {
            xr_c_program_direct_i64_emission_release(&binding);
            return direct_i64_error(
                errbuf, errbuf_len,
                error[0] ? error : "program direct-i64 call has no exact global binding");
        }
        out->target_plan = target;
        out->caller_function = binding.caller_target_row;
        out->callee_function = binding.callee_target_row;
        out->call = binding.call_row;
        out->argument = binding.argument_row;
        out->call_instruction = binding.instruction_row;
        out->callee = binding.callee.xi_function;
        out->argument_value = binding.xi_argument;
        out->target_fingerprint = binding.target_fingerprint;
        xr_c_program_direct_i64_emission_release(&binding);
        return XAOT_DIRECT_I64_TARGET_FOUND;
    }
    if (call->op != XI_CALL || call->nargs != 2 || !call->args || !call->args[1])
        return direct_i64_error(errbuf, errbuf_len,
                                "covered direct-i64 call has invalid live binding");

    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t argument_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t argument_value = XR_SEMANTIC_INDEX_NONE;
    if (!xr_aot_scalar_semantic_value_id(target, caller, call, &semantic_function, &semantic_value,
                                         errbuf, errbuf_len) ||
        !xr_aot_scalar_semantic_value_id(target, caller, call->args[1], &argument_function,
                                         &argument_value, errbuf, errbuf_len) ||
        semantic_function != caller_row->semantic_function ||
        argument_function != semantic_function)
        return direct_i64_error(errbuf, errbuf_len,
                                "covered direct-i64 call lacks exact semantic value identity");

    const XrSemanticPlan *semantic = xaot_bundle_program_semantic_for_func(bundle, caller, NULL);
    const XrSemanticOperationRecord *operation = NULL;
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->function != semantic_function ||
            candidate->result_value != semantic_value)
            continue;
        if (operation)
            return direct_i64_error(errbuf, errbuf_len,
                                    "covered direct-i64 call semantic operation is ambiguous");
        operation = candidate;
        operation_index = i;
    }
    if (!operation)
        return direct_i64_error(errbuf, errbuf_len,
                                "covered direct-i64 call has no semantic operation");

    const XrTargetCallRecord *target_call = NULL;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target, &call_count);
    for (uint32_t i = 0; calls && i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (target_call)
            return direct_i64_error(errbuf, errbuf_len,
                                    "covered direct-i64 TargetPlan call is ambiguous");
        target_call = &calls[i];
    }
    if (!target_call || target_call->caller_function != caller_row->id ||
        target_call->result_value != semantic_value || target_call->argument_count != 1 ||
        target_call->adapter_count != 0 ||
        target_call->calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL ||
        target_call->target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
        target_call->result_mode != XR_TARGET_CALL_VALUE ||
        target_call->result_ownership != XR_TARGET_CALL_NONE ||
        target_call->error_mode != XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL ||
        target_call->error_slot != XR_SEMANTIC_INDEX_NONE ||
        target_call->result_slot == XR_TARGET_INSTRUCTION_SLOT_NONE ||
        !direct_i64_machine_rep(target, target_call->result_register_rep) ||
        !direct_i64_machine_rep(target, target_call->result_memory_rep))
        return direct_i64_error(errbuf, errbuf_len,
                                "covered direct-i64 TargetPlan call row is inexact");

    const XrSemanticCallTargetRecord *semantic_target =
        xr_semantic_plan_call_target(semantic, target_call->semantic_call_target);
    uint32_t function_count = 0;
    const XrTargetFunctionRecord *functions = xr_target_plan_functions(target, &function_count);
    const XrTargetFunctionRecord *callee_row =
        functions && target_call->callee_function < function_count
            ? &functions[target_call->callee_function]
            : NULL;
    if (!semantic_target || !callee_row || semantic_target->operation != operation_index ||
        semantic_target->function != callee_row->semantic_function ||
        semantic_target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL)
        return direct_i64_error(errbuf, errbuf_len,
                                "covered direct-i64 callee identity is inexact");

    if (callee_row->id != target_call->callee_function ||
        xr_target_plan_module_for_function(target, callee_row->id, NULL) != semantic ||
        xr_target_plan_function_execution_family_mask(target, target_call->callee_function) !=
            XR_TARGET_EXECUTION_SCALAR_I64_CLOSED)
        return direct_i64_error(errbuf, errbuf_len,
                                "covered direct-i64 callee function row is inexact");

    uint32_t argument_count = 0;
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(target, &argument_count);
    if (!arguments || target_call->argument_begin >= argument_count)
        return direct_i64_error(errbuf, errbuf_len, "covered direct-i64 argument row is missing");
    const XrTargetCallArgumentRecord *argument = &arguments[target_call->argument_begin];
    if (argument->call != target_call->id || argument->ordinal != 0 ||
        argument->semantic_value != argument_value || argument->mode != XR_TARGET_CALL_VALUE ||
        argument->ownership != XR_TARGET_CALL_CONSUME ||
        argument->transfer_mode != XR_TRANSFER_SHARE || argument->flags != 0 ||
        argument->caller_slot == XR_TARGET_INSTRUCTION_SLOT_NONE ||
        argument->callee_slot == XR_TARGET_INSTRUCTION_SLOT_NONE ||
        !direct_i64_machine_rep(target, argument->register_rep) ||
        !direct_i64_machine_rep(target, argument->memory_rep) ||
        !direct_i64_machine_rep(target, argument->callee_register_rep) ||
        !direct_i64_machine_rep(target, argument->callee_memory_rep))
        return direct_i64_error(errbuf, errbuf_len,
                                "covered direct-i64 argument representation is inexact");

    const XiFunc *callee = NULL;
    uint32_t callee_count = 0;
    for (uint32_t i = 0; i < bundle->nmodules; i++) {
        const XiModule *module = bundle->modules ? bundle->modules[i] : NULL;
        if (module && module->init && module->init->semantic_plan == semantic)
            direct_i64_find_function(module->init, semantic, callee_row->semantic_function, &callee,
                                     &callee_count);
    }
    if (!callee || callee_count != 1)
        return direct_i64_error(errbuf, errbuf_len,
                                "covered direct-i64 Xi callee identity is not unique");

    const XrTargetInstructionRecord *call_instruction = NULL;
    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_function_instructions(target, caller_row->id, &instruction_count);
    for (uint32_t i = 0; instructions && i < instruction_count; i++) {
        if (instructions[i].opcode != XR_TARGET_INSTRUCTION_CALL_DIRECT_I64 ||
            instructions[i].immediate_bits != target_call->id)
            continue;
        if (call_instruction)
            return direct_i64_error(errbuf, errbuf_len,
                                    "covered direct-i64 instruction row is ambiguous");
        call_instruction = &instructions[i];
    }
    if (!call_instruction || call_instruction->function != caller_row->id ||
        call_instruction->result_slot != target_call->result_slot ||
        call_instruction->operand_count != 0 ||
        call_instruction->operand_slots[0] != XR_TARGET_INSTRUCTION_SLOT_NONE ||
        call_instruction->operand_slots[1] != XR_TARGET_INSTRUCTION_SLOT_NONE)
        return direct_i64_error(errbuf, errbuf_len,
                                "covered direct-i64 instruction row is inexact");

    out->target_plan = target;
    out->caller_function = caller_row;
    out->callee_function = callee_row;
    out->call = target_call;
    out->argument = argument;
    out->call_instruction = call_instruction;
    out->callee = callee;
    out->argument_value = call->args[1];
    out->target_fingerprint = xr_target_plan_fingerprint(target);
    return XAOT_DIRECT_I64_TARGET_FOUND;
}

static XaotLeafAggregateTargetStatus leaf_aggregate_error(char *errbuf, size_t errbuf_len,
                                                          const char *message) {
    if (errbuf && errbuf_len > 0)
        snprintf(errbuf, errbuf_len, "%s", message ? message : "invalid leaf-aggregate TargetPlan");
    return XAOT_LEAF_AGGREGATE_TARGET_INVALID;
}

static bool leaf_source_locator_equal(XiSourceLocator live, XrProgramSemanticSourceLocator frozen) {
    return live.kind == frozen.kind && live.span.start_line == frozen.start_line &&
           live.span.start_column == frozen.start_column && live.span.end_line == frozen.end_line &&
           live.span.end_column == frozen.end_column;
}

static bool leaf_value_locator_equal(const XiValue *live, XrProgramSemanticSourceLocator frozen) {
    return live && live->source_kind == frozen.kind &&
           live->source_span.start_line == frozen.start_line &&
           live->source_span.start_column == frozen.start_column &&
           live->source_span.end_line == frozen.end_line &&
           live->source_span.end_column == frozen.end_column;
}

static const XrSemanticProgramFunctionBinding *
leaf_program_function_for_row(const XrSemanticPlan *semantic, uint32_t program_row) {
    const XrSemanticProgramFunctionBinding *match = NULL;
    size_t count = xr_semantic_plan_program_function_binding_count(semantic);
    for (uint32_t i = 0; i < count; i++) {
        const XrSemanticProgramFunctionBinding *candidate =
            xr_semantic_plan_program_function_binding(semantic, i);
        if (!candidate || candidate->program_row != program_row)
            continue;
        if (match)
            return NULL;
        match = candidate;
    }
    return match;
}

static XaotLeafAggregateTargetStatus leaf_function_authority(
    const XaotBundle *bundle, const XiFunc *function, const XiModule **module_out,
    const XrProgramSemanticClosure **closure_out, const XrTargetPlan **target_out,
    const XrSemanticProgramFunctionBinding **binding_out, char *errbuf, size_t errbuf_len) {
    if (module_out)
        *module_out = NULL;
    if (closure_out)
        *closure_out = NULL;
    if (target_out)
        *target_out = NULL;
    if (binding_out)
        *binding_out = NULL;
    if (!bundle || !function)
        return XAOT_LEAF_AGGREGATE_TARGET_UNCOVERED;

    const XiModule *module = bundle_module_for_func(bundle, function);
    const XrProgramSemanticClosure *closure = module ? module->program_semantic_closure : NULL;
    if (closure && xr_program_semantic_closure_family(closure) ==
                       XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL)
        return leaf_aggregate_error(
            errbuf, errbuf_len,
            "leaf value-product execution cannot enter the legacy aggregate boundary");
    if (!module || !closure ||
        xr_program_semantic_closure_family(closure) !=
            XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL)
        return XAOT_LEAF_AGGREGATE_TARGET_UNCOVERED;
    if (module->init == function && function->psc_function_index == XI_PSC_ROW_NONE)
        return XAOT_LEAF_AGGREGATE_TARGET_UNCOVERED;
    if (!xr_program_semantic_closure_is_frozen(closure) ||
        !xr_program_semantic_closure_is_verified(closure) ||
        !xr_program_semantic_closure_verify(closure, NULL, 0) ||
        function->psc_function_index == XI_PSC_ROW_NONE)
        return leaf_aggregate_error(errbuf, errbuf_len,
                                    "leaf-aggregate Xi lacks frozen PSC function authority");

    uint32_t module_index = UINT32_MAX;
    for (uint32_t i = 0; i < bundle->nmodules; i++) {
        if (!bundle->modules || bundle->modules[i] != module)
            continue;
        if (module_index != UINT32_MAX)
            return leaf_aggregate_error(errbuf, errbuf_len,
                                        "leaf-aggregate Xi module authority is ambiguous");
        module_index = i;
    }
    const XrSemanticPlan *semantic =
        module_index != UINT32_MAX ? xaot_bundle_program_semantic_for_module(bundle, module_index)
                                   : NULL;
    const XrTargetPlan *target = semantic ? xaot_bundle_program_target_plan(bundle) : NULL;
    const XrSemanticProgramProvenance *provenance =
        semantic ? xr_semantic_plan_program_provenance(semantic) : NULL;
    XrGenerationClosureId generation = xr_program_semantic_closure_generation_id(closure);
    if (!target || !semantic || !provenance ||
        xr_target_plan_completed_family_mask(target) != XR_TARGET_REQUIRED_FAMILIES ||
        !xr_target_plan_is_verified(target) || !xr_target_plan_fingerprint_is_intact(target) ||
        provenance->program_schema != xr_program_semantic_closure_schema(closure) ||
        provenance->program_family != XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL ||
        provenance->type_count != xr_program_semantic_closure_type_count(closure) ||
        provenance->type_field_count != xr_program_semantic_closure_type_field_count(closure) ||
        provenance->function_count != xr_program_semantic_closure_function_count(closure) ||
        provenance->call_count != xr_program_semantic_closure_call_count(closure) ||
        !xr_fingerprint_equal(provenance->program_fingerprint,
                              xr_program_semantic_closure_fingerprint(closure)) ||
        memcmp(provenance->generation_identity.bytes, generation.bytes, sizeof(generation.bytes)) !=
            0)
        return leaf_aggregate_error(errbuf, errbuf_len,
                                    "leaf-aggregate TargetPlan and PSC authorities disagree");

    const XrProgramSemanticFunctionRecord *program_function =
        xr_program_semantic_closure_function(closure, function->psc_function_index);
    const XrSemanticProgramFunctionBinding *binding =
        leaf_program_function_for_row(semantic, function->psc_function_index);
    if (!program_function || !binding ||
        !xr_stable_id_equal(binding->program_function, program_function->id) ||
        binding->flags != program_function->flags ||
        !leaf_source_locator_equal(function->psc_declaration_locator,
                                   program_function->declaration_locator) ||
        ((function->semantic_plan != NULL ||
          function->semantic_plan_function_index != XR_SEMANTIC_INDEX_NONE) &&
         (function->semantic_plan != semantic ||
          function->semantic_plan_function_index != binding->semantic_function)))
        return leaf_aggregate_error(errbuf, errbuf_len,
                                    "leaf-aggregate Xi and PSC function bindings disagree");

    if (module_out)
        *module_out = module;
    if (closure_out)
        *closure_out = closure;
    if (target_out)
        *target_out = target;
    if (binding_out)
        *binding_out = binding;
    return XAOT_LEAF_AGGREGATE_TARGET_FOUND;
}

static bool leaf_block_contains_value(const XiFunc *function, const XiValue *value) {
    if (!function || !value || !value->block || value->block->func != function)
        return false;
    const XiBlock *block = value->block;
    bool block_found = false;
    for (uint32_t i = 0; i < function->nblocks; i++)
        if (function->blocks && function->blocks[i] == block)
            block_found = true;
    if (!block_found)
        return false;
    for (uint32_t i = 0; i < block->nvalues; i++)
        if (block->values && block->values[i] == value)
            return true;
    for (const XiPhi *phi = block->phis; phi; phi = phi->next)
        if (&phi->value == value)
            return true;
    return false;
}

static XaotLeafAggregateTargetStatus
leaf_semantic_value_identity(const XrTargetPlan *target, const XiFunc *function,
                             uint32_t semantic_function, const XiValue *value,
                             uint32_t *semantic_value_out, uint32_t *semantic_type_out) {
    if (semantic_value_out)
        *semantic_value_out = XR_SEMANTIC_INDEX_NONE;
    if (semantic_type_out)
        *semantic_type_out = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticPlan *semantic = target ? xr_target_plan_semantic_plan(target) : NULL;
    const XrSemanticFunctionRecord *record =
        semantic ? xr_semantic_plan_function(semantic, semantic_function) : NULL;
    if (!record || !function || !value || !semantic_value_out || !semantic_type_out ||
        !leaf_block_contains_value(function, value) || value->id >= record->value_count ||
        record->value_begin > UINT32_MAX - value->id)
        return XAOT_LEAF_AGGREGATE_TARGET_INVALID;
    uint32_t semantic_value = record->value_begin + value->id;
    uint32_t semantic_type = XR_SEMANTIC_INDEX_NONE;
    bool parameter_match = false;
    bool operation_match = false;
    for (uint32_t i = 0; i < xr_semantic_plan_parameter_count(semantic); i++) {
        const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(semantic, i);
        if (!parameter || parameter->function != semantic_function ||
            parameter->value != semantic_value)
            continue;
        if (value->op != XI_PARAM || !function->params || value->aux_int < 0 ||
            value->aux_int >= function->nparams || function->params[value->aux_int] != value)
            return XAOT_LEAF_AGGREGATE_TARGET_INVALID;
        if (parameter_match ||
            (semantic_type != XR_SEMANTIC_INDEX_NONE && semantic_type != parameter->type))
            return XAOT_LEAF_AGGREGATE_TARGET_INVALID;
        semantic_type = parameter->type;
        parameter_match = true;
    }
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (!operation || operation->function != semantic_function ||
            operation->result_value != semantic_value)
            continue;
        if (operation->opcode != value->op)
            return XAOT_LEAF_AGGREGATE_TARGET_INVALID;
        if (operation_match ||
            (semantic_type != XR_SEMANTIC_INDEX_NONE && semantic_type != operation->result_type))
            return XAOT_LEAF_AGGREGATE_TARGET_INVALID;
        semantic_type = operation->result_type;
        operation_match = true;
    }
    bool definition_exact = operation_match || parameter_match;
    if (parameter_match && (!operation_match || value->op != XI_PARAM))
        return XAOT_LEAF_AGGREGATE_TARGET_INVALID;
    const XrSemanticProgramTypeBinding *type_binding =
        definition_exact ? xr_semantic_plan_program_type_for_semantic_type(semantic, semantic_type)
                         : NULL;
    if (!type_binding)
        return value->psc_type_index == XI_PSC_ROW_NONE ? XAOT_LEAF_AGGREGATE_TARGET_UNCOVERED
                                                        : XAOT_LEAF_AGGREGATE_TARGET_INVALID;
    if (value->psc_type_index != type_binding->program_row)
        return XAOT_LEAF_AGGREGATE_TARGET_INVALID;
    *semantic_value_out = semantic_value;
    *semantic_type_out = semantic_type;
    return XAOT_LEAF_AGGREGATE_TARGET_FOUND;
}

static void leaf_find_program_function(const XiFunc *function, uint32_t program_row,
                                       const XiFunc **match, uint32_t *match_count) {
    if (!function || !match || !match_count)
        return;
    if (function->psc_function_index == program_row) {
        *match = function;
        (*match_count)++;
    }
    for (uint16_t i = 0; i < function->nchildren; i++)
        leaf_find_program_function(function->children[i], program_row, match, match_count);
}

static bool leaf_aggregate_rep(const XrTargetPlan *target, uint16_t rep, uint32_t semantic_type) {
    const XrTargetMachineRepRecord *machine = xr_target_plan_machine_rep(target, rep);
    XrCAggregateProjection projection = {0};
    return machine && machine->kind == XR_MACHINE_REP_AGGREGATE && machine->register_bits == 128 &&
           machine->memory_size == 16 && machine->memory_align == 8 &&
           machine->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
           xr_c_leaf_aggregate_projection(target, semantic_type, &projection) &&
           machine->detail == projection.layout;
}

XR_FUNC XaotLeafAggregateTargetStatus xaot_boundary_leaf_aggregate_function_status(
    const XaotBundle *bundle, const XiFunc *function, const XrTargetPlan **target_out,
    const XrTargetFunctionRecord **function_out, char *errbuf, size_t errbuf_len) {
    if (target_out)
        *target_out = NULL;
    if (function_out)
        *function_out = NULL;
    const XrTargetPlan *target = NULL;
    const XrSemanticProgramFunctionBinding *program_function = NULL;
    XaotLeafAggregateTargetStatus status = leaf_function_authority(
        bundle, function, NULL, NULL, &target, &program_function, errbuf, errbuf_len);
    if (status != XAOT_LEAF_AGGREGATE_TARGET_FOUND)
        return status;
    uint32_t count = 0;
    const XrTargetFunctionRecord *functions = xr_target_plan_functions(target, &count);
    uint32_t index = program_function->semantic_function;
    if (!functions || index >= count || functions[index].id != index ||
        functions[index].semantic_function != index)
        return leaf_aggregate_error(errbuf, errbuf_len, "leaf-aggregate function row is inexact");
    if (target_out)
        *target_out = target;
    if (function_out)
        *function_out = &functions[index];
    return XAOT_LEAF_AGGREGATE_TARGET_FOUND;
}

XR_FUNC XaotLeafAggregateTargetStatus xaot_boundary_leaf_aggregate_semantic_value(
    const XaotBundle *bundle, const XiFunc *function, const XiValue *value,
    const XrTargetPlan **target_out, uint32_t *semantic_function_out, uint32_t *semantic_value_out,
    char *errbuf, size_t errbuf_len) {
    if (target_out)
        *target_out = NULL;
    if (semantic_function_out)
        *semantic_function_out = XR_SEMANTIC_INDEX_NONE;
    if (semantic_value_out)
        *semantic_value_out = XR_SEMANTIC_INDEX_NONE;
    if (!semantic_function_out || !semantic_value_out)
        return leaf_aggregate_error(errbuf, errbuf_len,
                                    "leaf-aggregate value output is incomplete");
    const XrTargetPlan *target = NULL;
    const XrTargetFunctionRecord *function_row = NULL;
    XaotLeafAggregateTargetStatus status = xaot_boundary_leaf_aggregate_function_status(
        bundle, function, &target, &function_row, errbuf, errbuf_len);
    if (status != XAOT_LEAF_AGGREGATE_TARGET_FOUND)
        return status;
    uint32_t semantic_type = XR_SEMANTIC_INDEX_NONE;
    XaotLeafAggregateTargetStatus value_status =
        leaf_semantic_value_identity(target, function, function_row->semantic_function, value,
                                     semantic_value_out, &semantic_type);
    if (value_status == XAOT_LEAF_AGGREGATE_TARGET_UNCOVERED)
        return value_status;
    if (value_status != XAOT_LEAF_AGGREGATE_TARGET_FOUND)
        return leaf_aggregate_error(errbuf, errbuf_len,
                                    "leaf-aggregate Xi value lacks exact PSC identity");
    (void) semantic_type;
    *semantic_function_out = function_row->semantic_function;
    if (target_out)
        *target_out = target;
    return XAOT_LEAF_AGGREGATE_TARGET_FOUND;
}

XR_FUNC XaotLeafAggregateTargetStatus xaot_boundary_leaf_aggregate_abi_status(
    const XaotBundle *bundle, const XiFunc *function, char *errbuf, size_t errbuf_len) {
    const XrTargetPlan *target = NULL;
    const XrTargetFunctionRecord *function_row = NULL;
    XaotLeafAggregateTargetStatus status = xaot_boundary_leaf_aggregate_function_status(
        bundle, function, &target, &function_row, errbuf, errbuf_len);
    if (status != XAOT_LEAF_AGGREGATE_TARGET_FOUND)
        return status;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target, &call_count);
    for (uint32_t i = 0; calls && i < call_count; i++) {
        if (calls[i].callee_function != function_row->semantic_function)
            continue;
        const XrSemanticProgramCallBinding *binding = xr_semantic_plan_program_call_for_operation(
            xr_target_plan_semantic_plan(target), calls[i].semantic_operation);
        if (!binding || binding->target_function != function_row->semantic_function ||
            calls[i].target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
            calls[i].calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL)
            return leaf_aggregate_error(errbuf, errbuf_len,
                                        "leaf-aggregate inbound call authority is inexact");
        uint32_t instruction_count = 0, matches = 0;
        const XrTargetInstructionRecord *instructions = xr_target_plan_function_instructions(
            target, calls[i].caller_function, &instruction_count);
        for (uint32_t instruction = 0; instructions && instruction < instruction_count;
             instruction++)
            if (instructions[instruction].opcode == XR_TARGET_INSTRUCTION_CALL_DIRECT_AGGREGATE &&
                instructions[instruction].immediate_bits == calls[i].id)
                matches++;
        if (matches != 1)
            return leaf_aggregate_error(errbuf, errbuf_len,
                                        "leaf-aggregate inbound instruction authority is inexact");
    }
    return XAOT_LEAF_AGGREGATE_TARGET_FOUND;
}

XR_FUNC XaotLeafAggregateTargetStatus xaot_boundary_leaf_aggregate_call_view(
    const XaotBundle *bundle, const XiFunc *caller, const XiValue *call,
    XaotLeafAggregateTargetView *out, char *errbuf, size_t errbuf_len) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!bundle || !caller || !call || !out)
        return leaf_aggregate_error(errbuf, errbuf_len,
                                    "leaf-aggregate call view input is incomplete");
    const XrTargetPlan *target = NULL;
    const XrTargetFunctionRecord *caller_row = NULL;
    XaotLeafAggregateTargetStatus status = xaot_boundary_leaf_aggregate_function_status(
        bundle, caller, &target, &caller_row, errbuf, errbuf_len);
    if (status != XAOT_LEAF_AGGREGATE_TARGET_FOUND)
        return status;
    if (call->op != XI_CALL || call->nargs != 2 || !call->args || !call->args[1])
        return leaf_aggregate_error(errbuf, errbuf_len,
                                    "covered leaf-aggregate call has invalid live binding");

    uint32_t semantic_function = caller_row->semantic_function;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t argument_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t result_type = XR_SEMANTIC_INDEX_NONE;
    uint32_t argument_type = XR_SEMANTIC_INDEX_NONE;
    if (leaf_semantic_value_identity(target, caller, semantic_function, call, &semantic_value,
                                     &result_type) != XAOT_LEAF_AGGREGATE_TARGET_FOUND ||
        leaf_semantic_value_identity(target, caller, semantic_function, call->args[1],
                                     &argument_value,
                                     &argument_type) != XAOT_LEAF_AGGREGATE_TARGET_FOUND ||
        result_type != argument_type)
        return leaf_aggregate_error(errbuf, errbuf_len,
                                    "leaf-aggregate call lacks exact semantic identities");
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target);
    const XrSemanticOperationRecord *operation = NULL;
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < (uint32_t) xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->function != semantic_function ||
            candidate->result_value != semantic_value)
            continue;
        if (operation)
            return leaf_aggregate_error(errbuf, errbuf_len,
                                        "leaf-aggregate call semantic operation is ambiguous");
        operation = candidate;
        operation_index = i;
    }
    const XrSemanticProgramCallBinding *program_call =
        xr_semantic_plan_program_call_for_operation(semantic, operation_index);
    const XiModule *module = NULL;
    const XrProgramSemanticClosure *closure = NULL;
    const XrSemanticProgramFunctionBinding *caller_binding = NULL;
    if (!operation || operation->result_type != result_type || !program_call ||
        program_call->operation != operation_index ||
        leaf_function_authority(bundle, caller, &module, &closure, NULL, &caller_binding, NULL,
                                0) != XAOT_LEAF_AGGREGATE_TARGET_FOUND)
        return leaf_aggregate_error(errbuf, errbuf_len,
                                    "leaf-aggregate call lacks its typed program binding");
    const XrProgramSemanticCallRecord *psc_call =
        xr_program_semantic_closure_call(closure, program_call->program_row);
    const XrSemanticProgramTypeBinding *aggregate_binding =
        xr_semantic_plan_program_type_for_semantic_type(semantic, result_type);
    if (!psc_call || !aggregate_binding ||
        aggregate_binding->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE ||
        call->psc_call_index != program_call->program_row ||
        call->psc_type_index != aggregate_binding->program_row ||
        call->args[1]->psc_call_index != XI_PSC_ROW_NONE ||
        call->args[1]->psc_type_index != aggregate_binding->program_row ||
        !xr_stable_id_equal(program_call->program_call, psc_call->id) ||
        !xr_stable_id_equal(program_call->callsite, psc_call->callsite_identity) ||
        !xr_stable_id_equal(program_call->caller_program_function, psc_call->caller_function) ||
        !xr_stable_id_equal(program_call->callee_program_function, psc_call->callee_function) ||
        !caller_binding || caller_binding->program_row != caller->psc_function_index ||
        !xr_stable_id_equal(caller_binding->program_function, psc_call->caller_function) ||
        !leaf_value_locator_equal(call, psc_call->locator))
        return leaf_aggregate_error(errbuf, errbuf_len,
                                    "leaf-aggregate call and PSC identities disagree");

    const XrTargetCallRecord *target_call = NULL;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target, &call_count);
    for (uint32_t i = 0; calls && i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (target_call)
            return leaf_aggregate_error(errbuf, errbuf_len,
                                        "leaf-aggregate TargetPlan call is ambiguous");
        target_call = &calls[i];
    }
    if (!target_call || target_call->caller_function != semantic_function ||
        target_call->callee_function != program_call->target_function ||
        target_call->result_value != semantic_value || target_call->argument_count != 1 ||
        target_call->adapter_count != 0 ||
        target_call->calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL ||
        target_call->target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
        target_call->result_mode != XR_TARGET_CALL_CALLER_STORAGE ||
        target_call->result_ownership != XR_TARGET_CALL_NONE ||
        target_call->result_slot == XR_TARGET_INSTRUCTION_SLOT_NONE ||
        target_call->caller_storage_slot != target_call->result_slot ||
        target_call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        target_call->source_export != XR_SEMANTIC_INDEX_NONE || target_call->flags != 0)
        return leaf_aggregate_error(errbuf, errbuf_len,
                                    "leaf-aggregate TargetPlan call row is inexact");

    const XrSemanticCallTargetRecord *semantic_target =
        xr_semantic_plan_call_target(semantic, target_call->semantic_call_target);
    const XrSemanticProgramFunctionBinding *callee_binding =
        xr_semantic_plan_program_function_for_semantic_function(semantic,
                                                                target_call->callee_function);
    if (!semantic_target || semantic_target->operation != operation_index ||
        semantic_target->function != target_call->callee_function ||
        semantic_target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL || !caller_binding ||
        !callee_binding ||
        !xr_stable_id_equal(caller_binding->program_function,
                            program_call->caller_program_function) ||
        !xr_stable_id_equal(callee_binding->program_function,
                            program_call->callee_program_function))
        return leaf_aggregate_error(errbuf, errbuf_len,
                                    "leaf-aggregate callee identity is inexact");

    uint32_t function_count = 0;
    const XrTargetFunctionRecord *functions = xr_target_plan_functions(target, &function_count);
    uint32_t argument_count = 0;
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(target, &argument_count);
    if (!functions || target_call->callee_function >= function_count || !arguments ||
        target_call->argument_begin >= argument_count)
        return leaf_aggregate_error(errbuf, errbuf_len,
                                    "leaf-aggregate callee or argument row is missing");
    const XrTargetCallArgumentRecord *argument = &arguments[target_call->argument_begin];
    const XrSemanticParameterRecord *parameter =
        xr_semantic_plan_parameter(semantic, argument->callee_parameter);
    const XrTargetValueRepRecord *argument_rep = xr_target_plan_value_rep(target, argument_value);
    const XrTargetValueRepRecord *parameter_rep =
        parameter ? xr_target_plan_value_rep(target, parameter->value) : NULL;
    const XrTargetValueRepRecord *result_rep = xr_target_plan_value_rep(target, semantic_value);
    if (!parameter || parameter->function != target_call->callee_function ||
        parameter->type != result_type || argument->call != target_call->id ||
        argument->ordinal != 0 || argument->semantic_value != argument_value ||
        argument->mode != XR_TARGET_CALL_VALUE || argument->ownership != XR_TARGET_CALL_READ ||
        argument->transfer_mode != XR_TRANSFER_SHARE || argument->flags != 0 ||
        argument->register_rep != argument->memory_rep ||
        argument->register_rep != argument->callee_register_rep ||
        argument->register_rep != argument->callee_memory_rep || !argument_rep || !parameter_rep ||
        !result_rep || argument_rep->slot != argument->caller_slot ||
        parameter_rep->slot != argument->callee_slot ||
        result_rep->slot != target_call->result_slot ||
        target_call->result_register_rep != argument->register_rep ||
        target_call->result_memory_rep != argument->register_rep ||
        !leaf_aggregate_rep(target, argument->register_rep, operation->result_type))
        return leaf_aggregate_error(errbuf, errbuf_len,
                                    "leaf-aggregate representation or slot join is inexact");

    if (!module || !module->init || !callee_binding ||
        callee_binding->program_row == XI_PSC_ROW_NONE)
        return leaf_aggregate_error(errbuf, errbuf_len,
                                    "leaf-aggregate callee PSC binding is missing");
    const XrProgramSemanticFunctionRecord *callee_program =
        xr_program_semantic_closure_function(closure, callee_binding->program_row);
    const XiFunc *callee = NULL;
    uint32_t callee_count = 0;
    leaf_find_program_function(module->init, callee_binding->program_row, &callee, &callee_count);
    if (!callee || callee_count != 1 || !callee_program ||
        !xr_stable_id_equal(callee_binding->program_function, callee_program->id) ||
        !leaf_source_locator_equal(callee->psc_declaration_locator,
                                   callee_program->declaration_locator) ||
        callee->psc_return_type_index != aggregate_binding->program_row || callee->nparams != 1 ||
        !callee->params || !callee->params[0] ||
        callee->params[0]->psc_type_index != aggregate_binding->program_row ||
        ((callee->semantic_plan != NULL ||
          callee->semantic_plan_function_index != XR_SEMANTIC_INDEX_NONE) &&
         (callee->semantic_plan != semantic ||
          callee->semantic_plan_function_index != target_call->callee_function)))
        return leaf_aggregate_error(errbuf, errbuf_len,
                                    "leaf-aggregate Xi callee identity is not unique");
    const XrTargetInstructionRecord *call_instruction = NULL;
    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_function_instructions(target, semantic_function, &instruction_count);
    for (uint32_t i = 0; instructions && i < instruction_count; i++) {
        if (instructions[i].opcode != XR_TARGET_INSTRUCTION_CALL_DIRECT_AGGREGATE ||
            instructions[i].immediate_bits != target_call->id)
            continue;
        if (call_instruction)
            return leaf_aggregate_error(errbuf, errbuf_len,
                                        "leaf-aggregate instruction row is ambiguous");
        call_instruction = &instructions[i];
    }
    if (!call_instruction || call_instruction->function != semantic_function ||
        call_instruction->result_slot != target_call->result_slot ||
        call_instruction->operand_count != 0 ||
        call_instruction->operand_slots[0] != XR_TARGET_INSTRUCTION_SLOT_NONE ||
        call_instruction->operand_slots[1] != XR_TARGET_INSTRUCTION_SLOT_NONE)
        return leaf_aggregate_error(errbuf, errbuf_len,
                                    "leaf-aggregate instruction row is inexact");
    out->target_plan = target;
    out->caller_function = caller_row;
    out->callee_function = &functions[target_call->callee_function];
    out->call = target_call;
    out->argument = argument;
    out->call_instruction = call_instruction;
    out->callee = callee;
    out->argument_value = call->args[1];
    out->target_fingerprint = xr_target_plan_fingerprint(target);
    return XAOT_LEAF_AGGREGATE_TARGET_FOUND;
}

static const XiFunc *boundary_find_constructor(const XiFunc *parent,
                                               const XiClassData *class_data) {
    if (!parent || !class_data || !class_data->methods || !class_data->child_idx)
        return NULL;
    for (uint16_t i = 0; i < class_data->nmethod; i++) {
        const XiClassMethod *method = &class_data->methods[i];
        if (!method->is_constructor || method->is_static_constructor ||
            i >= class_data->ninst + class_data->nstat)
            continue;
        uint16_t child_index = class_data->child_idx[i];
        if (child_index < parent->nchildren) {
            const XiFunc *child = parent->children[child_index];
            if (child && child->name && strcmp(child->name, "constructor") == 0)
                return child;
        }
    }
    return NULL;
}

static const XiFunc *boundary_resolve_shared_constructor(const XaotBundle *bundle,
                                                         const XiFunc *current, int slot) {
    if (!bundle || !current || slot < 0)
        return NULL;
    for (uint32_t i = 0; i < bundle->nfunc_plans; i++) {
        const XaotFuncPlan *plan = &bundle->func_plans[i];
        if (plan->func != current || plan->module_index >= bundle->nmodules)
            continue;
        const XiModule *module = bundle->modules[plan->module_index];
        if (!module || slot >= module->nslots || !module->slot_classes)
            return NULL;
        return boundary_find_constructor(module->init, module->slot_classes[slot]);
    }
    return NULL;
}

XR_FUNC const XiFunc *xaot_boundary_resolve_constructor_call_target(const XaotBundle *bundle,
                                                                    const XiFunc *current,
                                                                    const XiValue *call,
                                                                    uint16_t *first_arg_out,
                                                                    uint16_t *first_param_out) {
    const XiFunc *target = NULL;
    if (first_arg_out)
        *first_arg_out = 0;
    if (first_param_out)
        *first_param_out = 0;
    if (!bundle || !current || !call || call->nargs < 1)
        return NULL;
    if (call->op == XI_CALL) {
        const XiValue *callee = xi_value_trace_repr(call->args[0]);
        if (callee && callee->op == XI_GET_SHARED)
            target = boundary_resolve_shared_constructor(bundle, current, (int) callee->aux_int);
    } else if (call->op == XI_CALL_METHOD && call->aux && (call->aux_int & 1) == 0) {
        const XiImportRef *module_ref = module_import_ref_for_value(bundle, current, call->args[0]);
        if (module_ref) {
            XiImportRef class_ref = *module_ref;
            const XiModule *owner = NULL;
            class_ref.member_name = (const char *) call->aux;
            class_ref.resolved_shared_slot = -1;
            class_ref.resolved_export_slot = -1;
            const XiClassData *cls = resolve_imported_class(bundle, &class_ref, &owner);
            if (cls && owner && owner->init)
                target = boundary_find_constructor(owner->init, cls);
        }
    }
    if (target) {
        if (first_arg_out)
            *first_arg_out = 1;
        if (first_param_out)
            *first_param_out = 1;
    }
    return target;
}

XR_FUNC const char *xaot_boundary_reason_name(XaotBoundaryReason reason) {
    switch (reason) {
        case XAOT_BOUNDARY_NONE:
            return "none";
        case XAOT_BOUNDARY_DIRECT_CALL:
            return "direct-call";
        case XAOT_BOUNDARY_DYNAMIC_CALL:
            return "dynamic-call";
        case XAOT_BOUNDARY_CLOSURE_OBJECT:
            return "closure-object";
        case XAOT_BOUNDARY_MODULE_INIT:
            return "module-init";
        case XAOT_BOUNDARY_EXCEPTION_FLOW:
            return "exception-flow";
        case XAOT_BOUNDARY_CORO_FRAME:
            return "coro-frame";
        case XAOT_BOUNDARY_TAGGED_TYPE:
            return "tagged-type";
        case XAOT_BOUNDARY_BOX:
            return "box";
        case XAOT_BOUNDARY_UNBOX:
            return "unbox";
        case XAOT_BOUNDARY_SHARED_SLOT:
            return "shared-slot";
        case XAOT_BOUNDARY_IMPORT_EXPORT:
            return "import-export";
        case XAOT_BOUNDARY_UNION_NULLABLE:
            return "union-nullable";
        case XAOT_BOUNDARY_RUNTIME_HELPER:
            return "runtime-helper";
        case XAOT_BOUNDARY_CORO_RESULT:
            return "coro-result";
        default:
            return "?";
    }
}

XR_FUNC const char *xaot_boundary_step_kind_name(XaotBoundaryStepKind kind) {
    switch (kind) {
        case XAOT_BOUNDARY_STEP_FUNC_ABI:
            return "func-abi";
        case XAOT_BOUNDARY_STEP_VALUE_REP:
            return "value-rep";
        case XAOT_BOUNDARY_STEP_DIRECT_CALL_ARG:
            return "direct-call-arg";
        case XAOT_BOUNDARY_STEP_DIRECT_CALL_RET:
            return "direct-call-ret";
        default:
            return "?";
    }
}

static const XiImportRef *value_import_ref(const XiValue *v) {
    v = xi_value_trace_repr(v);
    if (!v || v->op != XI_IMPORT_REF || !v->aux)
        return NULL;
    return (const XiImportRef *) v->aux;
}

static const XiImportRef *shared_slot_import_ref(const XiFunc *f, int slot) {
    uint32_t bi;

    if (!f || slot < 0)
        return NULL;
    for (bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        uint32_t vi;
        if (!blk)
            continue;
        for (vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_SET_SHARED || (int) v->aux_int != slot || v->nargs < 1)
                continue;
            const XiImportRef *ref = value_import_ref(v->args[0]);
            if (ref)
                return ref;
        }
    }
    return NULL;
}

static const XiImportRef *module_slot_import_ref(const XiModule *mod, int slot) {
    if (!mod || slot < 0 || slot >= mod->nslots || !mod->slot_imports)
        return NULL;
    return mod->slot_imports[slot];
}

static const XiModule *bundle_module_for_func(const XaotBundle *bundle, const XiFunc *func) {
    const XaotFuncPlan *plan;
    if (!bundle || !func)
        return NULL;
    plan = xaot_bundle_find_func_plan(bundle, func);
    if (plan && plan->module_index < bundle->nmodules)
        return bundle->modules[plan->module_index];
    for (const XiFunc *f = func; f; f = f->parent_func) {
        if (f->module)
            return f->module;
    }
    return NULL;
}

static bool module_matches_import(const XiModule *mod, const XiImportRef *ref) {
    if (!mod || !ref || !ref->module_path)
        return false;
    if (mod->path && strcmp(mod->path, ref->module_path) == 0)
        return true;
    if (mod->name && strcmp(mod->name, ref->module_path) == 0)
        return true;
    return false;
}

static const XiFunc *resolve_export_in_module(const XiModule *mod, const XiImportRef *ref) {
    uint16_t ei;

    if (!mod || !ref)
        return NULL;
    if (ref->resolved_shared_slot >= 0 && ref->resolved_shared_slot < mod->nslots &&
        mod->slot_funcs) {
        const XiFunc *slot_func = mod->slot_funcs[ref->resolved_shared_slot];
        if (slot_func)
            return slot_func;
    }
    if (!ref->member_name)
        return NULL;
    for (ei = 0; ei < mod->nexports; ei++) {
        const XiModuleExport *exp = &mod->exports[ei];
        if (exp->function && exp->name && strcmp(exp->name, ref->member_name) == 0)
            return exp->function;
    }
    return NULL;
}

static const XiFunc *resolve_import_ref(const XaotBundle *bundle, const XiImportRef *ref) {
    uint32_t mi;

    if (!bundle || !ref)
        return NULL;
    if (ref->resolved_mod_index >= 0 && (uint32_t) ref->resolved_mod_index < bundle->nmodules) {
        const XiFunc *target =
            resolve_export_in_module(bundle->modules[ref->resolved_mod_index], ref);
        if (target)
            return target;
    }
    for (mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *mod = bundle->modules[mi];
        const XiFunc *target;
        if (!module_matches_import(mod, ref))
            continue;
        target = resolve_export_in_module(mod, ref);
        if (target)
            return target;
    }
    return NULL;
}

static const XiImportRef *module_import_ref_for_value(const XaotBundle *bundle,
                                                      const XiFunc *current, const XiValue *value) {
    const XiModule *mod;
    const XiValue *v = xi_value_trace_repr(value);
    const XiImportRef *ref = value_import_ref(v);

    if (ref && !ref->member_name)
        return ref;
    if (!v || v->op != XI_GET_SHARED)
        return NULL;

    int slot = (int) v->aux_int;
    mod = bundle_module_for_func(bundle, current);
    ref = module_slot_import_ref(mod, slot);
    if (ref && !ref->member_name)
        return ref;
    ref = shared_slot_import_ref(current, slot);
    if (!ref && current && current->module && current->module->init != current)
        ref = shared_slot_import_ref(current->module->init, slot);
    return ref && !ref->member_name ? ref : NULL;
}

static const XiFunc *resolve_module_member_target(const XaotBundle *bundle, const XiFunc *current,
                                                  const XiValue *call) {
    const char *member_name;
    const XiImportRef *module_ref;
    XiImportRef member_ref;

    if (!bundle || !call || call->op != XI_CALL_METHOD || call->nargs < 1 || !call->aux)
        return NULL;
    if ((call->aux_int & 1) != 0)
        return NULL;

    member_name = (const char *) call->aux;
    module_ref = module_import_ref_for_value(bundle, current, call->args[0]);
    if (!module_ref)
        return NULL;

    member_ref = *module_ref;
    member_ref.member_name = member_name;
    member_ref.resolved_shared_slot = -1;
    member_ref.resolved_export_slot = -1;
    return resolve_import_ref(bundle, &member_ref);
}

static const XiImportRef *binding_import_ref_for_value(const XaotBundle *bundle,
                                                       const XiFunc *current,
                                                       const XiValue *value) {
    const XiValue *v = xi_value_trace_repr(value);
    const XiImportRef *ref = value_import_ref(v);
    const XiModule *mod;

    if (ref)
        return ref;
    if (!v || v->op != XI_GET_SHARED)
        return NULL;
    mod = bundle_module_for_func(bundle, current);
    ref = module_slot_import_ref(mod, (int) v->aux_int);
    if (ref)
        return ref;
    ref = shared_slot_import_ref(current, (int) v->aux_int);
    if (!ref && current && current->module && current->module->init != current)
        ref = shared_slot_import_ref(current->module->init, (int) v->aux_int);
    return ref;
}

static const XiClassData *resolve_imported_class(const XaotBundle *bundle, const XiImportRef *ref,
                                                 const XiModule **owner_out) {
    if (owner_out)
        *owner_out = NULL;
    if (!bundle || !ref || !ref->member_name)
        return NULL;
    for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *mod = bundle->modules[mi];
        bool resolved_match =
            ref->resolved_mod_index >= 0 && (uint32_t) ref->resolved_mod_index == mi;
        if (!mod || (!resolved_match && !module_matches_import(mod, ref)))
            continue;
        if (resolved_match && ref->resolved_shared_slot >= 0 &&
            ref->resolved_shared_slot < mod->nslots && mod->slot_classes &&
            mod->slot_classes[ref->resolved_shared_slot]) {
            if (owner_out)
                *owner_out = mod;
            return mod->slot_classes[ref->resolved_shared_slot];
        }
        for (uint16_t ei = 0; ei < mod->nexports; ei++) {
            const XiModuleExport *exp = &mod->exports[ei];
            if (!exp->class_data || !exp->name || strcmp(exp->name, ref->member_name) != 0)
                continue;
            if (owner_out)
                *owner_out = mod;
            return exp->class_data;
        }
    }
    return NULL;
}

static const XiFunc *resolve_imported_static_method_target(const XaotBundle *bundle,
                                                           const XiFunc *current,
                                                           const XiValue *call) {
    const XiImportRef *ref;
    XiImportRef nested_ref;
    const XiClassData *cls;
    const XiModule *owner;
    const char *method_name;

    if (!bundle || !current || !call || call->op != XI_CALL_METHOD || call->nargs < 1 ||
        !call->aux || (call->aux_int & 1) != 0)
        return NULL;
    ref = binding_import_ref_for_value(bundle, current, call->args[0]);
    if (!ref) {
        const XiValue *receiver = xi_value_trace_repr(call->args[0]);
        const XiImportRef *module_ref =
            receiver && receiver->op == XI_LOAD_FIELD && receiver->nargs >= 1 && receiver->aux
                ? module_import_ref_for_value(bundle, current, receiver->args[0])
                : NULL;
        if (module_ref) {
            nested_ref = *module_ref;
            nested_ref.member_name = (const char *) receiver->aux;
            nested_ref.resolved_shared_slot = -1;
            nested_ref.resolved_export_slot = -1;
            ref = &nested_ref;
        }
    }
    cls = resolve_imported_class(bundle, ref, &owner);
    method_name = (const char *) call->aux;
    if (!cls || !owner || !owner->init || !cls->methods || !cls->child_idx)
        return NULL;
    for (uint16_t mi = 0; mi < cls->nmethod; mi++) {
        const XiClassMethod *method = &cls->methods[mi];
        uint16_t child_idx;
        if (!method->is_static || method->is_static_constructor || !method->name ||
            strcmp(method->name, method_name) != 0)
            continue;
        child_idx = cls->child_idx[mi];
        return child_idx < owner->init->nchildren ? owner->init->children[child_idx] : NULL;
    }
    return NULL;
}

static const XiFunc *resolve_local_static_method_target(const XaotBundle *bundle,
                                                        const XiFunc *current,
                                                        const XiValue *call) {
    if (!bundle || !current || !call || call->op != XI_CALL_METHOD || call->nargs < 1 ||
        !call->aux || (call->aux_int & 1) != 0)
        return NULL;
    const XiValue *receiver = xi_value_trace_repr(call->args[0]);
    const XiModule *mod = bundle_module_for_func(bundle, current);
    if (!receiver || receiver->op != XI_GET_SHARED || !mod || !mod->slot_classes ||
        receiver->aux_int < 0 || receiver->aux_int >= mod->nslots)
        return NULL;
    const XiClassData *cls = mod->slot_classes[receiver->aux_int];
    if (!cls || !cls->methods || !cls->child_idx || !mod->init)
        return NULL;
    const char *method_name = (const char *) call->aux;
    for (uint16_t mi = 0; mi < cls->nmethod; mi++) {
        const XiClassMethod *method = &cls->methods[mi];
        if (!method->is_static || method->is_static_constructor || !method->name ||
            strcmp(method->name, method_name) != 0)
            continue;
        uint16_t child_idx = cls->child_idx[mi];
        return child_idx < mod->init->nchildren ? mod->init->children[child_idx] : NULL;
    }
    return NULL;
}

static const XiFunc *resolve_shared_function(const XaotBundle *bundle, const XiFunc *current,
                                             int slot) {
    const XiModule *mod = NULL;
    const XiFunc *f;
    const XiImportRef *ref;

    if (!current || slot < 0)
        return NULL;
    mod = bundle_module_for_func(bundle, current);
    /* Slot metadata lives on the module init function; walk the lexical
     * parent chain so calls made inside nested functions resolve too. */
    for (f = current; f; f = f->parent_func) {
        if (f->shared_slot_funcs && slot < f->shared_slot_func_count && f->shared_slot_funcs[slot])
            return f->shared_slot_funcs[slot];
    }
    if (mod && slot < mod->nslots && mod->slot_funcs && mod->slot_funcs[slot])
        return mod->slot_funcs[slot];

    ref = module_slot_import_ref(mod, slot);
    if (ref)
        return resolve_import_ref(bundle, ref);

    ref = shared_slot_import_ref(current, slot);
    if (!ref && mod && mod->init && mod->init != current)
        ref = shared_slot_import_ref(mod->init, slot);
    return resolve_import_ref(bundle, ref);
}

static const char *receiver_class_name(const XiValue *recv) {
    if (!recv || !recv->type)
        return NULL;
    if ((recv->type->kind == XR_KIND_CLASS || recv->type->kind == XR_KIND_INSTANCE) &&
        recv->type->instance.class_name)
        return recv->type->instance.class_name;
    return NULL;
}

static const XiFunc *method_func_from_class(const XiModule *mod, const XiClassData *cd,
                                            const char *method_name, int method_index,
                                            bool is_static_call) {
    if (!mod || !mod->init || !cd || !cd->methods || !cd->child_idx)
        return NULL;
    for (uint16_t mi = 0; mi < cd->nmethod; mi++) {
        const XiClassMethod *method = &cd->methods[mi];
        uint16_t child_idx;
        if (method->is_static_constructor || method->is_constructor ||
            method->is_static != is_static_call)
            continue;
        if (method_index >= 0) {
            if ((int) mi != method_index)
                continue;
        } else if (!method_name || !method->name || strcmp(method->name, method_name) != 0) {
            continue;
        }
        if (mi >= cd->ninst + cd->nstat)
            return NULL;
        child_idx = cd->child_idx[mi];
        if (child_idx >= mod->init->nchildren)
            return NULL;
        return mod->init->children[child_idx];
    }
    return NULL;
}

static const XiFunc *resolve_method_target(const XaotBundle *bundle, const XiValue *call) {
    const char *class_name;
    const char *method_name;
    int method_index = -1;
    bool is_static_call;
    uint32_t mi;

    if (!bundle || !call || (call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT) ||
        call->nargs < 1)
        return NULL;
    if (call->op == XI_CALL_METHOD && (call->aux_int & 1) != 0)
        return NULL;

    class_name = receiver_class_name(call->args[0]);
    if (!class_name)
        return NULL;
    is_static_call = call->args[0]->type && call->args[0]->type->kind == XR_KIND_CLASS;
    method_name = call->aux ? (const char *) call->aux : NULL;
    if (call->op == XI_CALL_METHOD_DIRECT)
        method_index = (int) call->aux_int;

    for (mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *mod = bundle->modules[mi];
        uint16_t si;
        if (!mod || !mod->slot_classes)
            continue;
        for (si = 0; si < mod->nslots; si++) {
            const XiClassData *cd = mod->slot_classes[si];
            const XiFunc *target;
            if (!cd || !cd->class_name || strcmp(cd->class_name, class_name) != 0)
                continue;
            target = method_func_from_class(mod, cd, method_name, method_index, is_static_call);
            if (target)
                return target;
        }
    }
    return NULL;
}

static const XiFunc *resolve_dispatch_plan_target(const XaotBundle *bundle, const XiValue *call) {
    const XaotMethodDispatchPlan *plan;
    const XaotDispatchTargetCase *target;
    if (!bundle || !call || (call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT))
        return NULL;
    plan = xaot_bundle_find_method_dispatch_plan_for_xi_call(bundle, call);
    if (!plan || plan->target_count != 1 || plan->target_start == 0 ||
        plan->target_start - 1 >= bundle->ndispatch_target_cases)
        return NULL;
    target = &bundle->dispatch_target_cases[plan->target_start - 1];
    return xaot_bundle_find_dispatch_target_func(bundle, target, NULL);
}

XR_FUNC const XiFunc *xaot_boundary_resolve_direct_call_target(const XaotBundle *bundle,
                                                               const XiFunc *current,
                                                               const XiValue *call,
                                                               uint16_t *first_arg_out) {
    const XiValue *callee;

    if (first_arg_out)
        *first_arg_out = 0;
    if (!bundle || !current || !call)
        return NULL;
    /* Covered typed calls have a single TargetPlan owner.  Returning no
     * legacy answer here makes any missed consumer fail closed instead of
     * silently reconstructing the callee from a closure/name shape. */
    XaotLeafAggregateTargetStatus leaf_aggregate =
        xaot_boundary_leaf_aggregate_function_status(bundle, current, NULL, NULL, NULL, 0);
    if (leaf_aggregate != XAOT_LEAF_AGGREGATE_TARGET_UNCOVERED)
        return NULL;
    XaotDirectI64TargetStatus direct_i64 =
        xaot_boundary_direct_i64_function_status(bundle, current, NULL, NULL, NULL, 0);
    if (direct_i64 != XAOT_DIRECT_I64_TARGET_UNCOVERED)
        return NULL;
    if (call->op == XI_CALL_METHOD || call->op == XI_CALL_METHOD_DIRECT) {
        /* Module-member call: args[0] is the imported namespace object, not a
         * receiver. The target is a plain function whose ABI has no `this`
         * slot, so argument mapping starts at args[1]. */
        const XiFunc *target = resolve_module_member_target(bundle, current, call);
        if (target && first_arg_out)
            *first_arg_out = 1;
        if (target)
            return target;
        target = resolve_local_static_method_target(bundle, current, call);
        if (target) {
            if (first_arg_out)
                *first_arg_out = 1;
            return target;
        }
        target = resolve_imported_static_method_target(bundle, current, call);
        if (target) {
            if (first_arg_out)
                *first_arg_out = 1;
            return target;
        }
        target = resolve_dispatch_plan_target(bundle, call);
        if (target)
            return target;
        return resolve_method_target(bundle, call);
    }
    if (call->op != XI_CALL || call->nargs < 1)
        return NULL;
    if (first_arg_out)
        *first_arg_out = 1;

    callee = xi_value_trace_repr(call->args[0]);
    if (!callee)
        return NULL;
    if (callee->op == XI_CLOSURE_NEW && callee->aux)
        return (const XiFunc *) callee->aux;
    if (callee->op == XI_STACK_ALLOC && callee->aux_int == XI_CLOSURE_NEW && callee->aux)
        return (const XiFunc *) callee->aux;
    if (callee->op == XI_CONST && callee->type && callee->type->kind == XR_KIND_NULL &&
        current->name)
        return current;
    if (callee->op == XI_GET_SHARED) {
        return resolve_shared_function(bundle, current, (int) callee->aux_int);
    }
    if (callee->op == XI_IMPORT_REF && callee->aux)
        return resolve_import_ref(bundle, (const XiImportRef *) callee->aux);
    return NULL;
}
