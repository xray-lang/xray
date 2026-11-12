/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_constructed_frontier_cases.h - Aggregate metadata reaches backend admission
 */
#include "lir/xlir_types.h"

static XrLirModule *constructed_frontier_module(unsigned scenario, XrLirFunction *root) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType fixed = 0;
    OK(xr_lir_fixed_array_type(m, XR_LIR_U8, 11, &fixed));
    if (scenario == 3) {
        XrLirIntrinsic signature = {71, &fixed, 1, fixed, true};
        XrLirRegistry registry = {&signature, 1};
        OK(xr_lir_set_registry(m, &registry));
    }
    if (scenario == 2) {
        XrLirStateModule module = {0};
        XrLirStateSlot slot = {.payload = fixed, .size = 11, .alignment = 1};
        XrLirStateLayout layout = {.modules = &module, .module_count = 1, .slots = &slot,
            .slot_count = 1, .payload_size = 11, .payload_alignment = 1};
        OK(xr_lir_set_state_layout(m, &layout));
    }
    XrLirType parameter = scenario == 2 ? XR_LIR_STATE : fixed;
    bool has_parameter = scenario <= 2;
    XrLirFunctionSpec spec = {.name = "constructed_frontier", .name_length = 20,
        .parameters = has_parameter ? &parameter : NULL, .parameter_count = has_parameter ? 1u : 0u,
        .result = scenario == 1 ? fixed : XR_LIR_UNIT};
    OK(xr_lir_declare_function(m, &spec, root));
    OK(xr_lir_define_function(m, *root));
    if (scenario == 2) OK(xr_lir_set_state_entry(m, *root));
    XrLirBlock block = {0};
    OK(xr_lir_add_block(m, *root, NULL, 0, &block));
    OK(xr_lir_set_entry(m, *root, block));
    XrLirValue input = {0};
    if (has_parameter) OK(xr_lir_function_parameter(m, *root, 0, &input));
    if (scenario == 2) {
        XrLirInstruction reference = {.op = XR_LIR_STATE_REFERENCE, .type = XR_LIR_STATE_REF,
            .operands = &input, .operand_count = 1};
        XrLirValue result = {0};
        OK(xr_lir_emit_value(m, block, &reference, &result));
    }
    XrLirInstruction ret = {.op = XR_LIR_RET, .type = XR_LIR_UNIT,
        .operands = scenario == 1 ? &input : NULL, .operand_count = scenario == 1 ? 1u : 0u};
    OK(xr_lir_set_terminator(m, block, &ret));
    if (scenario < 2) {
        spec = (XrLirFunctionSpec){.name = "plain_root", .name_length = 10, .result = XR_LIR_UNIT};
        OK(xr_lir_declare_function(m, &spec, root));
        OK(xr_lir_define_function(m, *root));
        OK(xr_lir_add_block(m, *root, NULL, 0, &block));
        OK(xr_lir_set_entry(m, *root, block));
        ret = (XrLirInstruction){.op = XR_LIR_RET, .type = XR_LIR_UNIT};
        OK(xr_lir_set_terminator(m, block, &ret));
    }
    OK(xr_lir_module_finish(m, NULL));
    return m;
}

static void constructed_backend_frontiers(void) {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        XrLirFunction root = {0};
        XrLirModule *m = constructed_frontier_module(scenario, &root);
        CHECK(xr_lir_constructed_type_count(m) == 1);
        XrRbcProgram *program = NULL;
        XrRbcCompileDiagnostic bytecode_diagnostic;
        XrRbcCompileStatus compiled = xr_rbc_compile(m, root, &program, &bytecode_diagnostic);
        XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
        XrCompileResources *resources = NULL;
        CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
        XrLirCRequest request = {.module = m, .entry = root, .resources = resources,
            .translation_unit = "constructed-frontier.c"};
        XrLirCOutput output = {0};
        XrLirCDiagnostic native_diagnostic;
        XrLirCStatus emitted = xr_lir_emit_c(&request, &output, &native_diagnostic);
        CHECK(emitted == XR_LIR_C_OK && output.bytes && output.length);
        if (scenario < 4) {
            CHECK(compiled == XR_RBC_COMPILE_UNSUPPORTED && !program);
            CHECK(strstr(bytecode_diagnostic.message, "constructed physical types"));
        } else {
            CHECK(compiled == XR_RBC_COMPILE_OK && program);
            xr_lir_module_destroy(m);
            m = NULL;
            const XrRbcModule *owned = xr_rbc_program_view(program);
            CHECK(owned->aggregate_type_count == 1 && owned->aggregate_types &&
                owned->aggregate_types[0].kind == XR_RBC_FIXED_U8 && owned->aggregate_types[0].count == 11);
            XrRbcExecution *execution = NULL;
            CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
            XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
            uint64_t bits = 91;
            CHECK(outcome && xr_rbc_outcome_status(outcome) == XR_RBC_OK &&
                xr_rbc_outcome_bits(outcome, &bits) && !bits);
            xr_rbc_outcome_free(&outcome);
            xr_rbc_execution_free(&execution);
        }
        xr_rbc_program_destroy(program);
        xr_lir_c_output_free(&output);
        xr_compile_resources_release(resources);
        xr_lir_module_destroy(m);
    }
    puts("constructed native types accepted while bytecode value frontiers remain closed");
}
