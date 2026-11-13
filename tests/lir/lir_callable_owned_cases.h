/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_callable_owned_cases.h - Authenticate the complete canonical code directory
 */
static void callable_owned_receiving(const XrRbcProgram *program) {
    const XrRbcModule *original = xr_rbc_program_view(program);
    CHECK(original->function_count == 3 && original->closure_entry_count == 1 &&
        original->closure_signature_count == 1);
    const XrRbcFunction *root = &original->functions[0];
    XrRbcFunction functions[3];
    XrRbcAggregateAccess *accesses = xr_malloc(root->aggregate_access_count * sizeof(*accesses));
    uint32_t *code = xr_malloc(root->code_count * sizeof(*code));
    CHECK(accesses && code);
    uint32_t reference = UINT32_MAX;
    for (uint32_t n = 0; n < root->aggregate_access_count; ++n)
        if (root->aggregate_accesses[n].operation == XR_RBC_FUNCTION_OWNED_REFERENCE) reference = n;
    CHECK(reference != UINT32_MAX);
    for (unsigned scenario = 0; scenario < 14; ++scenario) {
        XrRbcModule module = *original;
        memcpy(functions, original->functions, sizeof(functions));
        memcpy(accesses, root->aggregate_accesses, root->aggregate_access_count * sizeof(*accesses));
        memcpy(code, root->code, root->code_count * sizeof(*code));
        XrtCallableEntry entry = original->closure_entries[0];
        XrXirType capture = entry.captures[0], signature = original->closure_signatures[0];
        uint32_t target = original->closure_targets[0];
        XrRbcCallableType callable = original->callable_types[0];
        module.functions = functions;
        module.closure_entries = &entry;
        module.closure_targets = &target;
        module.closure_signatures = &signature;
        entry.captures = &capture;
        functions[0].aggregate_accesses = accesses;
        functions[0].code = code;
        if (scenario == 0) module.closure_types = NULL;
        if (scenario == 1) module.closure_entries = NULL;
        if (scenario == 2) target = 3;
        if (scenario == 3) entry.type = XR_XIR_I64;
        if (scenario == 4) ++entry.capture_count;
        if (scenario == 5) capture = XR_XIR_F64;
        if (scenario == 6) signature = XR_XIR_I64;
        if (scenario == 7) { callable.flags ^= 1u; module.callable_types = &callable; }
        if (scenario == 8) module.code_lease = (XrRbcCodeLease){0};
        if (scenario == 9) accesses[reference].destination = 0;
        if (scenario == 10) accesses[reference].operation = XR_RBC_FUNCTION_REFERENCE;
        if (scenario == 11) {
            bool replaced = false;
            for (size_t pc = 0; pc < root->code_count; ++pc)
                if ((code[pc] & 255) == XR_RBC_AGGREGATE_ACCESS && (code[pc] >> 16) == reference) {
                    CHECK(xr_rbc_encode(&code[pc], (XrRbcOperands){.op=XR_RBC_LOADI,
                        .a=accesses[reference].result.index, .immediate=1}));
                    replaced = true;
                }
            CHECK(replaced);
        }
        if (scenario == 12) accesses[reference].first = (uint32_t)root->aggregate_operand_count;
        if (scenario == 13) accesses[reference].callee = 0;
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, xr_rbc_program_locations(program), &execution) == XR_RBC_INVALID);
        CHECK(!execution);
    }
    XrRbcModule narrow = *original;
    memcpy(functions, original->functions, sizeof(functions));
    memcpy(code, root->code, root->code_count * sizeof(*code));
    XrRbcCallableValue parameter = {XR_RBC_CALLABLE_SCALAR, XR_POD_U8};
    XrtCallableEntry entry = original->closure_entries[0];
    XrXirType capture = XR_XIR_U8;
    uint32_t callee = original->closure_targets[0];
    functions[callee].captures = &parameter;
    functions[0].code = code;
    entry.captures = &capture;
    narrow.functions = functions;
    narrow.closure_entries = &entry;
    XrRbcArrayLocation input = root->aggregate_operands[root->aggregate_accesses[reference].first];
    CHECK(!input.spill);
    bool replaced = false;
    for (size_t pc = 0; pc < root->code_count; ++pc)
        if ((code[pc] & 255) == XR_RBC_LOADI && ((code[pc] >> 8) & 255) == input.index) {
            CHECK(xr_rbc_encode(&code[pc], (XrRbcOperands){.op=XR_RBC_LOADI, .a=input.index, .immediate=256}));
            replaced = true;
        }
    CHECK(replaced);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_module_prepare(&narrow, 0, NULL, 0, xr_rbc_program_locations(program), &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_INVALID);
    xr_rbc_execution_free(&execution);
    xr_rbc_outcome_free(&outcome);
    xr_free(accesses);
    xr_free(code);
    puts("canonical Owned callable raw directory rejects 14 corruptions and full-width U8 overflow");
}
