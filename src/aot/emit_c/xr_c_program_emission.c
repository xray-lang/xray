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
#include "../../ir/xi_program_semantic.h"
#include "../../ir/xi_program_semantic_plan.h"
#include "../../plan/semantic/xr_semantic_plan.h"
#include "../../plan/target/xr_target_verify.h"
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

static bool bind_direct_call(const XrCProgramEmissionContext *ctx,
                             XrCProgramDirectI64EmissionBinding *out) {
    uint32_t call_count = 0, argument_count = 0, instruction_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(ctx->target, &call_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(ctx->target, &argument_count);
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_instructions(ctx->target, &instruction_count);
    if (!calls || !arguments || !instructions || ctx->graph->target_call >= call_count ||
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
        program_call->operation != ctx->graph->entry_semantic_operation ||
        !xr_stable_id_equal(program_call->program_call, ctx->graph->call_identity) ||
        !xr_stable_id_equal(program_call->callsite, ctx->graph->callsite_identity) ||
        !xr_stable_id_equal(program_call->caller_program_function,
                            ctx->graph->entry_function_identity) ||
        !xr_stable_id_equal(program_call->callee_program_function,
                            ctx->graph->producer_function_identity) ||
        !xr_stable_id_equal(program_call->resolver_binding,
                            ctx->graph->resolver_binding))
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
    out->xi_call = xi_call;
    out->target_call = ctx->graph->target_call;
    out->target_instruction = instruction_row;
    out->target_argument = ctx->graph->target_argument;
    out->semantic_operation = call->semantic_operation;
    out->program_call = program_call->program_row;
    return true;
}

bool xr_c_program_direct_i64_emission_bind(
    const XrTargetPlan *target_plan, XiModule *const *modules, uint32_t module_count,
    XrCProgramDirectI64EmissionBinding *out, char *error, size_t error_size) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (error && error_size)
        error[0] = '\0';
    if (!target_plan || !modules || !out || !xr_target_plan_is_verified(target_plan) ||
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

    XrCProgramEmissionContext ctx = {
        .target = target_plan,
        .graph = &graphs[0],
        .partitions = partitions,
        .partition_count = partition_count,
        .modules = modules,
        .module_count = module_count,
    };
    XiModule *entry_module = module_for_partition(&ctx, graphs[0].entry_partition);
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

    XrCProgramFunctionExpectation caller = {
        .target_function = graphs[0].entry_target_function,
        .partition = graphs[0].entry_partition,
        .semantic_function = graphs[0].entry_semantic_function,
        .identity = graphs[0].entry_function_identity,
    };
    XrCProgramFunctionExpectation callee = {
        .target_function = graphs[0].producer_target_function,
        .partition = graphs[0].producer_partition,
        .semantic_function = graphs[0].producer_semantic_function,
        .identity = graphs[0].producer_function_identity,
    };
    if (!bind_function(&ctx, &caller, &out->caller) ||
        !bind_function(&ctx, &callee, &out->callee) ||
        out->caller.xi_function == out->callee.xi_function ||
        strcmp(out->caller.c_symbol, out->callee.c_symbol) == 0 ||
        !bind_direct_call(&ctx, out))
        return emission_fail(error, error_size,
                             "global function/call rows have no exact Xi emission binding");
    out->target_fingerprint = xr_target_plan_fingerprint(target_plan);
    return true;
}
