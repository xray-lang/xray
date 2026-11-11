/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_fixture.h - Real dynamic constructors, calls, cells and selected edges
 */
#ifndef LIR_ARRAY_FIXTURE_H
#define LIR_ARRAY_FIXTURE_H
#include "lir/xlir_array.h"
#include "lir/xlir_builder.h"

#define ARRAY_TRY(expression) do { status = (expression); if (status != XR_LIR_OK) goto failed; } while (0)

static XrLirStatus array_fixture_registry(XrLirType element, XrLirModule **output) {
    XrLirType constructor[] = {element}, length[] = {XR_LIR_ARRAY};
    XrLirType append[] = {XR_LIR_ARRAY_CELL, element};
    XrLirType get[] = {XR_LIR_ARRAY, XR_LIR_I64}, set[] = {XR_LIR_ARRAY_CELL, XR_LIR_I64, element};
    XrLirIntrinsic entries[] = {{11, constructor, 1, XR_LIR_ARRAY, true},
        {12, length, 1, XR_LIR_I64, true}, {13, append, 2, XR_LIR_UNIT, true},
        {14, get, 2, element, true}, {15, set, 3, XR_LIR_UNIT, true}};
    XrLirRegistry registry = {entries, 5};
    XrLirStatus status = xr_lir_module_create(&registry, output);
    if (status != XR_LIR_OK) return status;
    XrLirType constructor_shape[] = {XR_LIR_TYPE_INVALID}, length_shape[] = {element};
    XrLirType append_shape[] = {element, XR_LIR_TYPE_INVALID};
    XrLirType get_shape[] = {element, XR_LIR_TYPE_INVALID};
    XrLirType set_shape[] = {element, XR_LIR_TYPE_INVALID, XR_LIR_TYPE_INVALID};
    XrLirArrayIntrinsic shapes[] = {{11, constructor_shape, 1, element, true, XR_LIR_ARRAY_OP_NEW},
        {12, length_shape, 1, XR_LIR_TYPE_INVALID, false, XR_LIR_ARRAY_OP_LEN},
        {13, append_shape, 2, XR_LIR_TYPE_INVALID, false, XR_LIR_ARRAY_OP_PUSH},
        {14, get_shape, 2, XR_LIR_TYPE_INVALID, false, XR_LIR_ARRAY_OP_GET},
        {15, set_shape, 3, XR_LIR_TYPE_INVALID, false, XR_LIR_ARRAY_OP_SET}};
    for (uint32_t i = 0; i < 5; ++i) {
        status = xr_lir_set_array_intrinsic(*output, &shapes[i]);
        if (status != XR_LIR_OK) return status;
    }
    /* All vectors die here. Finished queries must use the receiving owner. */
    return XR_LIR_OK;
}

static XrLirStatus array_fixture_copy(XrLirModule *m, XrLirFunction function) {
    XrLirStatus status;
    XrLirBlock block;
    XrLirValue parameter, retained;
    ARRAY_TRY(xr_lir_define_function(m, function));
    ARRAY_TRY(xr_lir_function_parameter(m, function, 0, &parameter));
    ARRAY_TRY(xr_lir_add_block(m, function, NULL, 0, &block));
    ARRAY_TRY(xr_lir_set_entry(m, function, block));
    XrLirInstruction instruction = {.op=XR_LIR_ARRAY_RETAIN, .type=XR_LIR_ARRAY,
        .operands=&parameter, .operand_count=1};
    ARRAY_TRY(xr_lir_emit_value(m, block, &instruction, &retained));
    instruction = (XrLirInstruction){.op=XR_LIR_RET, .type=XR_LIR_UNIT,
        .operands=&retained, .operand_count=1};
    ARRAY_TRY(xr_lir_set_terminator(m, block, &instruction));
failed:
    return status;
}

