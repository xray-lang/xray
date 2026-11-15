/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_view_cases.h - Owned typed roles and raw bytecode rejection boundaries
 */
#include "lir_view_fixture.h"
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
        uint32_t role = entry->operation;
        entry->operation = 0;
        CHECK(xr_lir_verify(module, NULL) == XR_LIR_VERIFY_ERROR);
        entry->operation = role;
    }
    XrLirInstructionView *leave = &block->instructions[block->instruction_count - 2].view;
    CHECK(leave->op == XR_LIR_VIEW_FRAME_LEAVE);
    saved = *leave;
    leave->op = XR_LIR_ARRAY_DROP;
    CHECK(xr_lir_verify(module, NULL) == XR_LIR_VERIFY_ERROR);
    *leave = saved;
    OK(xr_lir_verify(module, NULL));
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
    CHECK(before.parameters != after.parameters && before.operation == after.operation);
    xr_lir_module_destroy(source);
    OK(xr_lir_verify(copy, NULL));
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(copy, (XrLirFunction){copy, 1}, &program, NULL) == XR_RBC_COMPILE_UNSUPPORTED && !program);
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request = {.module=copy, .entry={copy, 1}, .resources=resources,
        .translation_unit="view-frontier.c"};
    XrLirCOutput output = {0};
    CHECK(xr_lir_emit_c(&request, &output, NULL) == XR_LIR_C_UNSUPPORTED && !output.bytes && !output.length);
    xr_compile_resources_release(resources);
    xr_lir_module_destroy(copy);
}
