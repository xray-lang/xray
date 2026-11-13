/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_callable_bytecode_cases.h - Authenticate raw function values and signatures
 */
static void callable_raw_receiving(const XrRbcProgram *program) {
    const XrRbcModule *original = xr_rbc_program_view(program);
    CHECK(original->function_count == 3 && original->callable_type_count == 1);
    const XrRbcFunction *root = &original->functions[0];
    XrRbcFunction functions[3];
    uint32_t kinds[XR_RBC_REGISTER_COUNT_MAX];
    XrRbcAggregateAccess *accesses = xr_malloc(root->aggregate_access_count * sizeof(*accesses));
    uint32_t *code = xr_malloc(root->code_count * sizeof(*code));
    CHECK(accesses && code);
    uint32_t reference = UINT32_MAX;
    for (uint32_t n = 0; n < root->aggregate_access_count; ++n)
        if (root->aggregate_accesses[n].operation == XR_RBC_FUNCTION_REFERENCE) reference = n;
    CHECK(reference != UINT32_MAX);
    uint32_t result = root->aggregate_accesses[reference].result.index;
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        XrRbcModule module = *original;
        memcpy(functions, original->functions, sizeof(functions));
        memcpy(kinds, root->register_callables, root->register_count * sizeof(*kinds));
        memcpy(accesses, root->aggregate_accesses, root->aggregate_access_count * sizeof(*accesses));
        memcpy(code, root->code, root->code_count * sizeof(*code));
        module.functions = functions;
        functions[0].register_callables = kinds;
        functions[0].aggregate_accesses = accesses;
        functions[0].code = code;
        if (scenario == 0) module.callable_types = NULL;
        if (scenario == 1) functions[0].register_callable_count = root->register_count - 1;
        if (scenario == 2) kinds[result] = 0;
        if (scenario == 3) functions[accesses[reference].callee].callable = 0;
        if (scenario == 4) accesses[reference].slot = 2;
        if (scenario == 5) {
            bool replaced = false;
            for (size_t pc = 0; pc < root->code_count; ++pc)
                if ((code[pc] & 255) == XR_RBC_AGGREGATE_ACCESS && (code[pc] >> 16) == reference) {
                    CHECK(xr_rbc_encode(&code[pc], (XrRbcOperands){.op=XR_RBC_LOADI, .a=result, .immediate=1}));
                    replaced = true;
                }
            CHECK(replaced);
        }
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, xr_rbc_program_locations(program), &execution) == XR_RBC_INVALID);
        CHECK(!execution);
    }
    xr_free(code);
    xr_free(accesses);
}

static void callable_narrow_argument(const XrRbcProgram *program) {
    XrRbcModule module = *xr_rbc_program_view(program);
    XrRbcFunction functions[3];
    memcpy(functions, module.functions, sizeof(functions));
    module.functions = functions;
    XrRbcCallableType signature = module.callable_types[0];
    XrRbcCallableParameter parameter = signature.parameters[0];
    parameter.value = (XrRbcCallableValue){XR_RBC_CALLABLE_SCALAR, XR_POD_U8};
    signature.parameters = &parameter;
    module.callable_types = &signature;
    uint32_t *code = xr_malloc(functions[0].code_count * sizeof(*code));
    CHECK(code);
    memcpy(code, functions[0].code, functions[0].code_count * sizeof(*code));
    functions[0].code = code;
    bool replaced = false;
    for (size_t pc = 0; pc < functions[0].code_count; ++pc)
        if ((code[pc] & 255) == XR_RBC_LOADI && (code[pc] >> 16) == 40) {
            CHECK(xr_rbc_encode(&code[pc], (XrRbcOperands){.op=XR_RBC_LOADI, .a=(code[pc] >> 8) & 255, .immediate=256}));
            replaced = true;
        }
    CHECK(replaced);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, xr_rbc_program_locations(program), &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_INVALID);
    xr_rbc_outcome_free(&outcome);
    xr_rbc_execution_free(&execution);
    xr_free(code);
}

