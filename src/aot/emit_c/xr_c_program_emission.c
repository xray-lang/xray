/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_c_program_emission.c - Program TargetPlan to Xi C-emission binding
 */

#include "xr_c_program_emission.h"
#include "../refine/xr_aot_scalar_value.h"
#include "../../ir/xi_program_semantic.h"
#include "../../ir/xi_program_semantic_plan.h"
#include "../../ir/xi_value_query.h"
#include "../../plan/semantic/xr_semantic_plan.h"
#include "../../plan/target/xr_target_verify.h"
#include "../../base/xmalloc.h"
#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct XrCProgramEmissionContext {
    const XrTargetPlan *target;
    const XrTargetProgramGraphRecord *graph;
    const XrTargetModulePartitionRecord *partitions;
    uint32_t partition_count;
    XiModule *const *modules;
    uint32_t module_count;
} XrCProgramEmissionContext;

typedef struct XrCProgramFunctionExpectation {
    uint32_t target_function;
    uint32_t partition;
    uint32_t semantic_function;
    XrStableId identity;
} XrCProgramFunctionExpectation;

static bool emission_fail(char *error, size_t error_size, const char *format, ...) {
    if (error && error_size) {
        va_list args;
        va_start(args, format);
        int prefix = snprintf(error, error_size, "XR_TARGET_1001: ");
        if (prefix >= 0 && (size_t) prefix < error_size)
            vsnprintf(error + prefix, error_size - (size_t) prefix, format, args);
        va_end(args);
    }
    return false;
}

static XiModule *module_for_partition(const XrCProgramEmissionContext *ctx,
                                      uint32_t partition) {
    if (!ctx || partition >= ctx->partition_count)
        return NULL;
    const XrTargetModulePartitionRecord *row = &ctx->partitions[partition];
    const XrSemanticPlan *semantic =
        xr_target_plan_semantic_module(ctx->target, partition);
    XiModule *match = NULL;
    for (uint32_t i = 0; semantic && i < ctx->module_count; i++) {
        XiModule *candidate = ctx->modules[i];
        if (!candidate || candidate->psc_module_index != row->program_module_row ||
            !candidate->init || candidate->init->semantic_plan != semantic)
            continue;
        if (match)
            return NULL;
        match = candidate;
    }
    return match;
}

static bool format_function_symbol(XrStableId identity,
                                   char out[XR_C_PROGRAM_FUNCTION_SYMBOL_CAPACITY]) {
    char hex[XR_STABLE_ID_BYTES * 2u + 1u];
    xr_stable_id_hex(identity, hex);
    int written = snprintf(out, XR_C_PROGRAM_FUNCTION_SYMBOL_CAPACITY, "xr_pf_%s", hex);
    return written > 0 && (size_t) written < XR_C_PROGRAM_FUNCTION_SYMBOL_CAPACITY;
}

bool xr_c_program_initializer_symbol_identity(
    XrStableId module_identity, XrStableId semantic_function_identity,
    XrStableId *out) {
    char module_hex[XR_STABLE_ID_BYTES * 2u + 1u];
    char function_hex[XR_STABLE_ID_BYTES * 2u + 1u];
    char key[160];
    XrFingerprint digest = {{0}};
    xr_stable_id_hex(module_identity, module_hex);
    xr_stable_id_hex(semantic_function_identity, function_hex);
    int written = snprintf(
        key, sizeof(key),
        "xray-program-c-initializer-v1:module=%s:function=%s",
        module_hex, function_hex);
    return out && written > 0 && (size_t) written < sizeof(key) &&
           xr_stable_id_from_key(key, out, &digest);
}

static bool bind_function(const XrCProgramEmissionContext *ctx,
                          const XrCProgramFunctionExpectation *expected,
                          XrCProgramXiFunctionBinding *out) {
    uint32_t function_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(ctx->target, &function_count);
    if (!functions || expected->target_function >= function_count ||
        expected->partition >= ctx->partition_count)
        return false;
    const XrTargetModulePartitionRecord *partition =
        &ctx->partitions[expected->partition];
    const XrTargetFunctionRecord *target = &functions[expected->target_function];
    if (expected->target_function < partition->functions_begin ||
        expected->target_function - partition->functions_begin >= partition->functions_count ||
        target->id != expected->target_function ||
        target->semantic_function != expected->semantic_function)
        return false;

    const XrSemanticPlan *semantic = NULL;
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    if (!xr_target_plan_function_semantic_binding(ctx->target, expected->target_function,
                                                   &semantic, &semantic_function) ||
        semantic != xr_target_plan_semantic_module(ctx->target, expected->partition) ||
        semantic_function != expected->semantic_function)
        return false;
    const XrSemanticProgramFunctionBinding *program =
        xr_semantic_plan_program_function_for_semantic_function(semantic, semantic_function);
    XiModule *module = module_for_partition(ctx, expected->partition);
    const XrProgramSemanticFunctionRecord *source =
        program && module
            ? xr_program_semantic_closure_function(module->program_semantic_closure,
                                                   program->program_row)
            : NULL;
    const XiFunc *xi_function =
        program && module
            ? xi_program_semantic_function_for_row(module, program->program_row)
            : NULL;
    if (!program || !source || !xi_function ||
        !xr_stable_id_equal(program->program_function, expected->identity) ||
        !xr_stable_id_equal(source->id, expected->identity) ||
        xi_function->semantic_plan != semantic ||
        xi_function->semantic_plan_function_index != semantic_function)
        return false;

    *out = (XrCProgramXiFunctionBinding) {
        .xi_function = xi_function,
        .target_function = expected->target_function,
        .target_partition = expected->partition,
        .semantic_function = semantic_function,
        .program_function = program->program_row,
        .identity = expected->identity,
    };
    return format_function_symbol(expected->identity, out->c_symbol);
}

static bool machine_rep_is_i64(const XrTargetPlan *target, uint16_t rep) {
    const XrTargetMachineRepRecord *row = xr_target_plan_machine_rep(target, rep);
    return row && row->kind == XR_MACHINE_REP_I64;
}

static bool locator_is_exact(XiSourceLocator xi,
                             XrProgramSemanticSourceLocator psc) {
    return xi.kind != 0u && xi.kind == psc.kind && xi.span.start_line != 0u &&
           xi.span.start_line == psc.start_line &&
           xi.span.start_column != 0u &&
           xi.span.start_column == psc.start_column &&
           xi.span.end_line != 0u && xi.span.end_line == psc.end_line &&
           xi.span.end_column != 0u && xi.span.end_column == psc.end_column &&
           (xi.span.end_line > xi.span.start_line ||
            (xi.span.end_line == xi.span.start_line &&
             xi.span.end_column > xi.span.start_column));
}

static bool builder_callee_carrier_is_exact(
    const XrCProgramEmissionContext *ctx,
    const XrCProgramDirectI64EmissionBinding *binding,
    const XrSemanticProgramCallBinding *program_call, const XiValue *carrier) {
    if (!ctx || !binding || !program_call || !carrier ||
        carrier->op != XI_GET_SHARED || !carrier->block ||
        carrier->block->func != binding->caller.xi_function ||
        carrier->aux_int < 0)
        return false;
    const XiImportRef *ref =
        xi_value_import_ref(binding->caller.xi_function, carrier);
    XiModule *source_module =
        module_for_partition(ctx, binding->caller.target_partition);
    XiModule *callee_module =
        module_for_partition(ctx, binding->callee.target_partition);
    const XrProgramSemanticDependencyRecord *dependency =
        ref && source_module && source_module->program_semantic_closure
            ? xr_program_semantic_closure_dependency(
                  source_module->program_semantic_closure,
                  ref->psc_dependency_index)
            : NULL;
    uint32_t callee_module_index = UINT32_MAX;
    for (uint32_t i = 0; callee_module && i < ctx->module_count; i++)
        if (ctx->modules[i] == callee_module)
            callee_module_index = i;
    return ref && dependency && source_module && callee_module &&
           carrier->aux_int < source_module->nslots &&
           source_module->slot_imports &&
           source_module->slot_imports[carrier->aux_int] == ref &&
           xr_stable_id_equal(
               dependency->source_module,
               ctx->partitions[binding->caller.target_partition]
                   .module_identity) &&
           xr_stable_id_equal(
               dependency->dependency_module,
               ctx->partitions[binding->callee.target_partition]
                   .module_identity) &&
           xr_stable_id_equal(dependency->exported_function,
                              ctx->graph->producer_function_identity) &&
           xr_stable_id_equal(dependency->resolver_binding,
                              ctx->graph->resolver_binding) &&
           xr_stable_id_equal(ref->psc_resolver_binding,
                              program_call->resolver_binding) &&
           xr_stable_id_equal(ref->psc_resolver_binding,
                              dependency->resolver_binding) &&
           locator_is_exact(ref->psc_import_locator,
                            dependency->import_locator) &&
           ref->resolution_attempted && callee_module_index != UINT32_MAX &&
           ref->resolved_mod_index >= 0 &&
           (uint32_t) ref->resolved_mod_index == callee_module_index &&
           ref->resolved_module == callee_module &&
           ref->resolved_func == binding->callee.xi_function &&
           ref->resolved_shared_slot >= 0 &&
           ref->resolved_shared_slot < callee_module->nslots &&
           callee_module->slot_funcs &&
           callee_module->slot_funcs[ref->resolved_shared_slot] ==
               binding->callee.xi_function &&
           ref->resolved_export_slot >= 0 &&
           ref->resolved_export_slot < callee_module->nexports &&
           callee_module->exports &&
           callee_module->exports[ref->resolved_export_slot].function ==
               binding->callee.xi_function &&
           callee_module->exports[ref->resolved_export_slot].shared_slot ==
               ref->resolved_shared_slot;
}

static bool machine_rep_c_projection(const XrTargetMachineRepRecord *row,
                                     XrCValueRep *rep, const char **c_type) {
    if (!row || !rep || !c_type)
        return false;
    switch ((XrMachineRepKind) row->kind) {
        case XR_MACHINE_REP_VOID:
            *rep = XR_C_VALUE_REP_VOID;
            *c_type = "void";
            return true;
        case XR_MACHINE_REP_I64:
            *rep = XR_C_VALUE_REP_I64;
            *c_type = "int64_t";
            break;
        case XR_MACHINE_REP_DYN_VALUE:
            *rep = XR_C_VALUE_REP_TAGGED;
            *c_type = "XrValue";
            break;
        default:
            return false;
    }
    return row->register_bits != 0u && row->memory_size != 0u &&
           row->memory_align != 0u;
}

