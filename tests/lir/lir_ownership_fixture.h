/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_ownership_fixture.h - Borrowed inputs, explicit copies and owned results
 */
#ifndef LIR_OWNERSHIP_FIXTURE_H
#define LIR_OWNERSHIP_FIXTURE_H
#include "lir/xlir_builder.h"
#include "lir/xlir_string.h"

#define OWN_TRY(call) do { status = (call); if (status != XR_LIR_OK) goto failed; } while (0)
static XrLirStatus ownership_fixture(XrLirType type, bool append, uint32_t copies, XrLirModule **out) {
    XrLirModule *m = NULL;
    XrLirStatus status;
    OWN_TRY(xr_lir_module_create(NULL, &m));
    if (append) OWN_TRY(xr_lir_string_catalog_install(m, false, false, false));
    XrLirType parameters[] = {type, type};
    XrLirFunctionSpec spec = {.name="copies", .name_length=6, .result=type,
        .parameters=parameters, .parameter_count=2};
    XrLirFunction function = {0};
    XrLirBlock block = {0};
    XrLirValue inputs[2] = {{0}}, first = {0}, result = {0};
    OWN_TRY(xr_lir_declare_function(m, &spec, &function));
    OWN_TRY(xr_lir_define_function(m, function));
    OWN_TRY(xr_lir_add_block(m, function, NULL, 0, &block));
    OWN_TRY(xr_lir_set_entry(m, function, block));
    for (uint32_t p = 0; p < 2; ++p)
        OWN_TRY(xr_lir_function_parameter(m, function, p, &inputs[p]));
    XrLirOp retain = type == XR_LIR_STRING ? XR_LIR_STRING_RETAIN : XR_LIR_MAP_RETAIN;
    XrLirInstruction instruction = {.op=retain, .type=type, .operands=inputs, .operand_count=1};
    OWN_TRY(xr_lir_emit_value(m, block, &instruction, &first));
    if (append) {
        uint32_t intrinsic = 0;
        OWN_TRY(xr_lir_string_operation(m, XR_LIR_STRING_OP_APPEND_OWNED, &intrinsic));
        XrLirValue arguments[] = {first, inputs[1]};
        instruction = (XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC, .type=type,
            .intrinsic=intrinsic, .operands=arguments, .operand_count=2};
        OWN_TRY(xr_lir_emit_value(m, block, &instruction, &result));
    } else {
        result = first;
        for (uint32_t i = 1; i < copies; ++i) {
            XrLirValue previous = result;
            instruction = (XrLirInstruction){.op=retain, .type=type,
                .operands=&previous, .operand_count=1};
            OWN_TRY(xr_lir_emit_value(m, block, &instruction, &result));
            instruction = (XrLirInstruction){.op=type == XR_LIR_STRING ? XR_LIR_STRING_DROP : XR_LIR_MAP_DROP,
                .type=XR_LIR_UNIT, .operands=&previous, .operand_count=1};
            OWN_TRY(xr_lir_emit_effect(m, block, &instruction));
        }
    }
    instruction = (XrLirInstruction){.op=XR_LIR_RET, .type=XR_LIR_UNIT,
        .operands=&result, .operand_count=1};
    OWN_TRY(xr_lir_set_terminator(m, block, &instruction));
    OWN_TRY(xr_lir_module_finish(m, NULL));
    *out = m;
    return XR_LIR_OK;
failed:
    xr_lir_module_destroy(m);
    return status;
}
#undef OWN_TRY
#endif // LIR_OWNERSHIP_FIXTURE_H
