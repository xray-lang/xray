/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_ref_fixture.h - One borrowed signature for frame and module bindings
 */
#ifndef LIR_ARRAY_REF_FIXTURE_H
#define LIR_ARRAY_REF_FIXTURE_H
#include "lir/xlir_array.h"
#include "lir/xlir_state.h"

#define ARRAY_REF_TRY(expression) do { status = (expression); if (status != XR_LIR_OK) goto failed; } while (0)
static XrLirStatus array_ref_fixture(XrLirType element, XrLirModule **output) {
    XrLirModule *m = NULL;
    XrLirStatus status = xr_lir_array_module_create(NULL, &m);
    if (status != XR_LIR_OK) return status;
    XrLirStateModule module = {0};
    XrLirStateSlot slot = {.module=0, .payload=XR_LIR_ARRAY, .mutable=true,
        .size=8, .alignment=8, .array_element=element};
    XrLirStateLayout layout = {.modules=&module, .module_count=1, .slots=&slot,
        .slot_count=1, .payload_size=8, .payload_alignment=8};
    ARRAY_REF_TRY(xr_lir_set_state_layout(m, &layout));
    XrLirType state_type = XR_LIR_STATE, ref_type = XR_LIR_ARRAY_REF;
    XrLirFunction root = {0}, helper = {0};
    XrLirFunctionSpec spec = {.name="root", .name_length=4, .parameters=&state_type,
        .parameter_count=1, .result=XR_LIR_I64};
    ARRAY_REF_TRY(xr_lir_declare_function(m, &spec, &root));
    ARRAY_REF_TRY(xr_lir_set_state_entry(m, root));
    spec = (XrLirFunctionSpec){.name="use_ref", .name_length=7, .parameters=&ref_type,
        .parameter_count=1, .result=XR_LIR_UNIT};
    ARRAY_REF_TRY(xr_lir_declare_function(m, &spec, &helper));
    XrLirValue input = {0}, snapshot = {0};
    XrLirArrayShape shape = {element};
    ARRAY_REF_TRY(xr_lir_function_parameter(m, helper, 0, &input));
    ARRAY_REF_TRY(xr_lir_set_array_shape(m, input, &shape));
    ARRAY_REF_TRY(xr_lir_define_function(m, helper));
    XrLirBlock body = {0};
    ARRAY_REF_TRY(xr_lir_add_block(m, helper, NULL, 0, &body));
    ARRAY_REF_TRY(xr_lir_set_entry(m, helper, body));
    XrLirInstruction op = {.op=XR_LIR_ARRAY_REF_READ, .type=XR_LIR_ARRAY,
        .operands=&input, .operand_count=1};
    ARRAY_REF_TRY(xr_lir_emit_value(m, body, &op, &snapshot));
    XrLirValue values[2] = {input, snapshot};
    op = (XrLirInstruction){.op=XR_LIR_ARRAY_REF_WRITE, .type=XR_LIR_UNIT,
        .operands=values, .operand_count=2};
    ARRAY_REF_TRY(xr_lir_emit_effect(m, body, &op));
    op = (XrLirInstruction){.op=XR_LIR_ARRAY_DROP, .type=XR_LIR_UNIT,
        .operands=&snapshot, .operand_count=1};
    ARRAY_REF_TRY(xr_lir_emit_effect(m, body, &op));
    op = (XrLirInstruction){.op=XR_LIR_RET, .type=XR_LIR_UNIT};
    ARRAY_REF_TRY(xr_lir_set_terminator(m, body, &op));
    ARRAY_REF_TRY(xr_lir_define_function(m, root));
    ARRAY_REF_TRY(xr_lir_add_block(m, root, NULL, 0, &body));
    ARRAY_REF_TRY(xr_lir_set_entry(m, root, body));
    XrLirValue state = {0}, array = {0}, cell = {0}, reference = {0};
    ARRAY_REF_TRY(xr_lir_function_parameter(m, root, 0, &state));
    op = (XrLirInstruction){.op=XR_LIR_MODULE_BEGIN, .type=XR_LIR_UNIT,
        .operands=&state, .operand_count=1};
    ARRAY_REF_TRY(xr_lir_emit_effect(m, body, &op));
    uint32_t intrinsic = 0;
    ARRAY_REF_TRY(xr_lir_array_operation(m, element, XR_LIR_ARRAY_OP_NEW, &intrinsic));
    op = (XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC, .type=XR_LIR_ARRAY, .intrinsic=intrinsic};
    ARRAY_REF_TRY(xr_lir_emit_value(m, body, &op, &array));
    op = (XrLirInstruction){.op=XR_LIR_CELL_NEW, .type=XR_LIR_ARRAY_CELL,
        .operands=&array, .operand_count=1};
    ARRAY_REF_TRY(xr_lir_emit_value(m, body, &op, &cell));
    values[0] = state; values[1] = array;
    op = (XrLirInstruction){.op=XR_LIR_STATE_CONSTRUCT, .type=XR_LIR_STATE_REF,
        .operands=values, .operand_count=2};
    ARRAY_REF_TRY(xr_lir_emit_value(m, body, &op, &reference));
    op = (XrLirInstruction){.op=XR_LIR_STATE_PUBLISH, .type=XR_LIR_UNIT,
        .operands=&reference, .operand_count=1};
    ARRAY_REF_TRY(xr_lir_emit_effect(m, body, &op));
    op = (XrLirInstruction){.op=XR_LIR_MODULE_READY, .type=XR_LIR_UNIT,
        .operands=&state, .operand_count=1};
    ARRAY_REF_TRY(xr_lir_emit_effect(m, body, &op));
    XrLirValue sources[2] = {cell, reference};
    for (uint32_t i = 0; i < 2; ++i) {
        XrLirValue borrowed = {0};
        op = (XrLirInstruction){.op=XR_LIR_ARRAY_BORROW, .type=XR_LIR_ARRAY_REF,
            .operands=&sources[i], .operand_count=1};
        ARRAY_REF_TRY(xr_lir_emit_value(m, body, &op, &borrowed));
        op = (XrLirInstruction){.op=XR_LIR_CALL, .type=XR_LIR_UNIT, .callee=helper,
            .operands=&borrowed, .operand_count=1};
        ARRAY_REF_TRY(xr_lir_emit_effect(m, body, &op));
    }
    op = (XrLirInstruction){.op=XR_LIR_ARRAY_CELL_DROP, .type=XR_LIR_UNIT,
        .operands=&cell, .operand_count=1};
    ARRAY_REF_TRY(xr_lir_emit_effect(m, body, &op));
    op = (XrLirInstruction){.op=XR_LIR_ARRAY_DROP, .type=XR_LIR_UNIT,
        .operands=&array, .operand_count=1};
    ARRAY_REF_TRY(xr_lir_emit_effect(m, body, &op));
    XrLirValue zero = {0};
    op = (XrLirInstruction){.op=XR_LIR_CONST, .type=XR_LIR_I64};
    ARRAY_REF_TRY(xr_lir_emit_value(m, body, &op, &zero));
    op = (XrLirInstruction){.op=XR_LIR_RET, .type=XR_LIR_UNIT,
        .operands=&zero, .operand_count=1};
    ARRAY_REF_TRY(xr_lir_set_terminator(m, body, &op));
    ARRAY_REF_TRY(xr_lir_module_finish(m, NULL));
    *output = m;
    return XR_LIR_OK;
failed:
    xr_lir_module_destroy(m);
    return status;
}
#undef ARRAY_REF_TRY
#endif // LIR_ARRAY_REF_FIXTURE_H