static const XiValue *function_value_for_local_id(const XiFunc *function,
                                                   uint32_t local_value) {
    const XiValue *match = NULL;
    for (uint32_t parameter = 0; function && parameter < function->nparams;
         parameter++) {
        const XiValue *candidate =
            function->params ? function->params[parameter] : NULL;
        if (!candidate || candidate->id != local_value)
            continue;
        if (match && match != candidate)
            return NULL;
        match = candidate;
    }
    for (uint32_t block_index = 0; function && block_index < function->nblocks;
         block_index++) {
        const XiBlock *block = function->blocks ? function->blocks[block_index] : NULL;
        for (uint32_t value_index = 0; block && value_index < block->nvalues;
             value_index++) {
            const XiValue *candidate = block->values ? block->values[value_index] : NULL;
            if (!candidate || candidate->id != local_value)
                continue;
            if (match && match != candidate)
                return NULL;
            match = candidate;
        }
        for (const XiPhi *phi = block ? block->phis : NULL; phi; phi = phi->next) {
            if (phi->value.id != local_value)
                continue;
            if (match && match != &phi->value)
                return NULL;
            match = &phi->value;
        }
    }
    return match;
}

static bool bind_initializer_value_owner(
    const XrCProgramEmissionContext *ctx, uint32_t partition,
    XrCProgramXiFunctionBinding *out) {
    if (!ctx || !out || partition >= ctx->partition_count)
        return false;
    XiModule *module = module_for_partition(ctx, partition);
    const XrSemanticPlan *semantic =
        xr_target_plan_semantic_module(ctx->target, partition);
    const XiFunc *initializer = module ? module->init : NULL;
    uint32_t semantic_function =
        initializer ? initializer->semantic_plan_function_index
                    : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticFunctionRecord *semantic_row =
        semantic && semantic_function != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_function(semantic, semantic_function)
            : NULL;
    uint32_t target_function = UINT32_MAX;
    uint32_t target_function_count = 0;
    const XrTargetFunctionRecord *target_functions =
        xr_target_plan_functions(ctx->target, &target_function_count);
    const XrTargetModulePartitionRecord *partition_row =
        &ctx->partitions[partition];
    const XrSemanticPlan *bound_semantic = NULL;
    uint32_t bound_function = XR_SEMANTIC_INDEX_NONE;
    XrStableId symbol_identity = {{0}};
    if (!initializer || !semantic || !semantic_row ||
        !semantic_row->is_module_initializer ||
        initializer->semantic_plan != semantic || !target_functions ||
        !xr_target_plan_find_function(ctx->target, semantic,
                                      semantic_function, &target_function) ||
        target_function >= target_function_count ||
        target_function < partition_row->functions_begin ||
        target_function - partition_row->functions_begin >=
            partition_row->functions_count ||
        target_functions[target_function].id != target_function ||
        target_functions[target_function].semantic_function !=
            semantic_function ||
        !xr_target_plan_function_semantic_binding(
            ctx->target, target_function, &bound_semantic, &bound_function) ||
        bound_semantic != semantic || bound_function != semantic_function ||
        !xr_c_program_initializer_symbol_identity(partition_row->module_identity,
                                                  semantic_row->id,
                                                  &symbol_identity))
        return false;
    *out = (XrCProgramXiFunctionBinding) {
        .xi_function = initializer,
        .target_function = target_function,
        .target_partition = partition,
        .semantic_function = semantic_function,
        .program_function = XR_SEMANTIC_INDEX_NONE,
        .identity = symbol_identity,
    };
    return format_function_symbol(symbol_identity, out->c_symbol);
}

