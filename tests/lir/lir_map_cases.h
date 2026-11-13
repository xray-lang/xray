/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_map_cases.h - Map receiving owners and raw transport rejection
 *
 * KEY CONCEPT:
 *   Transforms and bytecode retain their own typed directories after input
 *   retirement. Raw mutations preserve every original ownership check.
 */
#include "lir_map_fixture.h"
static void map_indexed_cell_holes(XrtLocationOwner *locations) {
    for (unsigned string = 0; string < 2; ++string) {
        uint32_t code[6] = {0};
        XrRbcRegisterKind cells[] = {XR_RBC_MAP_STRING_I64, string ? XR_RBC_STRING : XR_RBC_BITS};
        XrRbcRegisterKind registers[] = {string ? XR_RBC_STRING_CELL : XR_RBC_BITS,
            string ? XR_RBC_STRING : XR_RBC_BITS, XR_RBC_BITS};
        XrRbcLiteral literal = {"owned", 5};
        if (string) {
            CHECK(xr_rbc_encode(&code[0], (XrRbcOperands){.op=XR_RBC_STRING_LITERAL, .a=1}));
            CHECK(xr_rbc_encode(&code[1], (XrRbcOperands){.op=XR_RBC_STRING_CELL_NEW, .a=0, .b=1, .c=1}));
            CHECK(xr_rbc_encode(&code[2], (XrRbcOperands){.op=XR_RBC_STRING_DROP, .a=1}));
            CHECK(xr_rbc_encode(&code[3], (XrRbcOperands){.op=XR_RBC_STRING_CELL_DROP, .a=0}));
            CHECK(xr_rbc_encode(&code[4], (XrRbcOperands){.op=XR_RBC_LOADI, .a=2, .immediate=42}));
        } else {
            CHECK(xr_rbc_encode(&code[0], (XrRbcOperands){.op=XR_RBC_LOADI, .a=0, .immediate=42}));
            CHECK(xr_rbc_encode(&code[1], (XrRbcOperands){.op=XR_RBC_CELL_NEW, .a=0, .immediate=1}));
            CHECK(xr_rbc_encode(&code[2], (XrRbcOperands){.op=XR_RBC_CELL_READ, .a=2, .b=0}));
            CHECK(xr_rbc_encode(&code[3], (XrRbcOperands){.op=XR_RBC_MOV, .a=1, .b=2}));
        }
        CHECK(xr_rbc_encode(&code[string ? 5 : 4], (XrRbcOperands){.op=XR_RBC_RET, .a=2}));
        XrRbcFunction function = {.code=code, .code_count=string ? 6u : 5u, .register_count=3,
            .register_kinds=registers, .register_kind_count=3, .cell_count=2,
            .cell_kinds=cells, .cell_kind_count=2};
        XrRbcModule module = {.functions=&function, .function_count=1, .literals=&literal, .literal_count=1};
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, locations, &execution) == XR_RBC_OK);
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        uint64_t bits = 0;
        if (xr_rbc_outcome_status(outcome) != XR_RBC_OK)
            fprintf(stderr, "Map hole before %s Cell runtime=%u\n", string ? "String" : "scalar", (unsigned)xr_rbc_outcome_status(outcome));
        CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome, &bits) && bits == 42);
        xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
    }
}
static void map_receiving_owners(void) {
    XrLirModule *source = map_fixture(1), *copies[3] = {0};
    OK(xr_lir_rebuild(source, &copies[0]));
    OK(xr_lir_inline(source, &copies[1]));
    OK(xr_lir_sccp(source, &copies[2]));
    xr_lir_module_destroy(source);
    for (unsigned n = 0; n < 4; ++n) {
        XrLirModule *module = n < 3 ? copies[n] : map_fixture(300);
        XrRbcProgram *program = NULL;
        XrRbcCompileDiagnostic diagnostic = {0};
        XrRbcCompileStatus compiled = xr_rbc_compile(module, (XrLirFunction){module, 1}, &program, &diagnostic);
        if (compiled) fprintf(stderr, "Map compile f%u b%u i%u: %s\n", diagnostic.function,
            diagnostic.block, diagnostic.instruction, diagnostic.message);
        CHECK(compiled == XR_RBC_COMPILE_OK);
        if (!n) map_indexed_cell_holes(xr_rbc_program_locations(program));
        xr_lir_module_destroy(module);
        const XrRbcModule *owned = xr_rbc_program_view(program);
        const XrRbcFunction *original = &owned->functions[0];
        CHECK(original->array_access_count && (n < 3 || original->reference_spill_count));
        for (unsigned mutation = 0; mutation < 6; ++mutation) {
            XrRbcModule bad = *owned;
            XrRbcFunction function = *original;
            XrRbcArrayAccess *rows = xr_malloc(original->array_access_count * sizeof(*rows));
            CHECK(rows);
            memcpy(rows, original->array_accesses, original->array_access_count * sizeof(*rows));
            function.array_accesses = rows;
            uint32_t get = UINT32_MAX, borrow = UINT32_MAX;
            for (uint32_t a = 0; a < function.array_access_count; ++a) {
                if (rows[a].operation == XR_RBC_ARRAY_GET) get = a;
                if (rows[a].operation == XR_RBC_ARRAY_BORROW) borrow = a;
            }
            CHECK(get != UINT32_MAX && borrow != UINT32_MAX);
            if (mutation == 0) rows[get].result.index = (uint32_t)function.aggregate_value_count;
            if (mutation == 1) rows[get].result.spill = true;
            if (mutation == 2) rows[get].count = 1;
            if (mutation == 3) rows[borrow].result.spill = true;
            if (mutation == 4) rows[get].operation = XR_RBC_ARRAY_REPEAT;
            XrPodType wrong = owned->aggregate_types[0];
            if (mutation == 5) { wrong.element = XR_POD_U64; bad.aggregate_types = &wrong; }
            bad.functions = &function;
            XrRbcExecution *execution = NULL;
            CHECK(xr_rbc_module_prepare(&bad, 0, NULL, 0, xr_rbc_program_locations(program), &execution) == XR_RBC_INVALID && !execution);
            xr_free(rows);
        }
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        xr_rbc_execution_free(&execution);
        xr_rbc_program_destroy(program);
        uint64_t bits = 0;
        CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome, &bits) && bits == 42);
        xr_rbc_outcome_free(&outcome);
    }
    puts("Map receiving owners, six raw shapes and 300 typed spill owners passed");
}

