/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_map_cases.h - Map receiving homes and scalar Cell indexing
 *
 * KEY CONCEPT:
 *   Transforms and code retain their own common homes after input retirement.
 *   Scalar Cells keep their independent indexed transport.
 */
#include "lir_map_fixture.h"
static void map_cell_declaration_and_scalar_cells(XrtLocationOwner *locations) {
    uint32_t code[5] = {0};
    CHECK(xr_rbc_encode(&code[0], (XrRbcOperands){.op=XR_RBC_LOADI, .a=0, .immediate=42}));
    CHECK(xr_rbc_encode(&code[1], (XrRbcOperands){.op=XR_RBC_CELL_NEW, .a=0, .immediate=1}));
    CHECK(xr_rbc_encode(&code[2], (XrRbcOperands){.op=XR_RBC_CELL_READ, .a=2, .b=0}));
    CHECK(xr_rbc_encode(&code[3], (XrRbcOperands){.op=XR_RBC_MOV, .a=1, .b=2}));
    CHECK(xr_rbc_encode(&code[4], (XrRbcOperands){.op=XR_RBC_RET, .a=2}));
    XrRbcRegisterKind cells[] = {XR_RBC_MAP_STRING_I64, XR_RBC_BITS};
    XrRbcFunction function = {.code=code, .code_count=5, .register_count=3, .cell_count=2,
        .cell_kinds=cells, .cell_kind_count=2};
    XrRbcModule module = {.functions=&function, .function_count=1};
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, locations, &execution) == XR_RBC_INVALID && !execution);
    /* Scalar Cell ordinals require their own initialized prefix. The retired
     * indexed Map declaration above cannot stand in for its first element. */
    uint32_t scalar[7] = {0};
    CHECK(xr_rbc_encode(&scalar[0], (XrRbcOperands){.op=XR_RBC_LOADI, .a=0, .immediate=17}));
    CHECK(xr_rbc_encode(&scalar[1], (XrRbcOperands){.op=XR_RBC_CELL_NEW, .a=0, .immediate=0}));
    CHECK(xr_rbc_encode(&scalar[2], (XrRbcOperands){.op=XR_RBC_LOADI, .a=0, .immediate=42}));
    memcpy(scalar + 3, code + 1, 4 * sizeof(*scalar));
    cells[0] = XR_RBC_BITS;
    function.code = scalar; function.code_count = 7;
    CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, locations, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution); uint64_t bits = 0;
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome, &bits) && bits == 42);
    xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
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
        if (!n) map_cell_declaration_and_scalar_cells(xr_rbc_program_locations(program));
        xr_lir_module_destroy(module);
        const XrRbcModule *owned = xr_rbc_program_view(program);
        const XrRbcValueCode *code = xr_rbc_module_value_code(owned, 0);
        CHECK(code && code->homes->owned_frame_needed);
        if (n == 3) {
            CHECK(code->homes->frame.slot_count >= 300);
            CHECK(!owned->functions[0].reference_spill_count);
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
    puts("Map receiving owners, scalar Cell indexing and 300 stable typed owners passed");
}

static void map_state_receiver(void) {
    XrLirModule *source = map_state_fixture();
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(source, (XrLirFunction){source, 1}, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(source);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution); xr_rbc_program_destroy(program);
    uint64_t bits = 0;
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome, &bits) && bits == 42);
    xr_rbc_outcome_free(&outcome);
    puts("Map State reads and writes use the fixed receiving value plan");
}