static bool project_value(const XrCProgramEmissionContext *ctx,
                          const XrCProgramXiFunctionBinding *function,
                          const XiValue *value,
                          XrCProgramValueEmissionBinding *out) {
    const XrSemanticPlan *semantic =
        ctx && function
            ? xr_target_plan_semantic_module(ctx->target,
                                             function->target_partition)
            : NULL;
    const XrSemanticFunctionRecord *semantic_function =
        semantic ? xr_semantic_plan_function(semantic,
                                             function->semantic_function)
                 : NULL;
    if (!ctx || !function || !value || !out || !semantic_function ||
        value->block == NULL || value->block->func != function->xi_function ||
        value->id >= semantic_function->value_count ||
        semantic_function->value_begin > UINT32_MAX - value->id)
        return false;
    uint32_t semantic_function_index = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    if (!xr_aot_scalar_program_semantic_value_id(
            ctx->target, function->target_partition,
            function->target_function, function->xi_function, value,
            &semantic_function_index, &semantic_value, NULL, 0) ||
        semantic_function_index != function->semantic_function ||
        semantic_value != semantic_function->value_begin + value->id)
        return false;
    const XrTargetValueRepRecord *target_value =
        xr_target_plan_value_rep_for_module(
            ctx->target, function->target_partition, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        target_value
            ? xr_target_plan_machine_rep(ctx->target,
                                         target_value->register_rep)
            : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        target_value
            ? xr_target_plan_machine_rep(ctx->target,
                                         target_value->memory_rep)
            : NULL;
    XrCValueRep c_rep = XR_C_VALUE_REP_COUNT;
    const char *c_type = NULL;
    if (!target_value || target_value->semantic_value != semantic_value ||
        !register_rep || !memory_rep || register_rep->kind != memory_rep->kind ||
        !machine_rep_c_projection(register_rep, &c_rep, &c_type))
        return false;
    XrCValueRep memory_c_rep = XR_C_VALUE_REP_COUNT;
    const char *memory_c_type = NULL;
    if (!machine_rep_c_projection(memory_rep, &memory_c_rep, &memory_c_type) ||
        memory_c_rep != c_rep || strcmp(memory_c_type, c_type) != 0)
        return false;
    *out = (XrCProgramValueEmissionBinding) {
        .xi_function = function->xi_function,
        .xi_value = value,
        .target_function = function->target_function,
        .target_partition = function->target_partition,
        .semantic_function = function->semantic_function,
        .emission = {
            .semantic_value = semantic_value,
            .target_register_rep = target_value->register_rep,
            .target_memory_rep = target_value->memory_rep,
            .target_register_kind = register_rep->kind,
            .target_memory_kind = memory_rep->kind,
            .register_bits = register_rep->register_bits,
            .memory_align = memory_rep->memory_align,
            .memory_size = memory_rep->memory_size,
            .rep = (uint8_t) c_rep,
            .materialization = XR_C_VALUE_MATERIALIZATION_NONE,
            .recipe_operand_value = UINT32_MAX,
            .recipe_argument_value = UINT32_MAX,
            .recipe_callee_function = UINT32_MAX,
            .backing_value = UINT32_MAX,
            .c_type = c_type,
        },
    };
    return true;
}

static bool bind_direct_call(const XrCProgramEmissionContext *ctx,
                             XrCProgramDirectI64EmissionBinding *out) {
    uint32_t function_count = 0, call_count = 0, argument_count = 0,
             instruction_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(ctx->target, &function_count);
    const XrTargetCallRecord *calls = xr_target_plan_calls(ctx->target, &call_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(ctx->target, &argument_count);
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_instructions(ctx->target, &instruction_count);
    if (!functions || out->caller.target_function >= function_count ||
        out->callee.target_function >= function_count || !calls || !arguments ||
        !instructions || ctx->graph->target_call >= call_count ||
        ctx->graph->target_argument >= argument_count)
        return false;
    const XrTargetCallRecord *call = &calls[ctx->graph->target_call];
    const XrTargetCallArgumentRecord *argument =
        &arguments[ctx->graph->target_argument];
    if (call->id != ctx->graph->target_call ||
        call->caller_function != ctx->graph->entry_target_function ||
        call->callee_function != ctx->graph->producer_target_function ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_PROGRAM_DIRECT ||
        call->target_kind != XR_TARGET_CALL_TARGET_PROGRAM_DIRECT ||
        call->argument_begin != ctx->graph->target_argument || call->argument_count != 1u ||
        argument->call != ctx->graph->target_call ||
        argument->ordinal != ctx->graph->argument_ordinal ||
        argument->caller_slot != ctx->graph->caller_slot ||
        argument->callee_slot != ctx->graph->callee_slot ||
        !machine_rep_is_i64(ctx->target, call->result_register_rep) ||
        !machine_rep_is_i64(ctx->target, call->result_memory_rep) ||
        !machine_rep_is_i64(ctx->target, argument->register_rep) ||
        !machine_rep_is_i64(ctx->target, argument->memory_rep) ||
        !machine_rep_is_i64(ctx->target, argument->callee_register_rep) ||
        !machine_rep_is_i64(ctx->target, argument->callee_memory_rep))
        return false;

    const XrSemanticPlan *entry =
        xr_target_plan_semantic_module(ctx->target, ctx->graph->entry_partition);
    const XrSemanticProgramCallBinding *program_call =
        entry ? xr_semantic_plan_program_call_for_operation(entry, call->semantic_operation)
              : NULL;
    const XiValue *xi_call =
        program_call
            ? xi_program_semantic_call_for_row(out->caller.xi_function,
                                               program_call->program_row)
            : NULL;
    if (!program_call || !xi_call || xi_call->block == NULL ||
        xi_call->block->func != out->caller.xi_function ||
        xi_call->op != XI_CALL || xi_call->nargs != 2u || !xi_call->args ||
        !xi_call->args[0] || xi_call->args[0]->op != XI_GET_SHARED ||
        !xi_call->args[1] || xi_call->args[1]->block == NULL ||
        xi_call->args[1]->block->func != out->caller.xi_function ||
        program_call->operation != ctx->graph->entry_semantic_operation ||
        !xr_stable_id_equal(program_call->program_call, ctx->graph->call_identity) ||
        !xr_stable_id_equal(program_call->callsite, ctx->graph->callsite_identity) ||
        !xr_stable_id_equal(program_call->caller_program_function,
                            ctx->graph->entry_function_identity) ||
        !xr_stable_id_equal(program_call->callee_program_function,
                            ctx->graph->producer_function_identity) ||
        !xr_stable_id_equal(program_call->resolver_binding,
                            ctx->graph->resolver_binding) ||
        !builder_callee_carrier_is_exact(ctx, out, program_call,
                                         xi_call->args[0]))
        return false;

    const XrTargetInstructionRecord *instruction = NULL;
    uint32_t instruction_row = UINT32_MAX;
    for (uint32_t i = 0; i < instruction_count; i++) {
        const XrTargetInstructionRecord *candidate = &instructions[i];
        if (candidate->function != ctx->graph->entry_target_function ||
            candidate->opcode != XR_TARGET_INSTRUCTION_CALL_DIRECT_I64 ||
            candidate->immediate_bits != ctx->graph->target_call)
            continue;
        if (instruction)
            return false;
        instruction = candidate;
        instruction_row = i;
    }
    if (!instruction || instruction->id != instruction_row ||
        instruction->result_slot != call->result_slot)
        return false;

    uint32_t callee_operand_uses = 0;
    for (uint32_t block_index = 0;
         block_index < out->caller.xi_function->nblocks; block_index++) {
        const XiBlock *block = out->caller.xi_function->blocks[block_index];
        if (!block || block->control == xi_call->args[0])
            return false;
        for (const XiPhi *phi = block->phis; phi; phi = phi->next)
            for (uint16_t argument = 0; argument < phi->value.nargs; argument++)
                if (phi->value.args[argument] == xi_call->args[0])
                    return false;
        for (uint32_t value_index = 0; value_index < block->nvalues;
             value_index++) {
            const XiValue *user = block->values[value_index];
            for (uint16_t argument = 0; user && argument < user->nargs;
                 argument++) {
                if (user->args[argument] != xi_call->args[0])
                    continue;
                if (user != xi_call || argument != 0u)
                    return false;
                callee_operand_uses++;
            }
        }
    }
    if (callee_operand_uses != 1u)
        return false;

    const XrSemanticFunctionRecord *caller_semantic =
        xr_semantic_plan_function(entry, out->caller.semantic_function);
    if (!caller_semantic ||
        xi_call->args[1]->id >= caller_semantic->value_count ||
        caller_semantic->value_begin > UINT32_MAX - xi_call->args[1]->id ||
        argument->semantic_value !=
            caller_semantic->value_begin + xi_call->args[1]->id ||
        xi_call->id >= caller_semantic->value_count ||
        caller_semantic->value_begin > UINT32_MAX - xi_call->id ||
        call->result_value != caller_semantic->value_begin + xi_call->id)
        return false;
    out->xi_call = xi_call;
    out->xi_argument = xi_call->args[1];
    out->xi_callee_operand = xi_call->args[0];
    out->caller_target_row = &functions[out->caller.target_function];
    out->callee_target_row = &functions[out->callee.target_function];
    out->call_row = call;
    out->argument_row = argument;
    out->instruction_row = instruction;
    out->target_call = ctx->graph->target_call;
    out->target_instruction = instruction_row;
    out->target_argument = ctx->graph->target_argument;
    out->semantic_operation = call->semantic_operation;
    out->program_call = program_call->program_row;
    out->callee_operand_elided = true;
    return true;
}

static bool append_function_values(
    const XrCProgramEmissionContext *ctx,
    const XrCProgramXiFunctionBinding *function,
    XrCProgramValueEmissionBinding *values, uint32_t capacity,
    uint32_t *count) {
    const XrSemanticPlan *semantic =
        xr_target_plan_semantic_module(ctx->target,
                                       function->target_partition);
    const XrSemanticFunctionRecord *semantic_function =
        semantic ? xr_semantic_plan_function(semantic,
                                             function->semantic_function)
                 : NULL;
    if (!semantic_function || !values || !count ||
        semantic_function->value_count > capacity - *count)
        return false;
    for (uint32_t local_value = 0; local_value < semantic_function->value_count;
         local_value++) {
        const XiValue *value =
            function_value_for_local_id(function->xi_function, local_value);
        if (!value)
            continue;
        if (!project_value(ctx, function, value, &values[*count]))
            return false;
        (*count)++;
    }
    return true;
}

static bool project_function_abi(
    const XrCProgramEmissionContext *ctx,
    const XrCProgramDirectI64EmissionBinding *binding,
    const XrCProgramXiFunctionBinding *function, uint16_t ordinal,
    XrCProgramFunctionAbiEmissionBinding *out) {
    const XrSemanticPlan *semantic =
        xr_target_plan_semantic_module(ctx->target,
                                       function->target_partition);
    const XrSemanticFunctionRecord *semantic_function =
        semantic ? xr_semantic_plan_function(semantic,
                                             function->semantic_function)
                 : NULL;
    if (!semantic_function || !out || ordinal > semantic_function->parameter_count ||
        semantic_function->is_module_initializer || semantic_function->capture_count != 0u ||
        (semantic_function->semantic_effects &
         (XI_EFFECT_MAY_THROW | XI_EFFECT_MAY_SUSPEND)) != 0u)
        return false;

    const XiValue *live_value = NULL;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    if (ordinal != 0u) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(
                semantic,
                semantic_function->parameter_begin + (uint32_t) ordinal - 1u);
        live_value = function->xi_function->params &&
                             ordinal <= function->xi_function->nparams
                         ? function->xi_function->params[ordinal - 1u]
                         : NULL;
        if (!parameter || parameter->function != function->semantic_function ||
            parameter->mode != XR_PARAM_READ || !live_value)
            return false;
        semantic_value = parameter->value;
    } else {
        uint32_t matches = 0;
        for (uint32_t block_index = 0;
             block_index < function->xi_function->nblocks; block_index++) {
            const XiBlock *block = function->xi_function->blocks
                                       ? function->xi_function->blocks[block_index]
                                       : NULL;
            if (!block || block->kind != XI_BLOCK_RETURN || !block->control)
                continue;
            if (block->id >= semantic_function->block_count)
                return false;
            const XrSemanticBlockRecord *semantic_block =
                xr_semantic_plan_block(
                    semantic, semantic_function->block_begin + block->id);
            if (!semantic_block ||
                semantic_block->function != function->semantic_function ||
                semantic_block->kind != XI_BLOCK_RETURN ||
                semantic_block->control_value == XR_SEMANTIC_INDEX_NONE ||
                (matches != 0u &&
                 semantic_value != semantic_block->control_value))
                return false;
            live_value = block->control;
            semantic_value = semantic_block->control_value;
            matches++;
        }
        if (matches != 1u || !live_value)
            return false;
    }

    XrCProgramValueEmissionBinding value = {0};
    if (!project_value(ctx, function, live_value, &value) ||
        value.emission.semantic_value != semantic_value ||
        value.emission.rep != XR_C_VALUE_REP_I64 ||
        value.emission.target_register_kind != XR_MACHINE_REP_I64 ||
        value.emission.target_memory_kind != XR_MACHINE_REP_I64)
        return false;
    if (function == &binding->caller) {
        if (ordinal != 0u || !binding->call_row ||
            binding->call_row->result_value != semantic_value ||
            binding->call_row->result_register_rep !=
                value.emission.target_register_rep ||
            binding->call_row->result_memory_rep !=
                value.emission.target_memory_rep)
            return false;
    } else if (function == &binding->callee) {
        if (ordinal == 0u) {
            if (!binding->call_row ||
                binding->call_row->result_register_rep !=
                    value.emission.target_register_rep ||
                binding->call_row->result_memory_rep !=
                    value.emission.target_memory_rep)
                return false;
        } else if (ordinal == 1u) {
            if (!binding->argument_row ||
                binding->argument_row->callee_parameter !=
                    semantic_function->parameter_begin ||
                binding->argument_row->callee_register_rep !=
                    value.emission.target_register_rep ||
                binding->argument_row->callee_memory_rep !=
                    value.emission.target_memory_rep)
                return false;
        } else {
            return false;
        }
    } else {
        return false;
    }

    *out = (XrCProgramFunctionAbiEmissionBinding) {
        .xi_function = function->xi_function,
        .target_function = function->target_function,
        .emission = {
            .semantic_function = function->semantic_function,
            .semantic_value = semantic_value,
            .ordinal = ordinal,
            .parameter_count = semantic_function->parameter_count,
            .target_register_kind = XR_MACHINE_REP_I64,
            .target_memory_kind = XR_MACHINE_REP_I64,
            .slot_class = XR_C_ABI_SLOT_VALUE,
            .boundary_kind = XR_C_ABI_BOUNDARY_NATIVE,
            .rep = XR_C_VALUE_REP_I64,
            .pointee_rep = XR_C_VALUE_REP_VOID,
            .aggregate_class = XR_C_ABI_AGGREGATE_NONE,
            .c_type = "int64_t",
        },
    };
    return true;
}

static bool project_initializer_abi(
    const XrCProgramEmissionContext *ctx,
    const XrCProgramXiFunctionBinding *function,
    XrCProgramFunctionAbiEmissionBinding *out) {
    const XrSemanticPlan *semantic =
        ctx && function
            ? xr_target_plan_semantic_module(ctx->target,
                                             function->target_partition)
            : NULL;
    const XrSemanticFunctionRecord *semantic_function =
        semantic ? xr_semantic_plan_function(semantic,
                                             function->semantic_function)
                 : NULL;
    if (!semantic_function || !out ||
        !semantic_function->is_module_initializer ||
        semantic_function->parameter_count != 0u ||
        function->xi_function == NULL ||
        function->xi_function->semantic_plan != semantic ||
        function->xi_function->semantic_plan_function_index !=
            function->semantic_function)
        return false;
    *out = (XrCProgramFunctionAbiEmissionBinding) {
        .xi_function = function->xi_function,
        .target_function = function->target_function,
        .emission = {
            .semantic_function = function->semantic_function,
            .semantic_value = XR_SEMANTIC_INDEX_NONE,
            .ordinal = 0u,
            .parameter_count = 0u,
            .target_register_kind = XR_MACHINE_REP_VOID,
            .target_memory_kind = XR_MACHINE_REP_VOID,
            .slot_class = XR_C_ABI_SLOT_VALUE,
            .boundary_kind = XR_C_ABI_BOUNDARY_TAGGED,
            .rep = XR_C_VALUE_REP_TAGGED,
            .pointee_rep = XR_C_VALUE_REP_VOID,
            .aggregate_class = XR_C_ABI_AGGREGATE_NONE,
            .c_type = "XrValue",
        },
    };
    return true;
}

static bool build_emission_rows(const XrCProgramEmissionContext *ctx,
                                XrCProgramDirectI64EmissionBinding *binding,
                                char *error, size_t error_size) {
    const XrSemanticPlan *caller_semantic = xr_target_plan_semantic_module(
        ctx->target, binding->caller.target_partition);
    const XrSemanticPlan *callee_semantic = xr_target_plan_semantic_module(
        ctx->target, binding->callee.target_partition);
    const XrSemanticFunctionRecord *caller_function =
        caller_semantic
            ? xr_semantic_plan_function(caller_semantic,
                                        binding->caller.semantic_function)
            : NULL;
    const XrSemanticFunctionRecord *callee_function =
        callee_semantic
            ? xr_semantic_plan_function(callee_semantic,
                                        binding->callee.semantic_function)
            : NULL;
    if (!caller_function || !callee_function ||
        caller_function->parameter_count != 0u ||
        callee_function->parameter_count != 1u ||
        caller_function->value_count >
            UINT32_MAX - callee_function->value_count)
        return emission_fail(error, error_size,
                             "program function ABI shape is not bounded direct-i64");

    uint32_t value_count = caller_function->value_count +
                           callee_function->value_count;
    XrCProgramXiFunctionBinding *initializers =
        (XrCProgramXiFunctionBinding *) xr_calloc(
            ctx->partition_count, sizeof(*initializers));
    if (!initializers)
        return emission_fail(error, error_size,
                             "program initializer binding allocation failed");
    binding->initializers = initializers;
    binding->initializer_count = ctx->partition_count;
    for (uint32_t partition = 0; partition < ctx->partition_count; partition++) {
        if (!bind_initializer_value_owner(ctx, partition,
                                          &initializers[partition]))
            return emission_fail(error, error_size,
                                 "program initializer has no exact TargetPlan owner");
        if (memcmp(initializers[partition].c_symbol, binding->caller.c_symbol,
                   sizeof(initializers[partition].c_symbol)) == 0 ||
            memcmp(initializers[partition].c_symbol, binding->callee.c_symbol,
                   sizeof(initializers[partition].c_symbol)) == 0)
            return emission_fail(error, error_size,
                                 "program initializer C symbol is not globally unique");
        for (uint32_t prior = 0; prior < partition; prior++)
            if (memcmp(initializers[partition].c_symbol,
                       initializers[prior].c_symbol,
                       sizeof(initializers[partition].c_symbol)) == 0)
                return emission_fail(error, error_size,
                                     "program initializer C symbols collide");
        const XrSemanticPlan *semantic = xr_target_plan_semantic_module(
            ctx->target, initializers[partition].target_partition);
        const XrSemanticFunctionRecord *initializer =
            semantic ? xr_semantic_plan_function(
                           semantic, initializers[partition].semantic_function)
                     : NULL;
        if (!initializer || initializer->value_count > UINT32_MAX - value_count)
            return emission_fail(error, error_size,
                                 "program initializer value coverage overflowed");
        value_count += initializer->value_count;
    }
    XrCProgramValueEmissionBinding *values =
        value_count
            ? (XrCProgramValueEmissionBinding *) xr_calloc(
                  value_count, sizeof(*values))
            : NULL;
    if (value_count && !values)
        return emission_fail(error, error_size,
                             "program C-emission value allocation failed");
    uint32_t projected = 0;
    if (!append_function_values(ctx, &binding->caller, values, value_count,
                                &projected)) {
        xr_free(values);
        return emission_fail(error, error_size,
                             "caller values have no exact C-emission projection");
    }
    if (!append_function_values(ctx, &binding->callee, values, value_count,
                                &projected)) {
        xr_free(values);
        return emission_fail(error, error_size,
                             "callee values have no exact C-emission projection");
    }
    for (uint32_t partition = 0; partition < ctx->partition_count; partition++) {
        if (!append_function_values(ctx, &initializers[partition], values,
                                    value_count, &projected)) {
            xr_free(values);
            return emission_fail(
                error, error_size,
                "module initializer values have no exact C-emission projection");
        }
    }
    if (ctx->partition_count > UINT32_MAX - 3u) {
        xr_free(values);
        return emission_fail(error, error_size,
                             "program initializer ABI coverage overflowed");
    }
    uint32_t abi_count = 3u + ctx->partition_count;
    XrCProgramFunctionAbiEmissionBinding *abis =
        (XrCProgramFunctionAbiEmissionBinding *) xr_calloc(
            abi_count, sizeof(*abis));
    if (!abis) {
        xr_free(values);
        return emission_fail(error, error_size,
                             "program C-emission ABI allocation failed");
    }
    if (!project_function_abi(ctx, binding, &binding->caller, 0u, &abis[0]) ||
        !project_function_abi(ctx, binding, &binding->callee, 0u, &abis[1]) ||
        !project_function_abi(ctx, binding, &binding->callee, 1u, &abis[2])) {
        xr_free(abis);
        xr_free(values);
        return emission_fail(error, error_size,
                             "program function ABI has no exact C-emission projection");
    }
    for (uint32_t i = 0; i < ctx->partition_count; i++) {
        if (project_initializer_abi(ctx, &initializers[i], &abis[3u + i]))
            continue;
        xr_free(abis);
        xr_free(values);
        return emission_fail(
            error, error_size,
            "program initializer ABI has no exact C-emission projection");
    }
    binding->values = values;
    binding->value_count = projected;
    binding->function_abis = abis;
    binding->function_abi_count = abi_count;
    return true;
}

static bool prepare_context(const XrTargetPlan *target_plan,
                            XiModule *const *modules, uint32_t module_count,
                            XrCProgramEmissionContext *out, char *error,
                            size_t error_size) {
    if (error && error_size)
        error[0] = '\0';
    if (!target_plan || !modules || !out ||
        !xr_target_plan_is_verified(target_plan) ||
        !xr_target_plan_fingerprint_is_intact(target_plan) ||
        !xr_target_plan_verify(target_plan, error, error_size))
        return emission_fail(error, error_size, "verified program TargetPlan is missing");

    uint32_t graph_count = 0, partition_count = 0;
    const XrTargetProgramGraphRecord *graphs =
        xr_target_plan_program_graphs(target_plan, &graph_count);
    const XrTargetModulePartitionRecord *partitions =
        xr_target_plan_module_partitions(target_plan, &partition_count);
    if (!graphs || graph_count != 1u || !partitions || partition_count != module_count ||
        module_count != 2u || graphs[0].schema != XR_TARGET_PROGRAM_GRAPH_SCHEMA_VERSION ||
        graphs[0].family != XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL ||
        graphs[0].module_count != module_count || graphs[0].function_count != 2u ||
        graphs[0].call_count != 1u || graphs[0].argument_count != 1u ||
        graphs[0].flags !=
            (XR_TARGET_PROGRAM_GRAPH_SINGLE_PLAN | XR_TARGET_PROGRAM_GRAPH_DIRECT_I64))
        return emission_fail(error, error_size, "program C-emission graph is not exact");

    *out = (XrCProgramEmissionContext) {
        .target = target_plan,
        .graph = &graphs[0],
        .partitions = partitions,
        .partition_count = partition_count,
        .modules = modules,
        .module_count = module_count,
    };
    XiModule *entry_module =
        module_for_partition(out, graphs[0].entry_partition);
    uint32_t entry_index = UINT32_MAX;
    for (uint32_t i = 0; entry_module && i < module_count; i++)
        if (modules[i] == entry_module)
            entry_index = i;
    if (entry_index == UINT32_MAX ||
        !xi_program_semantic_verify_module_set(modules, module_count, entry_index, NULL,
                                               error, error_size) ||
        !xi_program_semantic_plan_verify_module_set(modules, module_count, entry_index,
                                                    error, error_size))
        return emission_fail(error, error_size, "program Xi module set is not exact");
    return true;
}

static void graph_function_expectations(
    const XrTargetProgramGraphRecord *graph,
    XrCProgramFunctionExpectation *caller,
    XrCProgramFunctionExpectation *callee) {
    *caller = (XrCProgramFunctionExpectation) {
        .target_function = graph->entry_target_function,
        .partition = graph->entry_partition,
        .semantic_function = graph->entry_semantic_function,
        .identity = graph->entry_function_identity,
    };
    *callee = (XrCProgramFunctionExpectation) {
        .target_function = graph->producer_target_function,
        .partition = graph->producer_partition,
        .semantic_function = graph->producer_semantic_function,
        .identity = graph->producer_function_identity,
    };
}

static bool value_binding_equal(
    const XrCProgramValueEmissionBinding *left,
    const XrCProgramValueEmissionBinding *right) {
    if (!left || !right || left->xi_function != right->xi_function ||
        left->xi_value != right->xi_value ||
        left->target_function != right->target_function ||
        left->target_partition != right->target_partition ||
        left->semantic_function != right->semantic_function)
        return false;
    const XrCValueEmissionView *a = &left->emission;
    const XrCValueEmissionView *b = &right->emission;
#define XR_C_STRING_FIELD_EQUAL(field)                                                     \
    ((!a->field && !b->field) ||                                                           \
     (a->field && b->field && strcmp(a->field, b->field) == 0))
    if (!XR_C_STRING_FIELD_EQUAL(c_type) ||
        !XR_C_STRING_FIELD_EQUAL(backing_c_type) ||
        !XR_C_STRING_FIELD_EQUAL(literal_bytes) ||
        !XR_C_STRING_FIELD_EQUAL(recipe_symbol) ||
        !XR_C_STRING_FIELD_EQUAL(recipe_type_name) ||
        !XR_C_STRING_FIELD_EQUAL(recipe_member_name) ||
        a->recipe_arguments != b->recipe_arguments)
        return false;
#undef XR_C_STRING_FIELD_EQUAL
    return a->semantic_value == b->semantic_value &&
           a->target_register_rep == b->target_register_rep &&
           a->target_memory_rep == b->target_memory_rep &&
           a->target_register_kind == b->target_register_kind &&
           a->target_memory_kind == b->target_memory_kind &&
           a->register_bits == b->register_bits &&
           a->memory_align == b->memory_align &&
           a->memory_size == b->memory_size && a->rep == b->rep &&
           a->materialization == b->materialization &&
           a->reserved == b->reserved &&
           a->literal_byte_length == b->literal_byte_length &&
           a->recipe_operand_value == b->recipe_operand_value &&
           a->recipe_argument_value == b->recipe_argument_value &&
           a->recipe_layout_id == b->recipe_layout_id &&
           a->recipe_discriminant == b->recipe_discriminant &&
           a->recipe_argument_count == b->recipe_argument_count &&
           a->recipe_rule_id == b->recipe_rule_id &&
           a->recipe_callee_function == b->recipe_callee_function &&
           a->recipe_hof_kind == b->recipe_hof_kind &&
           a->recipe_hof_source_storage == b->recipe_hof_source_storage &&
           a->recipe_hof_result_storage == b->recipe_hof_result_storage &&
           a->recipe_hof_callback_parameter_reps[0] ==
               b->recipe_hof_callback_parameter_reps[0] &&
           a->recipe_hof_callback_parameter_reps[1] ==
               b->recipe_hof_callback_parameter_reps[1] &&
           a->recipe_hof_callback_return_rep ==
               b->recipe_hof_callback_return_rep &&
           a->recipe_hof_reserved == b->recipe_hof_reserved &&
           a->backing_value == b->backing_value &&
           a->backing_element_count == b->backing_element_count &&
           a->address_projection == b->address_projection &&
           a->backing_native_type == b->backing_native_type &&
           a->projection_reserved == b->projection_reserved;
}

static bool abi_binding_equal(
    const XrCProgramFunctionAbiEmissionBinding *left,
    const XrCProgramFunctionAbiEmissionBinding *right) {
    if (!left || !right || left->xi_function != right->xi_function ||
        left->target_function != right->target_function)
        return false;
    const XrCFunctionAbiEmissionView *a = &left->emission;
    const XrCFunctionAbiEmissionView *b = &right->emission;
    if ((!a->c_type != !b->c_type) ||
        (a->c_type && strcmp(a->c_type, b->c_type) != 0) ||
        (!a->pointee_c_type != !b->pointee_c_type) ||
        (a->pointee_c_type &&
         strcmp(a->pointee_c_type, b->pointee_c_type) != 0))
        return false;
    return a->semantic_function == b->semantic_function &&
           a->semantic_value == b->semantic_value &&
           a->ordinal == b->ordinal &&
           a->parameter_count == b->parameter_count &&
           a->target_register_kind == b->target_register_kind &&
           a->target_memory_kind == b->target_memory_kind &&
           a->slot_class == b->slot_class &&
           a->boundary_kind == b->boundary_kind && a->rep == b->rep &&
           a->pointee_rep == b->pointee_rep &&
           a->aggregate_class == b->aggregate_class;
}

static bool verifier_symbol_is_exact(
    XrStableId identity,
    const char symbol[XR_C_PROGRAM_FUNCTION_SYMBOL_CAPACITY]) {
    static const char hex[] = "0123456789abcdef";
    static const char prefix[] = "xr_pf_";
    if (!symbol || memchr(symbol, '\0', XR_C_PROGRAM_FUNCTION_SYMBOL_CAPACITY) !=
                       &symbol[XR_C_PROGRAM_FUNCTION_SYMBOL_CAPACITY - 1u] ||
        memcmp(symbol, prefix, sizeof(prefix) - 1u) != 0)
        return false;
    size_t offset = sizeof(prefix) - 1u;
    for (size_t i = 0; i < XR_STABLE_ID_BYTES; i++) {
        if (symbol[offset + i * 2u] != hex[identity.bytes[i] >> 4] ||
            symbol[offset + i * 2u + 1u] != hex[identity.bytes[i] & 0x0fu])
            return false;
    }
    return true;
}

static bool verifier_initializer_symbol_identity(
    XrStableId module_identity, XrStableId semantic_function_identity,
    XrStableId *out) {
    char module_hex[XR_STABLE_ID_BYTES * 2u + 1u];
    char function_hex[XR_STABLE_ID_BYTES * 2u + 1u];
    char key[160];
    XrFingerprint digest = {{0}};
    xr_stable_id_hex(module_identity, module_hex);
    xr_stable_id_hex(semantic_function_identity, function_hex);
    int written = snprintf(
        key, sizeof(key),
        "xray-program-c-initializer-v1:module=%s:function=%s",
        module_hex, function_hex);
    return out && written > 0 && (size_t) written < sizeof(key) &&
           xr_stable_id_from_key(key, out, &digest);
}

static bool verifier_function_binding_is_exact(
    const XrCProgramEmissionContext *ctx,
    const XrCProgramFunctionExpectation *expected,
    const XrCProgramXiFunctionBinding *binding) {
    uint32_t function_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(ctx->target, &function_count);
    if (!binding || !functions || expected->target_function >= function_count ||
        expected->partition >= ctx->partition_count ||
        binding->target_function != expected->target_function ||
        binding->target_partition != expected->partition ||
        binding->semantic_function != expected->semantic_function ||
        !xr_stable_id_equal(binding->identity, expected->identity) ||
        !verifier_symbol_is_exact(expected->identity, binding->c_symbol))
        return false;
    const XrTargetModulePartitionRecord *partition =
        &ctx->partitions[expected->partition];
    const XrTargetFunctionRecord *target = &functions[expected->target_function];
    if (expected->target_function < partition->functions_begin ||
        expected->target_function - partition->functions_begin >=
            partition->functions_count ||
        target->id != expected->target_function ||
        target->semantic_function != expected->semantic_function)
        return false;
    const XrSemanticPlan *semantic = NULL;
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    if (!xr_target_plan_function_semantic_binding(
            ctx->target, expected->target_function, &semantic,
            &semantic_function) ||
        semantic != xr_target_plan_semantic_module(ctx->target,
                                                   expected->partition) ||
        semantic_function != expected->semantic_function)
        return false;
    const XrSemanticProgramFunctionBinding *program =
        xr_semantic_plan_program_function_for_semantic_function(
            semantic, semantic_function);
    XiModule *module = module_for_partition(ctx, expected->partition);
    const XrProgramSemanticFunctionRecord *source =
        program && module
            ? xr_program_semantic_closure_function(
                  module->program_semantic_closure, program->program_row)
            : NULL;
    const XiFunc *function =
        program && module
            ? xi_program_semantic_function_for_row(module, program->program_row)
            : NULL;
    return program && source && function && binding->xi_function == function &&
           binding->program_function == program->program_row &&
           xr_stable_id_equal(program->program_function, expected->identity) &&
           xr_stable_id_equal(source->id, expected->identity) &&
           function->semantic_plan == semantic &&
           function->semantic_plan_function_index == semantic_function;
}

static bool verifier_initializer_value_owner(
    const XrCProgramEmissionContext *ctx, uint32_t partition,
    const XrCProgramXiFunctionBinding *actual) {
    if (!ctx || !actual || partition >= ctx->partition_count)
        return false;
    const XrTargetModulePartitionRecord *partition_row =
        &ctx->partitions[partition];
    const XrSemanticPlan *semantic =
        xr_target_plan_semantic_module(ctx->target, partition);
    XiModule *module = module_for_partition(ctx, partition);
    const XiFunc *initializer = module ? module->init : NULL;
    uint32_t semantic_function =
        initializer ? initializer->semantic_plan_function_index
                    : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticFunctionRecord *semantic_row =
        semantic && semantic_function != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_function(semantic, semantic_function)
            : NULL;
    uint32_t target_function = UINT32_MAX;
    uint32_t target_function_count = 0;
    const XrTargetFunctionRecord *target_functions =
        xr_target_plan_functions(ctx->target, &target_function_count);
    const XrSemanticPlan *target_semantic = NULL;
    uint32_t target_semantic_function = XR_SEMANTIC_INDEX_NONE;
    XrStableId symbol_identity = {{0}};
    if (!semantic || !module || !initializer || !semantic_row ||
        !semantic_row->is_module_initializer ||
        initializer->semantic_plan != semantic || !target_functions ||
        !xr_target_plan_find_function(ctx->target, semantic,
                                      semantic_function, &target_function) ||
        target_function >= target_function_count ||
        target_function < partition_row->functions_begin ||
        target_function - partition_row->functions_begin >=
            partition_row->functions_count ||
        target_functions[target_function].id != target_function ||
        target_functions[target_function].semantic_function !=
            semantic_function ||
        !xr_target_plan_function_semantic_binding(
            ctx->target, target_function, &target_semantic,
            &target_semantic_function) ||
        target_semantic != semantic ||
        target_semantic_function != semantic_function ||
        !verifier_initializer_symbol_identity(partition_row->module_identity,
                                               semantic_row->id,
                                               &symbol_identity))
        return false;
    return actual->xi_function == initializer &&
           actual->target_function == target_function &&
           actual->target_partition == partition &&
           actual->semantic_function == semantic_function &&
           actual->program_function == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(actual->identity, symbol_identity) &&
           verifier_symbol_is_exact(symbol_identity, actual->c_symbol);
}

static bool verifier_callee_operand_has_one_use(const XiFunc *caller,
                                                const XiValue *call,
                                                const XiValue *operand) {
    if (!caller || !call || !operand || operand->op != XI_GET_SHARED)
        return false;
    uint32_t uses = 0;
    for (uint32_t bi = 0; bi < caller->nblocks; bi++) {
        const XiBlock *block = caller->blocks[bi];
        if (!block || block->control == operand)
            return false;
        for (const XiPhi *phi = block->phis; phi; phi = phi->next)
            for (uint16_t ai = 0; ai < phi->value.nargs; ai++)
                if (phi->value.args[ai] == operand)
                    return false;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *user = block->values[vi];
            for (uint16_t ai = 0; user && ai < user->nargs; ai++) {
                if (user->args[ai] != operand)
                    continue;
                if (user != call || ai != 0u)
                    return false;
                uses++;
            }
        }
    }
    return uses == 1u;
}

