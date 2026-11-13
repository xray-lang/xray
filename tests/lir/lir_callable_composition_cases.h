/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "lir_callable_composition_fixture.h"
static void callable_frame_function_capture_rejected(const XrRbcProgram *program) {
    const XrRbcModule *original = xr_rbc_program_view(program);
    XrRbcModule module = *original;
    XrRbcFunction functions[3];
    memcpy(functions, original->functions, sizeof(functions));
    const XrRbcFunction *root = &original->functions[0];
    XrRbcAggregateAccess *accesses = xr_malloc(root->aggregate_access_count * sizeof(*accesses));
    XrRbcArrayLocation *operands = xr_malloc(root->aggregate_operand_count * sizeof(*operands));
    uint32_t *code = xr_malloc(root->code_count * sizeof(*code));
    CHECK(accesses && operands && code);
    memcpy(accesses, root->aggregate_accesses, root->aggregate_access_count * sizeof(*accesses));
    memcpy(operands, root->aggregate_operands, root->aggregate_operand_count * sizeof(*operands));
    memcpy(code, root->code, root->code_count * sizeof(*code));
    uint32_t reference = UINT32_MAX, bits = UINT32_MAX;
    for (uint32_t a = 0; a < root->aggregate_access_count; ++a)
        if (accesses[a].operation == XR_RBC_FUNCTION_OWNED_REFERENCE) reference = a;
    for (size_t pc = 0; pc < root->code_count; ++pc)
        if ((code[pc] & 255) == XR_RBC_LOADI) bits = (code[pc] >> 8) & 255;
    CHECK(reference != UINT32_MAX && bits != UINT32_MAX);
    uint32_t capture = operands[accesses[reference].first].index;
    bool replaced = false;
    for (size_t pc = 0; pc < root->code_count; ++pc)
        if ((code[pc] & 255) == XR_RBC_MOV && ((code[pc] >> 8) & 255) == capture) {
            CHECK(xr_rbc_encode(&code[pc], (XrRbcOperands){.op=XR_RBC_LOADI, .a=bits, .immediate=2}));
            replaced = true;
        }
    CHECK(replaced);
    accesses[reference].operation = XR_RBC_FUNCTION_REFERENCE;
    accesses[reference].destination = 1;
    operands[accesses[reference].first] = (XrRbcArrayLocation){bits, false};
    XrRbcEnvironment environment = {accesses[reference].callee, 0, reference};
    functions[0].environments = &environment;
    functions[0].environment_count = 1;
    functions[0].environment_size = 8;
    functions[0].aggregate_accesses = accesses;
    functions[0].aggregate_operands = operands;
    functions[0].code = code;
    module.functions = functions;
    XrRbcExecution *execution = NULL;
    XrRbcStatus status = xr_rbc_module_prepare(&module, 0, NULL, 0,
        xr_rbc_program_locations(program), &execution);
    if (status != XR_RBC_INVALID) fprintf(stderr, "Frame Fn capture from BITS prepare=%u\n", (unsigned)status);
    xr_rbc_execution_free(&execution);
    CHECK(status == XR_RBC_INVALID);
    xr_free(code); xr_free(operands); xr_free(accesses);
}
static void callable_composition_receiving(void) {
    XrLirModule *module = NULL;
    XrLirFunction root = {0};
    callable_composition_fixture(&module, &root);
    callable_transforms(module, root);
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    callable_frame_function_capture_rejected(program);
    const XrRbcModule *original = xr_rbc_program_view(program);
    CHECK(original->function_count == 3 && original->function_callable_parameter_count == 3);
    uint32_t callee = UINT32_MAX;
    for (uint32_t f = 0; f < 3; ++f)
        if (original->functions[f].capture_count) callee = f;
    CHECK(callee != UINT32_MAX);
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
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
        if (mutation == 6) {
            functions[callee].register_callables = (const uint32_t *)(uintptr_t)1;
            functions[callee].register_callable_count = 0;
        }
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
    puts("owned Fn captures, actual weakening and seven raw contracts survive code producer retirement");
}
