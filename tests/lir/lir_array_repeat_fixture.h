/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_repeat_fixture.h - Runtime length and borrowed fill through a closed constructor role
 */
#ifndef LIR_ARRAY_REPEAT_FIXTURE_H
#define LIR_ARRAY_REPEAT_FIXTURE_H
#include "lir/xlir_array.h"

#define REPEAT_TRY(expression) do { status = (expression); if (status != XR_LIR_OK) goto failed; } while (0)
static XrLirStatus array_repeat_fixture(XrLirType element, XrLirModule **output) {
    XrLirModule *m = NULL;
    XrLirStatus status = xr_lir_array_module_create(NULL, &m);
    if (status != XR_LIR_OK) return status;
    XrLirType parameters[] = {XR_LIR_I64, element};
    XrLirFunctionSpec spec = {.name = "repeat", .name_length = 6, .parameters = parameters,
        .parameter_count = 2, .result = XR_LIR_ARRAY};
    XrLirFunction function = {0};
    REPEAT_TRY(xr_lir_declare_function(m, &spec, &function));
    XrLirArrayShape shape = {element};
    REPEAT_TRY(xr_lir_set_array_result(m, function, &shape));
    REPEAT_TRY(xr_lir_define_function(m, function));
    XrLirBlock block = {0};
    REPEAT_TRY(xr_lir_add_block(m, function, NULL, 0, &block));
    REPEAT_TRY(xr_lir_set_entry(m, function, block));
    XrLirValue inputs[2] = {{0}}, array = {0};
    for (uint32_t p = 0; p < 2; ++p)
        REPEAT_TRY(xr_lir_function_parameter(m, function, p, &inputs[p]));
    uint32_t id = 0;
    REPEAT_TRY(xr_lir_array_operation(m, element, XR_LIR_ARRAY_OP_REPEAT, &id));
    XrLirInstruction call = {.op = XR_LIR_CALL_INTRINSIC, .type = XR_LIR_ARRAY,
        .intrinsic = id, .operands = inputs, .operand_count = 2};
    REPEAT_TRY(xr_lir_emit_value(m, block, &call, &array));
    XrLirInstruction ret = {.op = XR_LIR_RET, .type = XR_LIR_UNIT,
        .operands = &array, .operand_count = 1};
    REPEAT_TRY(xr_lir_set_terminator(m, block, &ret));
    REPEAT_TRY(xr_lir_module_finish(m, NULL));
    *output = m;
    return XR_LIR_OK;
failed:
    xr_lir_module_destroy(m);
    return status;
}
#undef REPEAT_TRY
#endif // LIR_ARRAY_REPEAT_FIXTURE_H