static bool verifier_callee_carrier_is_exact(
    const XrCProgramEmissionContext *ctx,
    const XrCProgramDirectI64EmissionBinding *binding,
    const XrSemanticProgramCallBinding *program_call, const XiValue *carrier) {
    if (!ctx || !binding || !program_call || !carrier ||
        carrier->op != XI_GET_SHARED || !carrier->block ||
        carrier->block->func != binding->caller.xi_function ||
        carrier->aux_int < 0)
        return false;
    const XiImportRef *ref =
        xi_value_import_ref(binding->caller.xi_function, carrier);
    XiModule *source_module =
        module_for_partition(ctx, binding->caller.target_partition);
    XiModule *callee_module =
        module_for_partition(ctx, binding->callee.target_partition);
    const XrProgramSemanticDependencyRecord *dependency =
        ref && source_module && source_module->program_semantic_closure
            ? xr_program_semantic_closure_dependency(
                  source_module->program_semantic_closure,
                  ref->psc_dependency_index)
            : NULL;
    uint32_t callee_module_index = UINT32_MAX;
    for (uint32_t i = 0; callee_module && i < ctx->module_count; i++)
        if (ctx->modules[i] == callee_module)
            callee_module_index = i;
    if (!ref || !dependency || !source_module || !callee_module ||
        carrier->aux_int >= source_module->nslots ||
        !source_module->slot_imports ||
        source_module->slot_imports[carrier->aux_int] != ref ||
        !xr_stable_id_equal(dependency->source_module,
                            ctx->partitions[binding->caller.target_partition]
                                .module_identity) ||
        !xr_stable_id_equal(dependency->dependency_module,
                            ctx->partitions[binding->callee.target_partition]
                                .module_identity) ||
        !xr_stable_id_equal(dependency->exported_function,
                            ctx->graph->producer_function_identity) ||
        !xr_stable_id_equal(dependency->resolver_binding,
                            ctx->graph->resolver_binding) ||
        !xr_stable_id_equal(ref->psc_resolver_binding,
                            program_call->resolver_binding) ||
        !xr_stable_id_equal(ref->psc_resolver_binding,
                            dependency->resolver_binding) ||
        !locator_is_exact(ref->psc_import_locator,
                          dependency->import_locator) ||
        !ref->resolution_attempted || callee_module_index == UINT32_MAX ||
        ref->resolved_mod_index < 0 ||
        (uint32_t) ref->resolved_mod_index != callee_module_index ||
        ref->resolved_module != callee_module ||
        ref->resolved_func != binding->callee.xi_function ||
        ref->resolved_shared_slot < 0 ||
        ref->resolved_shared_slot >= callee_module->nslots ||
        !callee_module->slot_funcs ||
        callee_module->slot_funcs[ref->resolved_shared_slot] !=
            binding->callee.xi_function ||
        ref->resolved_export_slot < 0 ||
        ref->resolved_export_slot >= callee_module->nexports ||
        !callee_module->exports ||
        callee_module->exports[ref->resolved_export_slot].function !=
            binding->callee.xi_function ||
        callee_module->exports[ref->resolved_export_slot].shared_slot !=
            ref->resolved_shared_slot)
        return false;
    return true;
}

