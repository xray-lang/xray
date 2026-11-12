/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_view_cases.h - Owned typed roles and raw bytecode rejection boundaries
 */
#include "lir_view_fixture.h"
#include "vm/register/xrbc_internal.h"
static void view_lir_rejections(XrLirModule *module) {
    XrLirBlockData *block = &module->functions[0].blocks[0];
    CHECK(block->instruction_count >= 3);
    XrLirInstructionView *frame = &block->instructions[0].view;
    XrLirInstructionView *slot = &block->instructions[1].view;
    CHECK(frame->op == XR_LIR_VIEW_FRAME_ENTER && slot->op == XR_LIR_CONST && slot->type == XR_LIR_U32);
    XrLirInstructionView saved = *frame;
    frame->op = XR_LIR_CONST;
    CHECK(xr_lir_verify(module, NULL) == XR_LIR_VERIFY_ERROR);
    *frame = saved;
    uint64_t bits = slot->bits;
    slot->bits = 1;
    CHECK(xr_lir_verify(module, NULL) == XR_LIR_VERIFY_ERROR);
    slot->bits = bits;
    uint32_t intrinsic;
    OK(xr_lir_view_operation(module, XR_LIR_VIEW_OP_NEW, &intrinsic));
    for (uint32_t n = 0; n < module->intrinsic_count; ++n) {
        XrLirIntrinsic *entry = &module->intrinsics[n];
        if (entry->id != intrinsic) continue;
        uint32_t role = entry->view_operation;
        entry->view_operation = 0;
        CHECK(xr_lir_verify(module, NULL) == XR_LIR_VERIFY_ERROR);
        entry->view_operation = role;
    }
    XrLirInstructionView *leave = &block->instructions[block->instruction_count - 2].view;
    CHECK(leave->op == XR_LIR_VIEW_FRAME_LEAVE);
    saved = *leave;
    leave->op = XR_LIR_ARRAY_DROP;
    CHECK(xr_lir_verify(module, NULL) == XR_LIR_VERIFY_ERROR);
    *leave = saved;
    OK(xr_lir_verify(module, NULL));
}
static void view_rbc_rejections(XrRbcProgram *program) {
    const XrRbcModule *owned = xr_rbc_program_view(program);
    CHECK(owned->function_count == 1);
    XrRbcFunction function = owned->functions[0];
    CHECK(function.view_access_count >= 5 && function.view_access_count <= 16);
    XrRbcViewAccess rows[16];
    memcpy(rows, function.view_accesses, function.view_access_count * sizeof(*rows));
    function.view_accesses = rows;
    XrRbcModule module = *owned;
    module.functions = &function;
    XrtLocationOwner *locations = xr_rbc_program_locations(program);
    CHECK(xr_rbc_module_admit(&module, locations) == XR_RBC_OK);
    XrRbcViewAccess saved = rows[0];
    rows[0].operation = XR_RBC_VIEW_NONE;
    CHECK(xr_rbc_module_admit(&module, locations) == XR_RBC_INVALID);
    rows[0] = saved;
    rows[0].result = rows[1].operands[1];
    CHECK(xr_rbc_module_admit(&module, locations) == XR_RBC_INVALID);
    rows[0] = saved;
    rows[1].bits = UINT64_C(1);
    CHECK(xr_rbc_module_admit(&module, locations) == XR_RBC_INVALID);
    rows[1].bits = 0;
    XrRbcArrayLocation operand = rows[1].operands[0];
    rows[1].operands[0] = rows[1].operands[1];
    CHECK(xr_rbc_module_admit(&module, locations) == XR_RBC_INVALID);
    rows[1].operands[0] = operand;
    CHECK(xr_rbc_module_admit(&module, locations) == XR_RBC_OK);
    CHECK(function.code_count >= 3 && function.code_count <= 128);
    uint32_t code[128];
    memcpy(code, function.code, function.code_count * sizeof(*code));
    function.code = code;
    size_t branch = function.code_count - 3, leave = function.code_count - 2;
    CHECK((code[branch] & 255) == XR_RBC_VIEW_ACCESS && (code[leave] & 255) == XR_RBC_VIEW_ACCESS);
    CHECK(rows[code[branch] >> 16].operation == XR_RBC_VIEW_CLEANUP);
    CHECK(rows[code[leave] >> 16].operation == XR_RBC_VIEW_FRAME_LEAVE);
    CHECK(xr_rbc_jump(&code[branch], branch, function.code_count - 1, function.code_count));
    CHECK(xr_rbc_module_admit(&module, locations) == XR_RBC_INVALID);
    CHECK(xr_rbc_jump(&code[branch], branch, leave, function.code_count));
    CHECK(xr_rbc_module_admit(&module, locations) == XR_RBC_OK);
}
static void view_receiving_owners(void) {
    XrLirModule *source = NULL, *copy = NULL;
    OK(view_fixture_create(&source));
    view_lir_rejections(source);
    OK(xr_lir_rebuild(source, &copy));
    XrLirIntrinsic before, after;
    uint32_t id;
    OK(xr_lir_view_operation(source, XR_LIR_VIEW_OP_NEW, &id));
    CHECK(id > XR_LIR_ARRAY_CATALOG_COUNT);
    OK(xr_lir_intrinsic_view(source, id, &before));
    OK(xr_lir_intrinsic_view(copy, id, &after));
    CHECK(before.parameters != after.parameters && before.view_operation == after.view_operation);
    xr_lir_module_destroy(source);
    OK(xr_lir_verify(copy, NULL));
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(copy, (XrLirFunction){copy, 1}, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(copy);
    view_rbc_rejections(program);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    uint64_t result = 0;
    CHECK(xr_rbc_outcome_bits(outcome, &result) && result == 1);
    xr_rbc_execution_free(&execution);
    xr_rbc_outcome_free(&outcome);
    xr_rbc_program_destroy(program);
}
