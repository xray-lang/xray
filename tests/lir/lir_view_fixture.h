/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_view_fixture.h - Natural readonly windows on an actual local Array binding
 */
#ifndef LIR_VIEW_FIXTURE_H
#define LIR_VIEW_FIXTURE_H
#include "lir/xlir_view.h"
#include "lir/xlir_array.h"
#include "lir/xlir_internal.h"
#define VIEW_TRY(call) do { status = (call); if (status != XR_LIR_OK) goto failed; } while (0)
static XrLirStatus view_fixture_call(XrLirModule *m, XrLirBlock block,
    XrLirSliceOperation role, const XrLirValue *args, uint32_t count, XrLirValue *result) {
    uint32_t id = 0;
    XrLirStatus status = xr_lir_slice_operation(m, role, XR_LIR_I64,
        role == XR_LIR_SLICE_OP_NEW ? XR_LIR_ARRAY_CELL : 0, &id);
    if (status != XR_LIR_OK) return status;
    const XrLirIntrinsic *signature = xr_lir_internal_slice_intrinsic(m, id);
    if (!signature) return XR_LIR_INVALID_STATE;
    XrLirType type = signature->result;
    XrLirInstruction call = {.op=XR_LIR_CALL_INTRINSIC,.type=type,.intrinsic=id,.operands=args,.operand_count=count};
    return type == XR_LIR_UNIT ? xr_lir_emit_effect(m, block, &call) : xr_lir_emit_value(m, block, &call, result);
}
static XrLirStatus view_fixture_create(XrLirModule **output) {
    XrLirModule *m = NULL; XrLirStatus status;
    VIEW_TRY(xr_lir_module_create(NULL, &m)); VIEW_TRY(xr_lir_slice_catalog_install(m));
    XrLirFunction function = {0}; XrLirBlock block = {0};
    XrLirFunctionSpec signature = {.name="view",.name_length=4,.result=XR_LIR_I64};
    VIEW_TRY(xr_lir_declare_function(m, &signature, &function)); VIEW_TRY(xr_lir_define_function(m, function));
    VIEW_TRY(xr_lir_add_block(m, function, NULL, 0, &block)); VIEW_TRY(xr_lir_set_entry(m, function, block));
    XrLirValue number = {0}, start = {0}, limit = {0}, open = {0}, array = {0}, cell = {0}, slice = {0}, child = {0}, bound = {0}, answer = {0};
    XrLirInstruction instruction = {.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=42};
    VIEW_TRY(xr_lir_emit_value(m, block, &instruction, &number)); instruction.bits = 0;
    VIEW_TRY(xr_lir_emit_value(m, block, &instruction, &start)); instruction.bits = 1;
    VIEW_TRY(xr_lir_emit_value(m, block, &instruction, &limit)); instruction.type = XR_LIR_I1; instruction.bits = 0;
    VIEW_TRY(xr_lir_emit_value(m, block, &instruction, &open));
    uint32_t constructor = 0; VIEW_TRY(xr_lir_array_operation(m, XR_LIR_I64, XR_LIR_ARRAY_OP_NEW, &constructor));
    instruction = (XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_ARRAY,.intrinsic=constructor,.operands=&number,.operand_count=1};
    VIEW_TRY(xr_lir_emit_value(m, block, &instruction, &array));
    instruction = (XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_ARRAY_CELL,.operands=&array,.operand_count=1};
    VIEW_TRY(xr_lir_emit_value(m, block, &instruction, &cell));
    instruction = (XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,.operands=&array,.operand_count=1};
    VIEW_TRY(xr_lir_emit_effect(m, block, &instruction)); XrLirValue range[] = {cell,start,limit};
    VIEW_TRY(view_fixture_call(m, block, XR_LIR_SLICE_OP_NEW, range, 3, &slice));
    XrLirValue reborrow[] = {slice,start,limit,open};
    VIEW_TRY(view_fixture_call(m, block, XR_LIR_SLICE_OP_REBORROW, reborrow, 4, &child));
    VIEW_TRY(view_fixture_call(m, block, XR_LIR_SLICE_OP_BIND, &child, 1, &bound));
    XrLirValue read[] = {bound,start}; VIEW_TRY(view_fixture_call(m, block, XR_LIR_SLICE_OP_GET, read, 2, &answer));
    VIEW_TRY(view_fixture_call(m, block, XR_LIR_SLICE_OP_END, &bound, 1, NULL));
    VIEW_TRY(view_fixture_call(m, block, XR_LIR_SLICE_OP_END, &child, 1, NULL));
    VIEW_TRY(view_fixture_call(m, block, XR_LIR_SLICE_OP_END, &slice, 1, NULL));
    instruction = (XrLirInstruction){.op=XR_LIR_ARRAY_CELL_DROP,.type=XR_LIR_UNIT,.operands=&cell,.operand_count=1};
    VIEW_TRY(xr_lir_emit_effect(m, block, &instruction));
    instruction = (XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&answer,.operand_count=1};
    VIEW_TRY(xr_lir_set_terminator(m, block, &instruction)); VIEW_TRY(xr_lir_module_finish(m, NULL));
    *output = m; return XR_LIR_OK;
failed:
    xr_lir_module_destroy(m); return status;
}
#undef VIEW_TRY
#endif // LIR_VIEW_FIXTURE_H