static XrLirStatus array_fixture_append(XrLirModule *m, XrLirFunction function) {
    XrLirStatus status;
    XrLirBlock block;
    XrLirValue operands[2];
    ARRAY_TRY(xr_lir_define_function(m, function));
    ARRAY_TRY(xr_lir_function_parameter(m, function, 0, &operands[0]));
    ARRAY_TRY(xr_lir_function_parameter(m, function, 1, &operands[1]));
    ARRAY_TRY(xr_lir_add_block(m, function, NULL, 0, &block));
    ARRAY_TRY(xr_lir_set_entry(m, function, block));
    XrLirInstruction instruction = {.op=XR_LIR_CALL_INTRINSIC, .type=XR_LIR_UNIT,
        .intrinsic=13, .operands=operands, .operand_count=2};
    ARRAY_TRY(xr_lir_emit_effect(m, block, &instruction));
    instruction = (XrLirInstruction){.op=XR_LIR_RET, .type=XR_LIR_UNIT};
    ARRAY_TRY(xr_lir_set_terminator(m, block, &instruction));
failed:
    return status;
}

static XrLirStatus array_fixture_create(XrLirType element, XrLirModule **output) {
    XrLirStatus status;
    XrLirModule *m = NULL;
    XrLirFunction root, copy, append;
    XrLirValue parameter, copy_parameter, cell_parameter;
    XrLirType array = XR_LIR_ARRAY, append_types[] = {XR_LIR_ARRAY_CELL, element};
    XrLirArrayShape shape = {element};
    ARRAY_TRY(array_fixture_registry(element, &m));
    XrLirFunctionSpec spec = {.name="root", .name_length=4, .result=XR_LIR_I64,
        .parameters=&element, .parameter_count=1};
    ARRAY_TRY(xr_lir_declare_function(m, &spec, &root));
    spec = (XrLirFunctionSpec){.name="copy", .name_length=4, .result=XR_LIR_ARRAY,
        .parameters=&array, .parameter_count=1};
    ARRAY_TRY(xr_lir_declare_function(m, &spec, &copy));
    ARRAY_TRY(xr_lir_set_array_result(m, copy, &shape));
    ARRAY_TRY(xr_lir_function_parameter(m, copy, 0, &copy_parameter));
    ARRAY_TRY(xr_lir_set_array_shape(m, copy_parameter, &shape));
    spec = (XrLirFunctionSpec){.name="append", .name_length=6, .result=XR_LIR_UNIT,
        .parameters=append_types, .parameter_count=2};
    ARRAY_TRY(xr_lir_declare_function(m, &spec, &append));
    ARRAY_TRY(xr_lir_function_parameter(m, append, 0, &cell_parameter));
    ARRAY_TRY(xr_lir_set_array_shape(m, cell_parameter, &shape));
    ARRAY_TRY(array_fixture_copy(m, copy));
    ARRAY_TRY(array_fixture_append(m, append));
    ARRAY_TRY(xr_lir_define_function(m, root));
    ARRAY_TRY(xr_lir_function_parameter(m, root, 0, &parameter));
    XrLirBlock entry, join;
    ARRAY_TRY(xr_lir_add_block(m, root, NULL, 0, &entry));
    ARRAY_TRY(xr_lir_add_block(m, root, &array, 1, &join));
    ARRAY_TRY(xr_lir_set_entry(m, root, entry));
    XrLirValue selected;
    ARRAY_TRY(xr_lir_block_parameter(m, join, 0, &selected));
    ARRAY_TRY(xr_lir_set_array_shape(m, selected, &shape));
    XrLirValue empty, constructed, copied, cell, read, condition, length;
    XrLirInstruction instruction = {.op=XR_LIR_CALL_INTRINSIC, .type=XR_LIR_ARRAY, .intrinsic=11};
    ARRAY_TRY(xr_lir_emit_value(m, entry, &instruction, &empty));
    instruction = (XrLirInstruction){.op=XR_LIR_ARRAY_DROP, .type=XR_LIR_UNIT,
        .operands=&empty, .operand_count=1};
    ARRAY_TRY(xr_lir_emit_effect(m, entry, &instruction));
    XrLirValue values[] = {parameter, parameter};
    instruction = (XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC, .type=XR_LIR_ARRAY,
        .intrinsic=11, .operands=values, .operand_count=2};
    ARRAY_TRY(xr_lir_emit_value(m, entry, &instruction, &constructed));
    instruction = (XrLirInstruction){.op=XR_LIR_CALL, .type=XR_LIR_ARRAY,
        .callee=copy, .operands=&constructed, .operand_count=1};
    ARRAY_TRY(xr_lir_emit_value(m, entry, &instruction, &copied));
    instruction = (XrLirInstruction){.op=XR_LIR_ARRAY_DROP, .type=XR_LIR_UNIT,
        .operands=&constructed, .operand_count=1};
    ARRAY_TRY(xr_lir_emit_effect(m, entry, &instruction));
    instruction = (XrLirInstruction){.op=XR_LIR_CELL_NEW, .type=XR_LIR_ARRAY_CELL,
        .operands=&copied, .operand_count=1};
    ARRAY_TRY(xr_lir_emit_value(m, entry, &instruction, &cell));
    instruction = (XrLirInstruction){.op=XR_LIR_ARRAY_DROP, .type=XR_LIR_UNIT,
        .operands=&copied, .operand_count=1};
    ARRAY_TRY(xr_lir_emit_effect(m, entry, &instruction));
    values[0] = cell; values[1] = parameter;
    instruction = (XrLirInstruction){.op=XR_LIR_CALL, .type=XR_LIR_UNIT,
        .callee=append, .operands=values, .operand_count=2};
    ARRAY_TRY(xr_lir_emit_effect(m, entry, &instruction));
    instruction = (XrLirInstruction){.op=XR_LIR_CELL_READ, .type=XR_LIR_ARRAY,
        .operands=&cell, .operand_count=1};
    ARRAY_TRY(xr_lir_emit_value(m, entry, &instruction, &read));
    instruction = (XrLirInstruction){.op=XR_LIR_CONST, .type=XR_LIR_I1, .bits=1};
    ARRAY_TRY(xr_lir_emit_value(m, entry, &instruction, &condition));
    instruction = (XrLirInstruction){.op=XR_LIR_CONDBR, .type=XR_LIR_UNIT,
        .operands=&condition, .operand_count=1, .edge_count=2,
        .edges={{join, &read, 1}, {join, &read, 1}}};
    ARRAY_TRY(xr_lir_set_terminator(m, entry, &instruction));
    instruction = (XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC, .type=XR_LIR_I64,
        .intrinsic=12, .operands=&selected, .operand_count=1};
    ARRAY_TRY(xr_lir_emit_value(m, join, &instruction, &length));
    instruction = (XrLirInstruction){.op=XR_LIR_ARRAY_DROP, .type=XR_LIR_UNIT,
        .operands=&selected, .operand_count=1};
    ARRAY_TRY(xr_lir_emit_effect(m, join, &instruction));
    instruction = (XrLirInstruction){.op=XR_LIR_ARRAY_CELL_DROP, .type=XR_LIR_UNIT,
        .operands=&cell, .operand_count=1};
    ARRAY_TRY(xr_lir_emit_effect(m, join, &instruction));
    instruction = (XrLirInstruction){.op=XR_LIR_RET, .type=XR_LIR_UNIT,
        .operands=&length, .operand_count=1};
    ARRAY_TRY(xr_lir_set_terminator(m, join, &instruction));
    ARRAY_TRY(xr_lir_module_finish(m, NULL));
    *output = m;
    return XR_LIR_OK;
failed:
    xr_lir_module_destroy(m);
    return status;
}
#undef ARRAY_TRY
#endif // LIR_ARRAY_FIXTURE_H
