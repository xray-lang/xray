/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_view_cases.h - Independent natural loan receivers and retired carrier rejection
 */
#include "lir_view_fixture.h"
static void view_lir_rejections(XrLirModule *module) {
    XrLirBlockData *block = &module->functions[0].blocks[0];
    XrLirInstructionView saved = block->instructions[0].view;
    block->instructions[0].view.op = XR_LIR_VIEW_FRAME_ENTER;
    CHECK(xr_lir_verify(module, NULL) == XR_LIR_VERIFY_ERROR);
    block->instructions[0].view = saved;
    uint32_t id = 0; OK(xr_lir_slice_operation(module, XR_LIR_SLICE_OP_NEW, XR_LIR_I64, XR_LIR_ARRAY_CELL, &id));
    for (uint32_t n = 0; n < module->intrinsic_count; ++n) {
        XrLirIntrinsic *entry = &module->intrinsics[n];
        if (entry->id != id) continue;
        uint32_t operation = entry->operation; entry->operation = 0;
        CHECK(xr_lir_verify(module, NULL) == XR_LIR_VERIFY_ERROR); entry->operation = operation;
    }
    for (uint32_t n = 0; n < block->instruction_count; ++n) {
        XrLirInstructionView *v = &block->instructions[n].view;
        if (v->op != XR_LIR_CALL_INTRINSIC || v->intrinsic != id) continue;
        XrLirType type = v->type; v->type = XR_LIR_SLICE_I64;
        CHECK(xr_lir_verify(module, NULL) == XR_LIR_VERIFY_ERROR); v->type = type;
    }
    OK(xr_lir_verify(module, NULL));
}
static void view_receiving_owners(void) {
    XrLirModule *source = NULL, *copy = NULL; OK(view_fixture_create(&source)); view_lir_rejections(source);
    OK(xr_lir_rebuild(source, &copy)); XrLirIntrinsic before, after; uint32_t id = 0;
    OK(xr_lir_slice_operation(source, XR_LIR_SLICE_OP_NEW, XR_LIR_I64, XR_LIR_ARRAY_CELL, &id));
    OK(xr_lir_intrinsic_view(source, id, &before)); OK(xr_lir_intrinsic_view(copy, id, &after));
    CHECK(before.parameters != after.parameters && before.operation == after.operation);
    xr_lir_module_destroy(source); OK(xr_lir_verify(copy, NULL));
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(copy, (XrLirFunction){copy,1}, &program, NULL) == XR_RBC_COMPILE_OK && program);
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY,0,false}; XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request = {.module=copy,.entry={copy,1},.resources=resources,.translation_unit="readonly-slice.c"};
    XrLirCOutput output = {0}; CHECK(xr_lir_emit_c(&request, &output, NULL) == XR_LIR_C_OK && output.bytes && output.length);
    xr_lir_c_output_free(&output); xr_compile_resources_release(resources); xr_lir_module_destroy(copy);
    XrRbcExecution *execution = NULL; CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    xr_rbc_program_destroy(program); XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution); uint64_t answer = 0; CHECK(xr_rbc_outcome_bits(outcome, &answer) && answer == 42);
    xr_rbc_outcome_free(&outcome);
}
