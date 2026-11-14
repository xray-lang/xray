/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_bytecode_owned_prepare_cases.h - One admission, independent preparations
 */
static XrRbcProgram *owned_prepare_fixture(void) {
    XrLirModule *module = NULL;
    OK(xr_lir_module_create(NULL, &module));
    XrLirType parameter = XR_LIR_I1;
    XrLirFunction function = test_function(module, "prepare", &parameter, 1, XR_LIR_I64);
    XrLirBlock block = test_block(module, function, NULL, 0);
    OK(xr_lir_set_entry(module, function, block));
    XrLirLiteral literal = {0};
    OK(xr_lir_add_literal(module, "own", 3, &literal));
    XrLirValue string = {0}, length = {0};
    XrLirInstruction instruction = {.op=XR_LIR_STRING_LITERAL, .type=XR_LIR_STRING, .bits=literal.id};
    OK(xr_lir_emit_value(module, block, &instruction, &string));
    instruction = (XrLirInstruction){.op=XR_LIR_STRING_LEN, .type=XR_LIR_I64,
        .operands=&string, .operand_count=1};
    OK(xr_lir_emit_value(module, block, &instruction, &length));
    instruction = (XrLirInstruction){.op=XR_LIR_STRING_DROP, .type=XR_LIR_UNIT,
        .operands=&string, .operand_count=1};
    OK(xr_lir_emit_effect(module, block, &instruction));
    test_return(module, block, length);
    OK(xr_lir_module_finish(module, NULL));
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(module, function, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    return program;
}

static XrRbcStatus owned_prepare_observed(const XrRbcProgram *program, const uint64_t *argument,
    XrRbcExecution **output) {
    unsigned before = admission_source_map_passes;
    XrRbcStatus status = xr_rbc_program_prepare(program, argument, 1, output);
#if !defined(XR_XIR_PARANOID) || !XR_XIR_PARANOID
    CHECK(admission_source_map_passes == before);
#else
    CHECK(admission_source_map_passes == before || admission_source_map_passes == before + 1);
    if (status == XR_RBC_OK) CHECK(admission_source_map_passes == before + 1);
#endif
    return status;
}

static void owned_prepare_result(XrRbcExecution **execution) {
    XrRbcOutcome *outcome = xr_rbc_execution_run(*execution);
    uint64_t bits = 0;
    CHECK(outcome && xr_rbc_outcome_status(outcome) == XR_RBC_OK &&
        xr_rbc_outcome_bits(outcome, &bits) && bits == 3);
    xr_rbc_execution_free(execution);
    xr_rbc_outcome_free(&outcome);
}

static void owned_program_admission(void) {
    compile_admission_calls = admission_source_map_passes = 0;
    XrRbcProgram *program = owned_prepare_fixture();
    CHECK(compile_admission_calls == 1 && admission_source_map_passes == 1);
    size_t baseline = live_allocations;
    uint64_t argument = 1;
    XrRbcExecution *execution = NULL;
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        CHECK(owned_prepare_observed(program, &argument, &execution) == XR_RBC_OK);
        owned_prepare_result(&execution);
        CHECK(live_allocations == baseline);
    }
#if !defined(XR_XIR_PARANOID) || !XR_XIR_PARANOID
    CHECK(compile_admission_calls == 1 && admission_source_map_passes == 1);
#endif
    const XrRbcModule *view = xr_rbc_program_view(program);
    CHECK(view->function_count == 1);
    XrRbcModule raw = *view;
    XrRbcFunction function = view->functions[0];
    raw.functions = &function;
    uint32_t *code = xr_malloc(function.code_count * sizeof(*code));
    CHECK(code);
    memcpy(code, function.code, function.code_count * sizeof(*code));
    function.code = code;
    code[0] = UINT32_MAX;
    CHECK(xr_rbc_module_prepare(&raw, 0, &argument, 1,
        xr_rbc_program_locations(program), &execution) == XR_RBC_INVALID && !execution);
    function = view->functions[0];
    function.register_kind_count = 0;
    CHECK(xr_rbc_module_prepare(&raw, 0, &argument, 1,
        xr_rbc_program_locations(program), &execution) == XR_RBC_INVALID && !execution);
    function = view->functions[0];
    unsigned before = admission_source_map_passes;
    CHECK(xr_rbc_module_prepare(&raw, 0, &argument, 1,
        xr_rbc_program_locations(program), &execution) == XR_RBC_OK);
    CHECK(admission_source_map_passes == before + 1);
    owned_prepare_result(&execution);
    xr_free(code);
    CHECK(live_allocations == baseline);
    argument = 2;
    before = admission_source_map_passes;
    CHECK(owned_prepare_observed(program, &argument, &execution) == XR_RBC_INVALID && !execution);
    CHECK(admission_source_map_passes == before && live_allocations == baseline);
    argument = 1;
    attempts = 0;
    CHECK(owned_prepare_observed(program, &argument, &execution) == XR_RBC_OK);
    size_t count = attempts;
    XrRbcExecution *occupied = execution;
    before = admission_source_map_passes;
    CHECK(owned_prepare_observed(program, &argument, &execution) == XR_RBC_INVALID && execution == occupied);
    CHECK(admission_source_map_passes == before);
    owned_prepare_result(&execution);
    CHECK(live_allocations == baseline);
    for (size_t point = 1; point <= count; ++point) {
        attempts = 0; fail_at = point;
        CHECK(owned_prepare_observed(program, &argument, &execution) == XR_RBC_OUT_OF_MEMORY && !execution);
        CHECK(live_allocations == baseline);
        fail_at = SIZE_MAX;
        CHECK(owned_prepare_observed(program, &argument, &execution) == XR_RBC_OK);
        owned_prepare_result(&execution);
        CHECK(live_allocations == baseline);
    }
    xr_rbc_program_destroy(program);
    CHECK(!live_allocations);
#if !defined(XR_XIR_PARANOID) || !XR_XIR_PARANOID
    printf("Owned preparation: one publication admission, no repeated pass; failure sites: %zu\n", count);
#else
    printf("Owned preparation: one publication admission, PARANOID recheck; failure sites: %zu\n", count);
#endif
}
