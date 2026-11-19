/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_callable_frontier_cases.h - Valid signatures reach both capability checks
 */
static void inline_array_callable_frontier(void) {
    for (uint32_t scenario = 0; scenario < 3; ++scenario) {
        XrLirModule *module = NULL;
        OK(xr_lir_module_create(NULL, &module));
        XrLirType element = 0, callable = 0;
        XrLirRecordField row[] = {{.type=XR_LIR_STRING}, {.type=XR_LIR_I64}};
        OK(xr_lir_tuple_type(module, row, 2, &element));
        OK(xr_lir_require_inline_array_element(module, element));
        XrLirCallableParameter parameter = {.type=XR_LIR_ARRAY, .array_element=element,
            .mode=scenario == 2 ? XR_PARAM_MOVE : XR_PARAM_READ};
        XrLirCallableType signature = {.parameters=&parameter, .parameter_count=scenario ? 1 : 0,
            .result=scenario ? XR_LIR_UNIT : XR_LIR_ARRAY, .result_element=scenario ? 0 : element,
            .flags=scenario == 2 ? 0 : XR_LIR_CALLABLE_C_ABI | XR_LIR_CALLABLE_NO_SUSPEND};
        CHECK(xr_lir_callable_type(module, &signature, &callable) == XR_LIR_INVALID_ARGUMENT && !callable);
        xr_lir_module_destroy(module);
    }
}

static void callable_backend_frontier(void) {
    inline_array_callable_frontier();
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirCallableParameter parameter = {.type=XR_LIR_I64};
        XrLirCallableType signature = {.parameters=&parameter, .parameter_count=1, .result=XR_LIR_I64};
        XrLirType callable = 0;
        OK(xr_lir_callable_type(m, &signature, &callable));
        if (scenario == 2) OK(xr_lir_cell_type(m, (XrLirCellType){XR_LIR_I64, 0}, &callable));
        if (scenario == 1) {
            XrLirIntrinsic intrinsic = {.id=91, .parameters=&callable, .parameter_count=1,
                .result=callable, .scalar_sync=true};
            XrLirRegistry registry = {&intrinsic, 1};
            OK(xr_lir_set_registry(m, &registry));
        } else {
            XrLirFunctionSpec spec = {.name="unused_callback", .name_length=15,
                .parameters=&callable, .parameter_count=1, .result=XR_LIR_UNIT};
            XrLirFunction f = {0};
            XrLirBlock block = {0};
            OK(xr_lir_declare_function(m, &spec, &f));
            OK(xr_lir_define_function(m, f));
            OK(xr_lir_add_block(m, f, NULL, 0, &block));
            OK(xr_lir_set_entry(m, f, block));
            XrLirInstruction ret = {.op=XR_LIR_RET, .type=XR_LIR_UNIT};
            OK(xr_lir_set_terminator(m, block, &ret));
        }
        XrLirFunction root = {0};
        XrLirBlock entry = {0};
        XrLirFunctionSpec spec = {.name="root", .name_length=4, .result=XR_LIR_UNIT};
        OK(xr_lir_declare_function(m, &spec, &root));
        OK(xr_lir_define_function(m, root));
        OK(xr_lir_add_block(m, root, NULL, 0, &entry));
        OK(xr_lir_set_entry(m, root, entry));
        XrLirInstruction ret = {.op=XR_LIR_RET, .type=XR_LIR_UNIT};
        OK(xr_lir_set_terminator(m, entry, &ret));
        OK(xr_lir_module_finish(m, NULL));
        XrRbcProgram *program = NULL;
        XrRbcCompileDiagnostic diagnostic = {0};
        XrRbcCompileStatus status = xr_rbc_compile(m, root, &program, &diagnostic);
        if (scenario) CHECK(status == XR_RBC_COMPILE_UNSUPPORTED && !program);
        else {
            CHECK(status == XR_RBC_COMPILE_OK && program);
            XrRbcExecution *execution = NULL;
            CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
            XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
            CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK);
            xr_rbc_outcome_free(&outcome);
            xr_rbc_execution_free(&execution);
            xr_rbc_program_destroy(program);
        }
        XrLirCOutput output = {0};
        XrLirCDiagnostic c_diagnostic = {0};
        XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
        XrCompileResources *resources = NULL;
        CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
        XrLirCRequest request = {.module=m, .entry=root, .resources=resources,
            .translation_unit="callable-frontier.c"};
        XrLirCStatus c_status = xr_lir_emit_c(&request, &output, &c_diagnostic);
        CHECK(c_status == XR_LIR_C_OK && output.bytes && output.length);
        xr_lir_c_output_free(&output);
        xr_compile_resources_release(resources);
        xr_lir_module_destroy(m);
    }
}
