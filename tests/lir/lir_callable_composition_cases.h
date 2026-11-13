/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "lir_callable_composition_fixture.h"
static void callable_composition_receiving(void) {
    XrLirModule *module = NULL;
    XrLirFunction root = {0};
    callable_composition_fixture(&module, &root);
    callable_transforms(module, root);
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    const XrRbcModule *original = xr_rbc_program_view(program);
    CHECK(original->function_count == 3 && original->function_callable_parameter_count == 3);
    uint32_t callee = UINT32_MAX;
    for (uint32_t f = 0; f < 3; ++f)
        if (original->functions[f].capture_count) callee = f;
    CHECK(callee != UINT32_MAX);
    for (unsigned mutation = 0; mutation < 6; ++mutation) {
        XrRbcModule received = *original;
        XrRbcFunction functions[3];
        XrRbcFunctionCallableParameters contracts[3];
        memcpy(functions, original->functions, sizeof(functions));
        memcpy(contracts, original->function_callable_parameters, sizeof(contracts));
        XrRbcCallableValue capture = functions[callee].captures[0];
        XrRbcFunctionCallableParameter row = contracts[callee].rows[0];
        functions[callee].captures = &capture;
        contracts[callee].rows = &row;
        received.functions = functions;
        received.function_callable_parameters = contracts;
        if (mutation == 0) capture.type = (uint32_t)received.callable_type_count;
        if (mutation == 1) capture.kind = XR_RBC_CALLABLE_SCALAR;
        if (mutation == 2) received.function_callable_parameter_count = 2;
        if (mutation == 3) row.parameter = functions[callee].parameter_count;
        if (mutation == 4) row.contract.mode = XR_PARAM_MOVE;
        if (mutation == 5) row.contract.value.type = (uint32_t)received.callable_type_count;
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&received, 0, NULL, 0,
            xr_rbc_program_locations(program), &execution) == XR_RBC_INVALID && !execution);
    }
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    xr_rbc_program_destroy(program);
    xr_lir_module_destroy(module);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    uint64_t bits = 0;
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome, &bits) && bits == 42);
    xr_rbc_outcome_free(&outcome);
    puts("owned Fn captures, actual weakening and six raw contracts survive code producer retirement");
}
