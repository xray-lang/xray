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

static void callable_vm_execution(void) {
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
