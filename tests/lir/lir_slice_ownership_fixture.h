/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_slice_ownership_fixture.h - Sealed local loans for verifier failure injection
 */
#include "lir/xlir_view.h"
#define SLICE_OWN_TRY(expression) do { status = (expression); if (status != XR_LIR_OK) goto failed; } while (0)
static XrLirStatus slice_ownership_fixture(XrLirType element, XrLirModule **output) {
    XrLirModule *m = NULL; XrLirStatus status; XrLirType slice = 0, parameter = XR_LIR_ARRAY_CELL;
    SLICE_OWN_TRY(xr_lir_module_create(NULL, &m)); SLICE_OWN_TRY(xr_lir_slice_catalog_install(m));
    SLICE_OWN_TRY(xr_lir_slice_type(m, element, &slice)); XrLirFunction function = {0};
    XrLirFunctionSpec signature = {.name="local_slice",.name_length=11,.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
    SLICE_OWN_TRY(xr_lir_declare_function(m, &signature, &function)); SLICE_OWN_TRY(xr_lir_define_function(m, function));
    XrLirValue input = {0}; SLICE_OWN_TRY(xr_lir_function_parameter(m, function, 0, &input));
    SLICE_OWN_TRY(xr_lir_set_array_shape(m, input, &(XrLirArrayShape){element})); XrLirBlock block = {0};
    SLICE_OWN_TRY(xr_lir_add_block(m, function, NULL, 0, &block)); SLICE_OWN_TRY(xr_lir_set_entry(m, function, block));
    XrLirValue args[3] = {input,{0},{0}}, first = {0}, child = {0}, length = {0};
    XrLirInstruction constant = {.op=XR_LIR_CONST,.type=XR_LIR_I64};
    SLICE_OWN_TRY(xr_lir_emit_value(m, block, &constant, &args[1])); constant.bits = 3;
    SLICE_OWN_TRY(xr_lir_emit_value(m, block, &constant, &args[2])); uint32_t id = 0;
    SLICE_OWN_TRY(xr_lir_slice_operation(m, XR_LIR_SLICE_OP_NEW, element, XR_LIR_ARRAY_CELL, &id));
    XrLirInstruction call = {.op=XR_LIR_CALL_INTRINSIC,.type=slice,.intrinsic=id,.operands=args,.operand_count=3};
    SLICE_OWN_TRY(xr_lir_emit_value(m, block, &call, &first));
    SLICE_OWN_TRY(xr_lir_slice_operation(m, XR_LIR_SLICE_OP_BIND, element, 0, &call.intrinsic));
    call.operands = &first; call.operand_count = 1;
    SLICE_OWN_TRY(xr_lir_emit_value(m, block, &call, &child));
    SLICE_OWN_TRY(xr_lir_slice_operation(m, XR_LIR_SLICE_OP_LEN, element, 0, &call.intrinsic));
    call.operands = &child; call.type = XR_LIR_I64; SLICE_OWN_TRY(xr_lir_emit_value(m, block, &call, &length));
    SLICE_OWN_TRY(xr_lir_slice_operation(m, XR_LIR_SLICE_OP_END, element, 0, &call.intrinsic));
    call.type = XR_LIR_UNIT; call.operands = &first; SLICE_OWN_TRY(xr_lir_emit_effect(m, block, &call));
    call.operands = &child; SLICE_OWN_TRY(xr_lir_emit_effect(m, block, &call));
    XrLirInstruction ret = {.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&length,.operand_count=1};
    SLICE_OWN_TRY(xr_lir_set_terminator(m, block, &ret)); SLICE_OWN_TRY(xr_lir_module_finish(m, NULL));
    *output = m; return XR_LIR_OK;
failed:
    xr_lir_module_destroy(m); return status;
}
#undef SLICE_OWN_TRY