static void map_state_receiver(void) {
    XrLirModule *source = map_state_fixture();
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(source, (XrLirFunction){source, 1}, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(source);
    const XrRbcModule *original = xr_rbc_program_view(program);
    const XrRbcFunction *function = &original->functions[0];
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
        uint32_t *code = xr_malloc(function->code_count * sizeof(*code));
        CHECK(code);
        memcpy(code, function->code, function->code_count * sizeof(*code));
        bool changed = false;
        for (size_t pc = 0; pc < function->code_count; ++pc) {
            if ((code[pc] & 255) != XR_RBC_ARRAY_ACCESS) continue;
            XrRbcArrayAccess a = function->array_accesses[code[pc] >> 16];
            if (a.operation != (mutation == 1 ? XR_RBC_MAP_STATE_WRITE : XR_RBC_MAP_STATE_READ)) continue;
            XrRbcArrayLocation reference = function->array_operands[a.first];
            CHECK(!reference.spill);
            XrRbcOperands replacement = {.op=mutation == 1 ? XR_RBC_STATE_WRITE : XR_RBC_STATE_READ,
                .a=mutation == 1 ? reference.index : a.result.index,
                .b=mutation == 1 ? function->array_operands[a.first + 1].index : reference.index,
                .c=XRT_MODULE_MAP_STRING_I64};
            if (mutation == 2) replacement.c = XRT_MODULE_I64;
            CHECK(xr_rbc_encode(&code[pc], replacement));
            changed = true; break;
        }
        CHECK(changed);
        XrRbcFunction bad_function = *function;
        bad_function.code = code;
        XrRbcModule bad = *original;
        bad.functions = &bad_function;
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&bad, 0, NULL, 0, xr_rbc_program_locations(program), &execution) == XR_RBC_INVALID && !execution);
        xr_free(code);
    }
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution); xr_rbc_program_destroy(program);
    uint64_t bits = 0;
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome, &bits) && bits == 42);
    xr_rbc_outcome_free(&outcome);
    puts("Map State cold reads/writes reject legacy inline transports and scalar retagging");
}