static bool verifier_call_binding_is_exact(
    const XrCProgramEmissionContext *ctx,
    const XrCProgramDirectI64EmissionBinding *binding) {
    uint32_t function_count = 0, call_count = 0, argument_count = 0,
             instruction_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(ctx->target, &function_count);
    const XrTargetCallRecord *calls =
        xr_target_plan_calls(ctx->target, &call_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(ctx->target, &argument_count);
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_instructions(ctx->target, &instruction_count);
    if (!functions || !calls || !arguments || !instructions ||
        ctx->graph->target_call >= call_count ||
        ctx->graph->target_argument >= argument_count ||
        binding->caller.target_function >= function_count ||
        binding->callee.target_function >= function_count)
        return false;
    const XrTargetCallRecord *call = &calls[ctx->graph->target_call];
    const XrTargetCallArgumentRecord *argument =
        &arguments[ctx->graph->target_argument];
    const XrTargetMachineRepRecord *result_register =
        xr_target_plan_machine_rep(ctx->target, call->result_register_rep);
    const XrTargetMachineRepRecord *result_memory =
        xr_target_plan_machine_rep(ctx->target, call->result_memory_rep);
    const XrTargetMachineRepRecord *argument_register =
        xr_target_plan_machine_rep(ctx->target, argument->register_rep);
    const XrTargetMachineRepRecord *argument_memory =
        xr_target_plan_machine_rep(ctx->target, argument->memory_rep);
    const XrTargetMachineRepRecord *callee_register =
        xr_target_plan_machine_rep(ctx->target, argument->callee_register_rep);
    const XrTargetMachineRepRecord *callee_memory =
        xr_target_plan_machine_rep(ctx->target, argument->callee_memory_rep);
    if (call->id != ctx->graph->target_call ||
        call->caller_function != ctx->graph->entry_target_function ||
        call->callee_function != ctx->graph->producer_target_function ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_PROGRAM_DIRECT ||
        call->target_kind != XR_TARGET_CALL_TARGET_PROGRAM_DIRECT ||
        call->argument_begin != ctx->graph->target_argument ||
        call->argument_count != 1u ||
        argument->call != ctx->graph->target_call ||
        argument->ordinal != ctx->graph->argument_ordinal ||
        argument->caller_slot != ctx->graph->caller_slot ||
        argument->callee_slot != ctx->graph->callee_slot ||
        !result_register || result_register->kind != XR_MACHINE_REP_I64 ||
        !result_memory || result_memory->kind != XR_MACHINE_REP_I64 ||
        !argument_register || argument_register->kind != XR_MACHINE_REP_I64 ||
        !argument_memory || argument_memory->kind != XR_MACHINE_REP_I64 ||
        !callee_register || callee_register->kind != XR_MACHINE_REP_I64 ||
        !callee_memory || callee_memory->kind != XR_MACHINE_REP_I64)
        return false;
    const XrSemanticPlan *entry = xr_target_plan_semantic_module(
        ctx->target, ctx->graph->entry_partition);
    const XrSemanticProgramCallBinding *program_call =
        entry ? xr_semantic_plan_program_call_for_operation(
                    entry, call->semantic_operation)
              : NULL;
    const XiValue *xi_call =
        program_call ? xi_program_semantic_call_for_row(
                           binding->caller.xi_function,
                           program_call->program_row)
                     : NULL;
    if (!program_call || !xi_call || xi_call->op != XI_CALL ||
        xi_call->nargs != 2u || !xi_call->args || !xi_call->args[0] ||
        !xi_call->args[1] || xi_call->block == NULL ||
        xi_call->block->func != binding->caller.xi_function ||
        !xr_stable_id_equal(program_call->program_call,
                            ctx->graph->call_identity) ||
        !xr_stable_id_equal(program_call->callsite,
                            ctx->graph->callsite_identity) ||
        !xr_stable_id_equal(program_call->caller_program_function,
                            ctx->graph->entry_function_identity) ||
        !xr_stable_id_equal(program_call->callee_program_function,
                            ctx->graph->producer_function_identity) ||
        !xr_stable_id_equal(program_call->resolver_binding,
                            ctx->graph->resolver_binding) ||
        !verifier_callee_carrier_is_exact(ctx, binding, program_call,
                                          xi_call->args[0]) ||
        program_call->operation != ctx->graph->entry_semantic_operation ||
        !verifier_callee_operand_has_one_use(
            binding->caller.xi_function, xi_call, xi_call->args[0]))
        return false;
    const XrTargetInstructionRecord *instruction = NULL;
    uint32_t instruction_index = UINT32_MAX;
    for (uint32_t i = 0; i < instruction_count; i++) {
        if (instructions[i].function != ctx->graph->entry_target_function ||
            instructions[i].opcode != XR_TARGET_INSTRUCTION_CALL_DIRECT_I64 ||
            instructions[i].immediate_bits != ctx->graph->target_call)
            continue;
        if (instruction)
            return false;
        instruction = &instructions[i];
        instruction_index = i;
    }
    uint32_t call_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t call_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t argument_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t argument_value = XR_SEMANTIC_INDEX_NONE;
    if (!instruction || instruction->id != instruction_index ||
        instruction->result_slot != call->result_slot ||
        !xr_aot_scalar_program_semantic_value_id(
            ctx->target, binding->caller.target_partition,
            binding->caller.target_function, binding->caller.xi_function,
            xi_call, &call_function, &call_value, NULL, 0) ||
        !xr_aot_scalar_program_semantic_value_id(
            ctx->target, binding->caller.target_partition,
            binding->caller.target_function, binding->caller.xi_function,
            xi_call->args[1], &argument_function, &argument_value, NULL, 0) ||
        call_function != binding->caller.semantic_function ||
        argument_function != binding->caller.semantic_function ||
        call->result_value != call_value ||
        argument->semantic_value != argument_value)
        return false;
    return binding->xi_call == xi_call &&
           binding->xi_argument == xi_call->args[1] &&
           binding->xi_callee_operand == xi_call->args[0] &&
           binding->callee_operand_elided &&
           binding->caller_target_row ==
               &functions[binding->caller.target_function] &&
           binding->callee_target_row ==
               &functions[binding->callee.target_function] &&
           binding->call_row == call && binding->argument_row == argument &&
           binding->instruction_row == instruction &&
           binding->target_call == ctx->graph->target_call &&
           binding->target_instruction == instruction_index &&
           binding->target_argument == ctx->graph->target_argument &&
           binding->semantic_operation == call->semantic_operation &&
           binding->program_call == program_call->program_row;
}

static bool verifier_value_binding_is_exact(
    const XrCProgramEmissionContext *ctx,
    const XrCProgramXiFunctionBinding *function, const XiValue *value,
    const XrCProgramValueEmissionBinding *binding) {
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    if (!binding || !xr_aot_scalar_program_semantic_value_id(
                        ctx->target, function->target_partition,
                        function->target_function, function->xi_function, value,
                        &semantic_function, &semantic_value, NULL, 0) ||
        semantic_function != function->semantic_function)
        return false;
    const XrTargetValueRepRecord *target_value =
        xr_target_plan_value_rep_for_module(
            ctx->target, function->target_partition, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        target_value ? xr_target_plan_machine_rep(
                           ctx->target, target_value->register_rep)
                     : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        target_value ? xr_target_plan_machine_rep(
                           ctx->target, target_value->memory_rep)
                     : NULL;
    if (!target_value || target_value->semantic_value != semantic_value ||
        !register_rep || !memory_rep ||
        register_rep->kind != memory_rep->kind)
        return false;
    XrCValueRep expected_rep = XR_C_VALUE_REP_COUNT;
    const char *expected_type = NULL;
    if (register_rep->kind == XR_MACHINE_REP_VOID) {
        expected_rep = XR_C_VALUE_REP_VOID;
        expected_type = "void";
    } else if (register_rep->kind == XR_MACHINE_REP_I64) {
        expected_rep = XR_C_VALUE_REP_I64;
        expected_type = "int64_t";
    } else if (register_rep->kind == XR_MACHINE_REP_DYN_VALUE) {
        expected_rep = XR_C_VALUE_REP_TAGGED;
        expected_type = "XrValue";
    } else {
        return false;
    }
    XrCProgramValueEmissionBinding expected = {
        .xi_function = function->xi_function,
        .xi_value = value,
        .target_function = function->target_function,
        .target_partition = function->target_partition,
        .semantic_function = function->semantic_function,
        .emission = {
            .semantic_value = semantic_value,
            .target_register_rep = target_value->register_rep,
            .target_memory_rep = target_value->memory_rep,
            .target_register_kind = register_rep->kind,
            .target_memory_kind = memory_rep->kind,
            .register_bits = register_rep->register_bits,
            .memory_align = memory_rep->memory_align,
            .memory_size = memory_rep->memory_size,
            .rep = (uint8_t) expected_rep,
            .materialization = XR_C_VALUE_MATERIALIZATION_NONE,
            .recipe_operand_value = UINT32_MAX,
            .recipe_argument_value = UINT32_MAX,
            .recipe_callee_function = UINT32_MAX,
            .backing_value = UINT32_MAX,
            .c_type = expected_type,
        },
    };
    return value_binding_equal(binding, &expected);
}

static const XrCProgramValueEmissionBinding *verifier_find_value(
    const XrCProgramDirectI64EmissionBinding *binding,
    const XiFunc *function, const XiValue *value) {
    const XrCProgramValueEmissionBinding *match = NULL;
    for (uint32_t i = 0; binding && binding->values &&
                         i < binding->value_count; i++) {
        if (binding->values[i].xi_function != function ||
            binding->values[i].xi_value != value)
            continue;
        if (match)
            return NULL;
        match = &binding->values[i];
    }
    return match;
}

static bool verifier_abi_binding_is_exact(
    const XrCProgramEmissionContext *ctx,
    const XrCProgramDirectI64EmissionBinding *binding,
    const XrCProgramXiFunctionBinding *function, uint16_t ordinal,
    const XrCProgramFunctionAbiEmissionBinding *actual) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_module(
        ctx->target, function->target_partition);
    const XrSemanticFunctionRecord *semantic_function =
        semantic ? xr_semantic_plan_function(semantic,
                                             function->semantic_function)
                 : NULL;
    if (!semantic_function || !actual ||
        ordinal > semantic_function->parameter_count ||
        semantic_function->is_module_initializer ||
        semantic_function->capture_count != 0u ||
        (semantic_function->semantic_effects &
         (XI_EFFECT_MAY_THROW | XI_EFFECT_MAY_SUSPEND)) != 0u)
        return false;
    const XiValue *live_value = NULL;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    if (ordinal == 0u) {
        uint32_t matches = 0;
        for (uint32_t bi = 0; bi < function->xi_function->nblocks; bi++) {
            const XiBlock *block = function->xi_function->blocks[bi];
            if (!block || block->kind != XI_BLOCK_RETURN || !block->control ||
                block->id >= semantic_function->block_count)
                continue;
            const XrSemanticBlockRecord *semantic_block =
                xr_semantic_plan_block(
                    semantic, semantic_function->block_begin + block->id);
            if (!semantic_block ||
                semantic_block->function != function->semantic_function ||
                semantic_block->kind != XI_BLOCK_RETURN ||
                semantic_block->control_value == XR_SEMANTIC_INDEX_NONE ||
                (matches && semantic_value != semantic_block->control_value))
                return false;
            live_value = block->control;
            semantic_value = semantic_block->control_value;
            matches++;
        }
        if (matches != 1u || !live_value)
            return false;
    } else {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(
                semantic, semantic_function->parameter_begin + ordinal - 1u);
        live_value = function->xi_function->params &&
                             ordinal <= function->xi_function->nparams
                         ? function->xi_function->params[ordinal - 1u]
                         : NULL;
        if (!parameter || parameter->function != function->semantic_function ||
            parameter->mode != XR_PARAM_READ || !live_value)
            return false;
        semantic_value = parameter->value;
    }
    const XrCProgramValueEmissionBinding *value = verifier_find_value(
        binding, function->xi_function, live_value);
    if (!value || value->emission.semantic_value != semantic_value ||
        value->emission.rep != XR_C_VALUE_REP_I64 ||
        value->emission.target_register_kind != XR_MACHINE_REP_I64 ||
        value->emission.target_memory_kind != XR_MACHINE_REP_I64)
        return false;
    if (function == &binding->caller) {
        if (ordinal != 0u || binding->call_row->result_value != semantic_value ||
            binding->call_row->result_register_rep !=
                value->emission.target_register_rep ||
            binding->call_row->result_memory_rep !=
                value->emission.target_memory_rep)
            return false;
    } else if (function == &binding->callee) {
        if (ordinal == 0u) {
            if (binding->call_row->result_register_rep !=
                    value->emission.target_register_rep ||
                binding->call_row->result_memory_rep !=
                    value->emission.target_memory_rep)
                return false;
        } else if (ordinal == 1u) {
            if (binding->argument_row->callee_parameter !=
                    semantic_function->parameter_begin ||
                binding->argument_row->callee_register_rep !=
                    value->emission.target_register_rep ||
                binding->argument_row->callee_memory_rep !=
                    value->emission.target_memory_rep)
                return false;
        } else {
            return false;
        }
    } else {
        return false;
    }
    XrCProgramFunctionAbiEmissionBinding expected = {
        .xi_function = function->xi_function,
        .target_function = function->target_function,
        .emission = {
            .semantic_function = function->semantic_function,
            .semantic_value = semantic_value,
            .ordinal = ordinal,
            .parameter_count = semantic_function->parameter_count,
            .target_register_kind = XR_MACHINE_REP_I64,
            .target_memory_kind = XR_MACHINE_REP_I64,
            .slot_class = XR_C_ABI_SLOT_VALUE,
            .boundary_kind = XR_C_ABI_BOUNDARY_NATIVE,
            .rep = XR_C_VALUE_REP_I64,
            .pointee_rep = XR_C_VALUE_REP_VOID,
            .aggregate_class = XR_C_ABI_AGGREGATE_NONE,
            .c_type = "int64_t",
        },
    };
    return abi_binding_equal(actual, &expected);
}

static bool verifier_initializer_abi_binding_is_exact(
    const XrCProgramEmissionContext *ctx,
    const XrCProgramXiFunctionBinding *function,
    const XrCProgramFunctionAbiEmissionBinding *actual) {
    const XrSemanticPlan *semantic =
        ctx && function
            ? xr_target_plan_semantic_module(ctx->target,
                                             function->target_partition)
            : NULL;
    const XrSemanticFunctionRecord *semantic_function =
        semantic ? xr_semantic_plan_function(semantic,
                                             function->semantic_function)
                 : NULL;
    if (!semantic_function || !semantic_function->is_module_initializer ||
        semantic_function->parameter_count != 0u || !actual)
        return false;
    XrCProgramFunctionAbiEmissionBinding expected = {
        .xi_function = function->xi_function,
        .target_function = function->target_function,
        .emission = {
            .semantic_function = function->semantic_function,
            .semantic_value = XR_SEMANTIC_INDEX_NONE,
            .ordinal = 0u,
            .parameter_count = 0u,
            .target_register_kind = XR_MACHINE_REP_VOID,
            .target_memory_kind = XR_MACHINE_REP_VOID,
            .slot_class = XR_C_ABI_SLOT_VALUE,
            .boundary_kind = XR_C_ABI_BOUNDARY_TAGGED,
            .rep = XR_C_VALUE_REP_TAGGED,
            .pointee_rep = XR_C_VALUE_REP_VOID,
            .aggregate_class = XR_C_ABI_AGGREGATE_NONE,
            .c_type = "XrValue",
        },
    };
    return abi_binding_equal(actual, &expected);
}

void xr_c_program_direct_i64_emission_release(
    XrCProgramDirectI64EmissionBinding *binding) {
    if (!binding)
        return;
    xr_free(binding->initializers);
    xr_free(binding->values);
    xr_free(binding->function_abis);
    memset(binding, 0, sizeof(*binding));
}

bool xr_c_program_direct_i64_emission_verify(
    const XrCProgramDirectI64EmissionBinding *binding,
    const XrTargetPlan *target_plan, XiModule *const *modules,
    uint32_t module_count, char *error, size_t error_size) {
    XrCProgramEmissionContext ctx = {0};
    if (!binding || binding->schema_version !=
                        XR_C_PROGRAM_DIRECT_I64_EMISSION_SCHEMA_VERSION ||
        !binding->verified || !prepare_context(target_plan, modules, module_count,
                                               &ctx, error, error_size) ||
        !xr_fingerprint_equal(binding->target_fingerprint,
                              xr_target_plan_fingerprint(target_plan)))
        return emission_fail(error, error_size,
                             "program C-emission binding header is invalid");

    XrCProgramFunctionExpectation caller_expectation = {0};
    XrCProgramFunctionExpectation callee_expectation = {0};
    graph_function_expectations(ctx.graph, &caller_expectation,
                                &callee_expectation);
    if (!verifier_function_binding_is_exact(
            &ctx, &caller_expectation, &binding->caller) ||
        !verifier_function_binding_is_exact(
            &ctx, &callee_expectation, &binding->callee) ||
        binding->caller.xi_function == binding->callee.xi_function ||
        memcmp(binding->caller.c_symbol, binding->callee.c_symbol,
               sizeof(binding->caller.c_symbol)) == 0 ||
        !verifier_call_binding_is_exact(&ctx, binding))
        return emission_fail(error, error_size,
                             "program function/call emission binding changed");

    const XrSemanticPlan *caller_semantic = xr_target_plan_semantic_module(
        ctx.target, binding->caller.target_partition);
    const XrSemanticPlan *callee_semantic = xr_target_plan_semantic_module(
        ctx.target, binding->callee.target_partition);
    const XrSemanticFunctionRecord *caller_function =
        caller_semantic
            ? xr_semantic_plan_function(caller_semantic,
                                        binding->caller.semantic_function)
            : NULL;
    const XrSemanticFunctionRecord *callee_function =
        callee_semantic
            ? xr_semantic_plan_function(callee_semantic,
                                        binding->callee.semantic_function)
            : NULL;
    if (!caller_function || !callee_function || !binding->initializers ||
        binding->initializer_count != ctx.module_count || !binding->values ||
        !binding->function_abis ||
        binding->function_abi_count != 3u + binding->initializer_count)
        return emission_fail(error, error_size,
                             "program C-emission row coverage is incomplete");

    uint32_t value_index = 0;
    const XrCProgramXiFunctionBinding *functions[2] = {
        &binding->caller, &binding->callee,
    };
    for (uint32_t fi = 0; fi < 2u; fi++) {
        const XrSemanticPlan *semantic = xr_target_plan_semantic_module(
            ctx.target, functions[fi]->target_partition);
        const XrSemanticFunctionRecord *function =
            semantic ? xr_semantic_plan_function(
                           semantic, functions[fi]->semantic_function)
                     : NULL;
        for (uint32_t local = 0; function && local < function->value_count;
             local++) {
            const XiValue *xi_value = function_value_for_local_id(
                functions[fi]->xi_function, local);
            if (!xi_value)
                continue;
            if (!binding->values || value_index >= binding->value_count ||
                !verifier_value_binding_is_exact(
                    &ctx, functions[fi], xi_value,
                    &binding->values[value_index]))
                return emission_fail(
                    error, error_size,
                    "program value C-emission row %u (function %u local %u) changed",
                    value_index, functions[fi]->target_function, local);
            value_index++;
        }
    }
    for (uint32_t partition = 0; partition < binding->initializer_count;
         partition++) {
        const XrCProgramXiFunctionBinding *initializer =
            &binding->initializers[partition];
        if (!verifier_initializer_value_owner(&ctx, partition, initializer))
            return emission_fail(error, error_size,
                                 "program initializer TargetPlan owner changed");
        if (memcmp(initializer->c_symbol, binding->caller.c_symbol,
                   sizeof(initializer->c_symbol)) == 0 ||
            memcmp(initializer->c_symbol, binding->callee.c_symbol,
                   sizeof(initializer->c_symbol)) == 0)
            return emission_fail(error, error_size,
                                 "program initializer C symbol changed authority");
        for (uint32_t prior = 0; prior < partition; prior++)
            if (memcmp(initializer->c_symbol,
                       binding->initializers[prior].c_symbol,
                       sizeof(initializer->c_symbol)) == 0)
                return emission_fail(error, error_size,
                                     "program initializer C symbols collide");
        const XrSemanticPlan *semantic = xr_target_plan_semantic_module(
            ctx.target, initializer->target_partition);
        const XrSemanticFunctionRecord *function =
            semantic ? xr_semantic_plan_function(
                           semantic, initializer->semantic_function)
                     : NULL;
        for (uint32_t local = 0; function && local < function->value_count;
             local++) {
            const XiValue *xi_value = function_value_for_local_id(
                initializer->xi_function, local);
            if (!xi_value)
                continue;
            if (value_index >= binding->value_count ||
                !verifier_value_binding_is_exact(
                    &ctx, initializer, xi_value,
                    &binding->values[value_index]))
                return emission_fail(
                    error, error_size,
                    "program initializer C-emission row %u (partition %u local %u) changed",
                    value_index, partition, local);
            value_index++;
        }
    }
    if (value_index != binding->value_count)
        return emission_fail(error, error_size,
                             "program value C-emission coverage is incomplete");

    const XrCProgramXiFunctionBinding *abi_functions[3] = {
        &binding->caller, &binding->callee, &binding->callee,
    };
    const uint16_t abi_ordinals[3] = {0u, 0u, 1u};
    for (uint32_t i = 0; i < 3u; i++) {
        if (!verifier_abi_binding_is_exact(
                &ctx, binding, abi_functions[i], abi_ordinals[i],
                &binding->function_abis[i]))
            return emission_fail(error, error_size,
                                 "program function ABI emission row changed");
    }
    for (uint32_t i = 0; i < binding->initializer_count; i++) {
        if (!verifier_initializer_abi_binding_is_exact(
                &ctx, &binding->initializers[i],
                &binding->function_abis[3u + i]))
            return emission_fail(
                error, error_size,
                "program initializer ABI emission row changed");
    }
    return true;
}

bool xr_c_program_direct_i64_emission_bind(
    const XrTargetPlan *target_plan, XiModule *const *modules,
    uint32_t module_count, XrCProgramDirectI64EmissionBinding *out,
    char *error, size_t error_size) {
    if (!out)
        return emission_fail(error, error_size,
                             "program C-emission output is missing");
    memset(out, 0, sizeof(*out));
    XrCProgramEmissionContext ctx = {0};
    if (!prepare_context(target_plan, modules, module_count, &ctx, error,
                         error_size))
        return false;

    XrCProgramFunctionExpectation caller = {0};
    XrCProgramFunctionExpectation callee = {0};
    graph_function_expectations(ctx.graph, &caller, &callee);
    XrCProgramDirectI64EmissionBinding candidate = {0};
    if (!bind_function(&ctx, &caller, &candidate.caller))
        return emission_fail(error, error_size,
                             "entry function has no exact Xi emission binding");
    if (!bind_function(&ctx, &callee, &candidate.callee))
        return emission_fail(error, error_size,
                             "producer function has no exact Xi emission binding");
    if (candidate.caller.xi_function == candidate.callee.xi_function ||
        strcmp(candidate.caller.c_symbol, candidate.callee.c_symbol) == 0)
        return emission_fail(error, error_size,
                             "program C function identities collide");
    if (!bind_direct_call(&ctx, &candidate))
        return emission_fail(error, error_size,
                             "program call has no exact Xi emission binding");
    if (!build_emission_rows(&ctx, &candidate, error, error_size)) {
        xr_c_program_direct_i64_emission_release(&candidate);
        return false;
    }
    candidate.schema_version =
        XR_C_PROGRAM_DIRECT_I64_EMISSION_SCHEMA_VERSION;
    candidate.target_fingerprint = xr_target_plan_fingerprint(target_plan);
    candidate.verified = true;
    if (!xr_c_program_direct_i64_emission_verify(
            &candidate, target_plan, modules, module_count, error, error_size)) {
        xr_c_program_direct_i64_emission_release(&candidate);
        return false;
    }
    *out = candidate;
    return true;
}

const XrCProgramXiFunctionBinding *
xr_c_program_direct_i64_function_binding(
    const XrCProgramDirectI64EmissionBinding *binding,
    const XiFunc *function) {
    if (!binding || !binding->verified || !function)
        return NULL;
    if (binding->caller.xi_function == function)
        return &binding->caller;
    if (binding->callee.xi_function == function)
        return &binding->callee;
    const XrCProgramXiFunctionBinding *match = NULL;
    for (uint32_t i = 0; binding->initializers &&
                         i < binding->initializer_count; i++) {
        if (binding->initializers[i].xi_function != function)
            continue;
        if (match)
            return NULL;
        match = &binding->initializers[i];
    }
    if (match)
        return match;
    return NULL;
}

bool xr_c_program_direct_i64_value_view(
    const XrCProgramDirectI64EmissionBinding *binding,
    const XiFunc *function, const XiValue *value,
    XrCValueEmissionView *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!binding || !binding->verified || !function || !value || !out)
        return false;
    const XrCProgramValueEmissionBinding *match = NULL;
    for (uint32_t i = 0; i < binding->value_count; i++) {
        const XrCProgramValueEmissionBinding *candidate = &binding->values[i];
        if (candidate->xi_function != function || candidate->xi_value != value)
            continue;
        if (match)
            return false;
        match = candidate;
    }
    if (!match)
        return false;
    *out = match->emission;
    return true;
}