static void callable_frame_receiving(const XrRbcProgram *program) {
    const XrRbcModule *original = xr_rbc_program_view(program);
    const XrRbcFunction *root = &original->functions[0];
    CHECK(root->environment_count == 1 && root->environment_size == 8);
    XrRbcEnvironment environment = root->environments[0];
    uint32_t callee = environment.callee, producer = environment.producer;
    bool binding = original->functions[callee].captures[0].kind == XR_RBC_CALLABLE_BINDING;
    XrRbcFunction functions[3];
    XrRbcAggregateAccess *accesses = xr_malloc(root->aggregate_access_count * sizeof(*accesses));
    XrRbcArrayLocation *operands = xr_malloc(root->aggregate_operand_count * sizeof(*operands));
    uint32_t *code = xr_malloc((root->code_count + 1) * sizeof(*code));
    CHECK(accesses && operands && code && callee < 3);
    for (unsigned scenario = 0; scenario < (binding ? 14u : 11u); ++scenario) {
        XrRbcModule module = *original;
        memcpy(functions, original->functions, sizeof(functions));
        memcpy(accesses, root->aggregate_accesses, root->aggregate_access_count * sizeof(*accesses));
        memcpy(operands, root->aggregate_operands, root->aggregate_operand_count * sizeof(*operands));
        memcpy(code, root->code, root->code_count * sizeof(*code));
        environment = root->environments[0];
        XrRbcCallableValue capture = functions[callee].captures[0];
        functions[callee].captures = &capture;
        functions[0].environments = &environment;
        functions[0].aggregate_accesses = accesses;
        functions[0].aggregate_operands = operands;
        module.functions = functions;
        if (scenario == 0) functions[callee].captures = NULL;
        if (scenario == 1) functions[callee].capture_count = functions[callee].parameter_count + 1;
        if (scenario == 2) capture.kind = XR_RBC_CALLABLE_REFERENCE;
        if (scenario == 3) functions[0].environment_size = 16;
        if (scenario == 4) environment.offset = 1;
        if (scenario == 5) environment.producer = (uint32_t)root->aggregate_access_count;
        if (scenario == 6) accesses[producer].destination = 0;
        if (scenario == 7) accesses[producer].count = 0;
        if (scenario == 8) operands[accesses[producer].first] = accesses[producer].result;
        if (scenario >= 9) {
            uint32_t reference = 0;
            size_t reference_pc = 0;
            for (size_t pc = 0; pc < root->code_count; ++pc)
                if ((code[pc] & 255) == XR_RBC_AGGREGATE_ACCESS && (code[pc] >> 16) == producer) {
                    reference = code[pc];
                    reference_pc = pc;
                }
            CHECK(reference);
            if (scenario == 9) {
                code[root->code_count] = code[root->code_count - 1];
                code[root->code_count - 1] = reference;
                functions[0].code_count += 1;
            } else if (scenario == 10) CHECK(xr_rbc_jump(&code[root->code_count - 1], root->code_count - 1,
                reference_pc, root->code_count));
            functions[0].code = code;
            functions[0].source_ids = NULL;
            functions[0].source_id_count = 0;
        }
        if (scenario == 11 || scenario == 12) {
            bool replaced = false;
            for (size_t pc = 0; pc < root->code_count; ++pc)
                if ((code[pc] & 255) == XR_RBC_CELL_NEW) {
                    uint32_t destination = (code[pc] >> 8) & 255;
                    CHECK(xr_rbc_encode(&code[pc], scenario == 11 ?
                        (XrRbcOperands){.op=XR_RBC_MOV, .a=destination, .b=destination} :
                        (XrRbcOperands){.op=XR_RBC_LOADI, .a=destination, .immediate=1}));
                    replaced = true;
                }
            CHECK(replaced);
        }
        if (scenario == 13) capture = (XrRbcCallableValue){XR_RBC_CALLABLE_SCALAR, XR_POD_I64};
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, xr_rbc_program_locations(program), &execution) == XR_RBC_INVALID);
        CHECK(!execution);
    }
    xr_free(code);
    xr_free(operands);
    xr_free(accesses);
}
