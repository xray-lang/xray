/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "vm/register/xrbc_internal.h"
static void callable_scope_raw_receiving(const XrRbcProgram *program) {
    const XrRbcModule *original = xr_rbc_program_view(program);
    CHECK(original->function_count == 3 && original->function_scopes[0].count == 3);
    const XrRbcFunction *root = &original->functions[0];
    XrRbcAggregateAccess *accesses = xr_malloc(root->aggregate_access_count * sizeof(*accesses));
    uint32_t *code = xr_malloc(root->code_count * sizeof(*code));
    CHECK(accesses && code);
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
        XrRbcModule module = *original;
        XrRbcFunction functions[3];
        XrRbcFunctionScopes scopes[3];
        uint32_t parents[3];
        memcpy(functions, original->functions, sizeof(functions));
        memcpy(scopes, original->function_scopes, sizeof(scopes));
        memcpy(parents, scopes[0].parents, sizeof(parents));
        memcpy(accesses, root->aggregate_accesses, root->aggregate_access_count * sizeof(*accesses));
        memcpy(code, root->code, root->code_count * sizeof(*code));
        module.functions = functions;
        module.function_scopes = scopes;
        scopes[0].parents = parents;
        functions[0].aggregate_accesses = accesses;
        functions[0].code = code;
        if (mutation == 0) module.function_scopes = NULL;
        if (mutation == 1) --module.function_scope_count;
        if (mutation == 2) scopes[0].parents = NULL;
        if (mutation == 3) parents[0] = 1;
        if (mutation == 4) parents[1] = 4;
        if (mutation >= 5) {
            bool changed = false;
            for (size_t n = 0; n < root->aggregate_access_count; ++n) {
                if (mutation == 5 && accesses[n].operation == XR_RBC_FUNCTION_BIND) {
                    accesses[n].destination = 3;
                    changed = true;
                    break;
                }
                if (mutation == 6 && accesses[n].operation == XR_RBC_FUNCTION_BIND && accesses[n].window == 1) {
                    accesses[n].window = 0;
                    changed = true;
                    break;
                }
            }
            if (mutation == 7) {
                for (size_t pc = 0; pc < root->code_count; ++pc)
                    if ((code[pc] & 255) == XR_RBC_AGGREGATE_ACCESS &&
                        accesses[code[pc] >> 16].operation == XR_RBC_FUNCTION_SCOPE_LEAVE) {
                        CHECK(xr_rbc_jump(&code[pc], pc, root->code_count - 1, root->code_count));
                        changed = true;
                        break;
                    }
            }
            CHECK(changed);
        }
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, xr_rbc_program_locations(program), &execution) == XR_RBC_INVALID);
        CHECK(!execution);
    }
    xr_free(code);
    xr_free(accesses);
}
static void callable_scope_verification(void) {
    XrLirModule *module = NULL;
    XrLirFunction root = {0};
    callable_factory_fixture_mode(&module, &root, false, true);
    XrLirModule *rebuilt = NULL, *inlined = NULL, *folded = NULL;
    OK(xr_lir_rebuild(module, &rebuilt));
    OK(xr_lir_inline(rebuilt, &inlined));
    OK(xr_lir_sccp(inlined, &folded));
    xr_lir_module_destroy(rebuilt);
    xr_lir_module_destroy(inlined);
    OK(xr_lir_verify(folded, NULL));
    xr_lir_module_destroy(folded);
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic = {0};
    XrRbcCompileStatus compiled = xr_rbc_compile(module, root, &program, &diagnostic);
    if (compiled != XR_RBC_COMPILE_OK)
        fprintf(stderr, "scoped callable compile=%u %s\n", (unsigned)compiled, diagnostic.message);
    CHECK(compiled == XR_RBC_COMPILE_OK && program);
    CHECK(xr_rbc_program_view(program)->function_scopes && xr_rbc_program_view(program)->function_scope_count == 3);
    callable_scope_raw_receiving(program);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    xr_rbc_program_destroy(program);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    uint64_t bits = 0;
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome, &bits) && bits == 137);
    xr_rbc_outcome_free(&outcome);
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
        XrLirModule *copy = NULL;
        OK(xr_lir_rebuild(module, &copy));
        XrLirFunctionData *function = &copy->functions[mutation == 5 ? 1 : root.id - 1];
        XrLirBlockData *block = &function->blocks[function->entry - 1];
        XrLirInstructionView *first_scope = NULL, *temporary = NULL, *bind = NULL, *call = NULL, *invoke = NULL, *reference = NULL;
        for (uint32_t n = 0; n < block->instruction_count; ++n) {
            XrLirInstructionView *v = &block->instructions[n].view;
            if (v->op == XR_LIR_FUNCTION_SCOPE_ENTER) {
                if (!first_scope) first_scope = v;
                else if (!temporary) temporary = v;
            }
            if (v->op == XR_LIR_FUNCTION_BIND && !bind) bind = v;
            if (v->op == XR_LIR_CALL && !call) call = v;
            if (v->op == XR_LIR_CALL_INDIRECT && !invoke) invoke = v;
            if (v->op == XR_LIR_FUNCTION_REF) reference = v;
        }
        CHECK(first_scope && temporary);
        if (mutation == 0) ((uint32_t *)temporary->operands)[0] = temporary->result;
        if (mutation == 1) {
            for (uint32_t n = 0; n < block->instruction_count; ++n) {
                XrLirInstructionView *v = &block->instructions[n].view;
                if (v->op == XR_LIR_FUNCTION_SCOPE_LEAVE && v->operands[0] == temporary->result) {
                    ((uint32_t *)v->operands)[0] = first_scope->result;
                    break;
                }
            }
        }
        if (mutation == 2) {
            temporary->op = XR_LIR_CONST;
            temporary->operands = NULL;
            temporary->operand_count = 0;
        }
        if (mutation == 3) {
            CHECK(call && invoke);
            ((uint32_t *)invoke->operands)[0] = call->result;
        }
        if (mutation == 4) {
            CHECK(bind && invoke);
            ((uint32_t *)invoke->operands)[0] = temporary->result;
        }
        if (mutation == 5) {
            CHECK(reference);
            ((uint32_t *)block->instructions[block->instruction_count - 1].view.operands)[0] = reference->result;
        }
        if (mutation == 6) {
            CHECK(bind);
            for (uint32_t n = 0; n < block->instruction_count; ++n) {
                XrLirInstructionView *v = &block->instructions[n].view;
                if (v->op == XR_LIR_FUNCTION_BIND && v != bind) v->bits = bind->bits;
            }
        }
        CHECK(xr_lir_verify(copy, NULL) == XR_LIR_VERIFY_ERROR);
        xr_lir_module_destroy(copy);
    }
    xr_lir_module_destroy(module);
    puts("function scope capabilities survive transforms and reject seven stale or forged uses");
}
static void callable_owned_factory_cases(void) {
    callable_scope_verification();
    for (unsigned frame = 0; frame < 2; ++frame) {
        XrLirModule *module = NULL;
        XrLirFunction root;
        callable_factory_fixture(&module, &root, frame != 0);
        XrRbcProgram *program = NULL;
        XrRbcCompileStatus status = xr_rbc_compile(module, root, &program, NULL);
        if (frame) {
            CHECK(status == XR_RBC_COMPILE_UNSUPPORTED && !program);
            XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
            XrCompileResources *resources = NULL;
            CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
            XrLirCRequest request = {.module=module, .entry=root, .resources=resources, .translation_unit="frame-return.c"};
            XrLirCOutput output = {0};
            CHECK(xr_lir_emit_c(&request, &output, NULL) == XR_LIR_C_UNSUPPORTED && !output.bytes && !output.length);
            xr_compile_resources_release(resources);
        } else {
            CHECK(status == XR_RBC_COMPILE_OK && program);
            const XrRbcModule *original = xr_rbc_program_view(program);
            uint32_t factory = UINT32_MAX;
            for (uint32_t f = 0; f < original->function_count; ++f)
                if (original->functions[f].result_kind == XR_RBC_FUNCTION) factory = f;
            CHECK(factory != UINT32_MAX && original->callable_result_count == original->function_count);
            uint32_t *results = xr_malloc(original->callable_result_count * sizeof(*results));
            CHECK(results);
            for (unsigned mutation = 0; mutation < 5; ++mutation) {
                XrRbcModule copy = *original;
                memcpy(results, original->callable_results, original->callable_result_count * sizeof(*results));
                copy.callable_results = results;
                if (mutation == 0) copy.callable_results = NULL;
                if (mutation == 1) --copy.callable_result_count;
                if (mutation == 2) results[factory] = 0;
                if (mutation == 3) results[factory] = (uint32_t)original->callable_type_count + 1;
                XrRbcFunction functions[3];
                uint32_t *code = NULL;
                if (mutation == 4) {
                    CHECK(original->function_count == 3);
                    memcpy(functions, original->functions, sizeof(functions));
                    XrRbcFunction *function = &functions[factory];
                    code = xr_malloc(function->code_count * sizeof(*code));
                    CHECK(code);
                    memcpy(code, function->code, function->code_count * sizeof(*code));
                    function->code = code;
                    copy.functions = functions;
                    bool replaced = false;
                    for (size_t pc = 0; pc < function->code_count; ++pc)
                        if (xr_rbc_aggregate_return_word(function->aggregate_accesses, function->aggregate_access_count, code[pc])) {
                            XrRbcAggregateAccess ret = function->aggregate_accesses[code[pc] >> 16];
                            CHECK(ret.operation == XR_RBC_FUNCTION_SCOPE_RETURN && ret.count == 1);
                            CHECK(xr_rbc_encode(&code[pc], (XrRbcOperands){.op=XR_RBC_RET, .a=ret.source}));
                            replaced = true;
                        }
                    CHECK(replaced);
                }
                XrRbcExecution *execution = NULL;
                CHECK(xr_rbc_module_prepare(&copy, 0, NULL, 0, xr_rbc_program_locations(program), &execution) == XR_RBC_INVALID && !execution);
                xr_free(code);
            }
            xr_free(results);
            XrRbcExecution *execution = NULL;
            CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
            xr_rbc_program_destroy(program);
            program = NULL;
            xr_lir_module_destroy(module);
            module = NULL;
            XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
            xr_rbc_execution_free(&execution);
            uint64_t bits = 0;
            CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome, &bits) && bits == 137);
            xr_rbc_outcome_free(&outcome);
        }
        xr_lir_module_destroy(module);
    }
    puts("owned factory results survive Program and callee retirement; Frame results and five raw return corruptions reject");
}