bool xr_c_program_direct_i64_function_abi_view(
    const XrCProgramDirectI64EmissionBinding *binding,
    const XiFunc *function, uint16_t ordinal,
    XrCFunctionAbiEmissionView *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!binding || !binding->verified || !function || !out)
        return false;
    const XrCProgramFunctionAbiEmissionBinding *match = NULL;
    for (uint32_t i = 0; i < binding->function_abi_count; i++) {
        const XrCProgramFunctionAbiEmissionBinding *candidate =
            &binding->function_abis[i];
        if (candidate->xi_function != function ||
            candidate->emission.ordinal != ordinal)
            continue;
        if (match)
            return false;
        match = candidate;
    }
    if (!match)
        return false;
    *out = match->emission;
    return true;
}

bool xr_c_program_direct_i64_call_is_exact(
    const XrCProgramDirectI64EmissionBinding *binding,
    const XiFunc *caller, const XiValue *call) {
    return binding && binding->verified && caller && call &&
           binding->caller.xi_function == caller && binding->xi_call == call &&
           call->op == XI_CALL && call->nargs == 2u && call->args &&
           call->args[1] == binding->xi_argument && binding->caller_target_row &&
           binding->callee_target_row && binding->call_row &&
           binding->argument_row && binding->instruction_row;
}

bool xr_c_program_direct_i64_callee_operand_is_elided(
    const XrCProgramDirectI64EmissionBinding *binding, const XiFunc *caller,
    const XiValue *value) {
    return binding && binding->verified && binding->callee_operand_elided &&
           binding->caller.xi_function == caller &&
           binding->xi_callee_operand == value && binding->xi_call &&
           binding->xi_call->args && binding->xi_call->nargs == 2u &&
           binding->xi_call->args[0] == value;
}
