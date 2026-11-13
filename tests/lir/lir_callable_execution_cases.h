/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "lir_callable_bytecode_cases.h"

static void callable_transforms(XrLirModule *module, XrLirFunction root) {
    XrLirModule *rebuilt = NULL, *inlined = NULL, *folded = NULL;
    OK(xr_lir_rebuild(module, &rebuilt));
    OK(xr_lir_inline(rebuilt, &inlined));
    OK(xr_lir_sccp(inlined, &folded));
    xr_lir_module_destroy(rebuilt);
    xr_lir_module_destroy(inlined);
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(folded, (XrLirFunction){folded, root.id}, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(folded);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    xr_rbc_program_destroy(program);
    uint64_t bits = 0;
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome, &bits) && bits == 42);
    xr_rbc_outcome_free(&outcome);
}

static void callable_frame_frontiers(const XrLirModule *module, XrLirFunction root) {
    for (unsigned scenario = 0; scenario < 2; ++scenario) {
        XrLirModule *copy = NULL;
        OK(xr_lir_rebuild(module, &copy));
        XrLirFunctionData *function = &copy->functions[root.id - 1];
        XrLirBlockData *entry = &function->blocks[function->entry - 1];
        XrLirInstructionView *reference = NULL;
        for (uint32_t n = 0; n < entry->instruction_count; ++n)
            if (entry->instructions[n].view.op == XR_LIR_FUNCTION_REF) reference = &entry->instructions[n].view;
        CHECK(reference);
        if (!scenario) reference->bits = XR_LIR_CLOSURE_OWNED;
        else {
            function->result = reference->type;
            XrLirInstructionView *ret = &entry->instructions[entry->instruction_count - 1].view;
            CHECK(ret->op == XR_LIR_RET && ret->operand_count == 1);
            ((uint32_t *)ret->operands)[0] = reference->result;
        }
        OK(xr_lir_verify(copy, NULL));
        XrRbcProgram *program = NULL;
        CHECK(xr_rbc_compile(copy, (XrLirFunction){copy, root.id}, &program, NULL) == XR_RBC_COMPILE_UNSUPPORTED);
        CHECK(!program);
        XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
        XrCompileResources *resources = NULL;
        CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
        XrLirCRequest request = {.module=copy, .entry={copy, root.id}, .resources=resources,
            .translation_unit="frame-frontier.c"};
        XrLirCOutput output = {0};
        CHECK(xr_lir_emit_c(&request, &output, NULL) == XR_LIR_C_UNSUPPORTED);
        CHECK(!output.bytes && !output.length);
        xr_compile_resources_release(resources);
        xr_lir_module_destroy(copy);
    }
}

static void callable_vm_execution(void) {
    XrLirModule *captured = NULL;
    XrLirFunction captured_root = {0};
    callable_fixture_capture(&captured, &captured_root, true);
    callable_transforms(captured, captured_root);
    callable_frame_frontiers(captured, captured_root);
    XrRbcProgram *captured_program = NULL;
    CHECK(xr_rbc_compile(captured, captured_root, &captured_program, NULL) == XR_RBC_COMPILE_OK);
    callable_frame_receiving(captured_program);
    xr_rbc_program_destroy(captured_program);
    xr_lir_module_destroy(captured);
    XrLirModule *module = NULL;
    XrLirFunction root = {0};
    callable_fixture(&module, &root);
    callable_transforms(module, root);
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic = {0};
    XrRbcCompileStatus status = xr_rbc_compile(module, root, &program, &diagnostic);
    if (status != XR_RBC_COMPILE_OK)
        fprintf(stderr, "Callable compile=%u %s\n", (unsigned)status, diagnostic.message);
    CHECK(status == XR_RBC_COMPILE_OK && program);
    callable_raw_receiving(program);
    callable_narrow_argument(program);
    XrLirFunction scalar = {0};
    OK(xr_lir_function_at(module, 0, &scalar));
    XrRbcProgram *direct = NULL;
    CHECK(xr_rbc_compile(module, scalar, &direct, &diagnostic) == XR_RBC_COMPILE_OK && direct);
    xr_lir_module_destroy(module);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    xr_rbc_program_destroy(program);
    uint64_t bits = 0;
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome, &bits) && bits == 42);
    xr_rbc_outcome_free(&outcome);
    uint64_t argument = 41;
    CHECK(xr_rbc_program_prepare(direct, &argument, 1, &execution) == XR_RBC_OK);
    outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    xr_rbc_program_destroy(direct);
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome, &bits) && bits == 42);
    xr_rbc_outcome_free(&outcome);
}
