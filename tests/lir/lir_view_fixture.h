/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_view_fixture.h - Canonical Array ownership below typed activation calls
 */
#ifndef LIR_VIEW_FIXTURE_H
#define LIR_VIEW_FIXTURE_H
#include "lir/xlir_view.h"
#include "lir/xlir_array.h"
#include "lir/xlir_internal.h"
#include <stdio.h>

#define VIEW_TRY(call) do { status = (call); if (status != XR_LIR_OK) { \
    if (status != XR_LIR_OUT_OF_MEMORY) fprintf(stderr, "view fixture line %u status %u\n", \
        (unsigned)__LINE__, (unsigned)status); goto failed; } } while (0)
static XrLirStatus view_fixture_call(XrLirModule *m, XrLirBlock block,
    XrLirViewOperation role, const XrLirValue *args, uint32_t count, XrLirValue *result) {
    uint32_t intrinsic = 0;
    XrLirStatus status = xr_lir_view_operation(m, role, &intrinsic);
    if (status != XR_LIR_OK) return status;
    const XrLirIntrinsic *signature = xr_lir_internal_view_intrinsic(m, intrinsic);
    if (!signature) return XR_LIR_INVALID_STATE;
    XrLirType type = signature->result;
    XrLirInstruction instruction = {.op=XR_LIR_CALL_INTRINSIC, .type=type,
        .operands=args, .operand_count=count, .intrinsic=intrinsic};
    return type == XR_LIR_UNIT ? xr_lir_emit_effect(m, block, &instruction) :
        xr_lir_emit_value(m, block, &instruction, result);
}
static XrLirStatus view_fixture_constant(XrLirModule *m, XrLirBlock block,
    XrLirType type, uint64_t bits, XrLirValue *result) {
    XrLirInstruction instruction = {.op=XR_LIR_CONST, .type=type, .bits=bits};
    return xr_lir_emit_value(m, block, &instruction, result);
}
static XrLirStatus view_fixture_create(XrLirModule **output) {
    XrLirModule *m = NULL;
    XrLirStatus status = xr_lir_module_create(NULL, &m);
    if (status != XR_LIR_OK) return status;
    XrLirFunction f = {0};
    XrLirBlock block = {0};
    XrLirValue frame, slot, number, start, end, open, array, cell, slice, length;
    XrLirFunctionSpec spec = {.name="view", .name_length=4, .result=XR_LIR_I64};
    VIEW_TRY(xr_lir_view_catalog_install(m));
    VIEW_TRY(xr_lir_declare_function(m, &spec, &f));
    VIEW_TRY(xr_lir_define_function(m, f));
    VIEW_TRY(xr_lir_add_block(m, f, NULL, 0, &block));
    VIEW_TRY(xr_lir_set_entry(m, f, block));
    XrLirInstruction instruction = {.op=XR_LIR_VIEW_FRAME_ENTER, .type=XR_LIR_VIEW_FRAME,
        .bits=UINT64_C(1) | (UINT64_C(1) << 32)};
    VIEW_TRY(xr_lir_emit_value(m, block, &instruction, &frame));
    VIEW_TRY(view_fixture_constant(m, block, XR_LIR_U32, 0, &slot));
    VIEW_TRY(view_fixture_constant(m, block, XR_LIR_I64, 7, &number));
    VIEW_TRY(view_fixture_constant(m, block, XR_LIR_I64, 0, &start));
    VIEW_TRY(view_fixture_constant(m, block, XR_LIR_I64, 1, &end));
    VIEW_TRY(view_fixture_constant(m, block, XR_LIR_I1, 0, &open));
    uint32_t constructor = 0;
    VIEW_TRY(xr_lir_array_operation(m, XR_LIR_I64, XR_LIR_ARRAY_OP_NEW, &constructor));
    instruction = (XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC, .type=XR_LIR_ARRAY,
        .intrinsic=constructor, .operands=&number, .operand_count=1};
    VIEW_TRY(xr_lir_emit_value(m, block, &instruction, &array));
    XrLirValue cell_args[] = {frame, slot, array};
    VIEW_TRY(view_fixture_call(m, block, XR_LIR_VIEW_OP_CELL_NEW, cell_args, 3, &cell));
    instruction = (XrLirInstruction){.op=XR_LIR_ARRAY_DROP, .type=XR_LIR_UNIT,
        .operands=&array, .operand_count=1};
    VIEW_TRY(xr_lir_emit_effect(m, block, &instruction));
    XrLirValue range_args[] = {frame, slot, cell, start, end, open};
    VIEW_TRY(view_fixture_call(m, block, XR_LIR_VIEW_OP_NEW, range_args, 6, &slice));
    XrLirValue read_args[] = {frame, slice};
    VIEW_TRY(view_fixture_call(m, block, XR_LIR_VIEW_OP_LEN, read_args, 2, &length));
    XrLirValue cleanup_args[] = {frame, slot};
    VIEW_TRY(view_fixture_call(m, block, XR_LIR_VIEW_OP_CLEANUP, cleanup_args, 2, NULL));
    instruction = (XrLirInstruction){.op=XR_LIR_VIEW_FRAME_LEAVE, .type=XR_LIR_UNIT,
        .operands=&frame, .operand_count=1};
    VIEW_TRY(xr_lir_emit_effect(m, block, &instruction));
    instruction = (XrLirInstruction){.op=XR_LIR_RET, .type=XR_LIR_UNIT,
        .operands=&length, .operand_count=1};
    VIEW_TRY(xr_lir_set_terminator(m, block, &instruction));
    VIEW_TRY(xr_lir_module_finish(m, NULL));
    *output = m;
    return XR_LIR_OK;
failed:
    xr_lir_module_destroy(m);
    return status;
}
#undef VIEW_TRY
#endif // LIR_VIEW_FIXTURE_H
