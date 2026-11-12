/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_fixed_bytecode_fixture.h - Typed variadic bytes reaching physical homes
 */
#include "lir/xlir_types.h"

static XrLirValue fixed_bytecode_value(XrLirModule *module, XrLirBlock block,
    XrLirOp op, XrLirType type, const XrLirValue *operands, uint32_t count, uint64_t bits) {
    XrLirInstruction instruction = {.op = op, .type = type, .operands = operands,
        .operand_count = count, .bits = bits};
    XrLirValue result = {0};
    OK(xr_lir_emit_value(module, block, &instruction, &result));
    return result;
}

static XrLirModule *fixed_bytecode_fixture(uint32_t count, bool dynamic, bool ignored, XrLirFunction *root) {
    XrLirModule *module = NULL;
    OK(xr_lir_module_create(NULL, &module));
    XrLirType fixed = 0;
    OK(xr_lir_fixed_array_type(module, XR_LIR_U8, count, &fixed));
    XrLirType parameters[] = {XR_LIR_U8, XR_LIR_I64};
    XrLirFunctionSpec function = {.name = "fixed_inputs", .name_length = 12,
        .parameters = parameters, .parameter_count = 2, .result = XR_LIR_I64};
    OK(xr_lir_declare_function(module, &function, root));
    OK(xr_lir_define_function(module, *root));
    XrLirBlock block = {0};
    OK(xr_lir_add_block(module, *root, NULL, 0, &block));
    OK(xr_lir_set_entry(module, *root, block));
    XrLirValue seed = {0}, index = {0};
    OK(xr_lir_function_parameter(module, *root, 0, &seed));
    OK(xr_lir_function_parameter(module, *root, 1, &index));
    XrLirValue *operands = xr_malloc((size_t)count * sizeof(*operands));
    CHECK(operands);
    XrLirValue wide = {0};
    if (dynamic) wide = fixed_bytecode_value(module, block, XR_LIR_INTCAST, XR_LIR_I64, &seed, 1, 0);
    for (uint32_t n = 0; n < count; ++n) {
        operands[n] = seed;
        if (dynamic) {
            XrLirValue offset = fixed_bytecode_value(module, block, XR_LIR_CONST, XR_LIR_I64, NULL, 0, n);
            XrLirValue values[] = {wide, offset};
            XrLirValue sum = fixed_bytecode_value(module, block, XR_LIR_ADD, XR_LIR_I64, values, 2, 0);
            operands[n] = fixed_bytecode_value(module, block, XR_LIR_INTCAST, XR_LIR_U8, &sum, 1, 0);
        }
    }
    XrLirValue aggregate = fixed_bytecode_value(module, block, XR_LIR_FIXED_ARRAY_NEW, fixed, operands, count, 0);
    xr_free(operands);
    XrLirValue arguments[] = {aggregate, index}, byte = {0};
    char path[] = "fixed-bytecode.xr";
    XrLirInstruction get = {.op = XR_LIR_FIXED_ARRAY_GET, .type = XR_LIR_U8,
        .operands = arguments, .operand_count = 2, .source = {path, sizeof(path) - 1, 37, 4, 37, 12}};
    OK(xr_lir_emit_value(module, block, &get, &byte));
    memset(path, 'x', sizeof(path) - 1);
    XrLirValue result = ignored ? fixed_bytecode_value(module, block, XR_LIR_CONST, XR_LIR_I64, NULL, 0, count) :
        fixed_bytecode_value(module, block, XR_LIR_INTCAST, XR_LIR_I64, &byte, 1, 0);
    XrLirInstruction ret = {.op = XR_LIR_RET, .type = XR_LIR_UNIT, .operands = &result, .operand_count = 1};
    OK(xr_lir_set_terminator(module, block, &ret));
    OK(xr_lir_module_finish(module, NULL));
    return module;
}

static XrLirModule *fixed_bytecode_loop_fixture(uint32_t count, XrLirFunction *root) {
    XrLirModule *module = NULL;
    OK(xr_lir_module_create(NULL, &module));
    XrLirType fixed = 0, input = XR_LIR_I64;
    OK(xr_lir_fixed_array_type(module, XR_LIR_U8, count, &fixed));
    XrLirFunctionSpec function = {.name = "fixed_loop", .name_length = 10,
        .parameters = &input, .parameter_count = 1, .result = XR_LIR_I64};
    OK(xr_lir_declare_function(module, &function, root));
    OK(xr_lir_define_function(module, *root));
    XrLirType types[] = {fixed, fixed, XR_LIR_I64};
    XrLirBlock entry = {0}, loop = {0}, done = {0};
    OK(xr_lir_add_block(module, *root, NULL, 0, &entry));
    OK(xr_lir_add_block(module, *root, types, 3, &loop));
    OK(xr_lir_add_block(module, *root, types, 3, &done));
    OK(xr_lir_set_entry(module, *root, entry));
    XrLirValue limit = {0}, current[3], result[3];
    OK(xr_lir_function_parameter(module, *root, 0, &limit));
    for (uint32_t p = 0; p < 3; ++p) {
        OK(xr_lir_block_parameter(module, loop, p, &current[p]));
        OK(xr_lir_block_parameter(module, done, p, &result[p]));
    }
    XrLirValue *operands = xr_malloc((size_t)count * sizeof(*operands));
    CHECK(operands);
    XrLirValue initial[3];
    for (uint32_t p = 0; p < 2; ++p) {
        XrLirValue byte = fixed_bytecode_value(module, entry, XR_LIR_CONST, XR_LIR_U8, NULL, 0, p ? 11 : 7);
        for (uint32_t n = 0; n < count; ++n) operands[n] = byte;
        initial[p] = fixed_bytecode_value(module, entry, XR_LIR_FIXED_ARRAY_NEW, fixed, operands, count, 0);
    }
    xr_free(operands);
    initial[2] = fixed_bytecode_value(module, entry, XR_LIR_CONST, XR_LIR_I64, NULL, 0, 0);
    XrLirInstruction enter = {.op = XR_LIR_BR, .type = XR_LIR_UNIT,
        .edges = {{loop, initial, 3}}, .edge_count = 1};
    OK(xr_lir_set_terminator(module, entry, &enter));
    XrLirValue condition = {0}, compare[] = {current[2], limit};
    XrLirInstruction predicate = {.op = XR_LIR_ICMP, .type = XR_LIR_I1, .predicate = XR_LIR_LT,
        .operands = compare, .operand_count = 2};
    OK(xr_lir_emit_value(module, loop, &predicate, &condition));
    XrLirValue one = fixed_bytecode_value(module, loop, XR_LIR_CONST, XR_LIR_I64, NULL, 0, 1);
    XrLirValue add[] = {current[2], one};
    XrLirValue next = fixed_bytecode_value(module, loop, XR_LIR_ADD, XR_LIR_I64, add, 2, 0);
    XrLirValue rotated[] = {current[1], current[0], next};
    XrLirInstruction branch = {.op = XR_LIR_CONDBR, .type = XR_LIR_UNIT, .operands = &condition,
        .operand_count = 1, .edges = {{loop, rotated, 3}, {done, current, 3}}, .edge_count = 2};
    OK(xr_lir_set_terminator(module, loop, &branch));
    XrLirValue index = fixed_bytecode_value(module, done, XR_LIR_CONST, XR_LIR_I64, NULL, 0, count - 1), bytes[2];
    for (uint32_t p = 0; p < 2; ++p) {
        XrLirValue get[] = {result[p], index};
        XrLirValue byte = fixed_bytecode_value(module, done, XR_LIR_FIXED_ARRAY_GET, XR_LIR_U8, get, 2, 0);
        bytes[p] = fixed_bytecode_value(module, done, XR_LIR_INTCAST, XR_LIR_I64, &byte, 1, 0);
    }
    XrLirValue scale[] = {bytes[0], fixed_bytecode_value(module, done, XR_LIR_CONST, XR_LIR_I64, NULL, 0, 100)};
    XrLirValue answer[] = {fixed_bytecode_value(module, done, XR_LIR_MUL, XR_LIR_I64, scale, 2, 0), bytes[1]};
    XrLirValue value = fixed_bytecode_value(module, done, XR_LIR_ADD, XR_LIR_I64, answer, 2, 0);
    XrLirInstruction ret = {.op = XR_LIR_RET, .type = XR_LIR_UNIT, .operands = &value, .operand_count = 1};
    OK(xr_lir_set_terminator(module, done, &ret));
    OK(xr_lir_module_finish(module, NULL));
    return module;
}

static XrLirModule *fixed_bytecode_state_fixture(uint32_t count, unsigned mode, XrLirFunction *root) {
    XrLirModule *module = NULL;
    OK(xr_lir_module_create(NULL, &module));
    XrLirType fixed = 0, state_type = XR_LIR_STATE;
    OK(xr_lir_fixed_array_type(module, XR_LIR_U8, count, &fixed));
    XrLirStateModule owner = {0};
    XrLirStateSlot slot = {.payload = fixed, .mutable = mode != 1, .size = count, .alignment = 1};
    XrLirStateLayout layout = {.modules = &owner, .module_count = 1, .slots = &slot, .slot_count = 1,
        .payload_size = count, .payload_alignment = 1};
    OK(xr_lir_set_state_layout(module, &layout));
    XrLirFunctionSpec function = {.name = "fixed_state", .name_length = 11,
        .parameters = &state_type, .parameter_count = 1, .result = XR_LIR_I64};
    OK(xr_lir_declare_function(module, &function, root));
    OK(xr_lir_define_function(module, *root));
    OK(xr_lir_set_state_entry(module, *root));
    XrLirBlock block = {0};
    OK(xr_lir_add_block(module, *root, NULL, 0, &block));
    OK(xr_lir_set_entry(module, *root, block));
    XrLirValue state = {0};
    OK(xr_lir_function_parameter(module, *root, 0, &state));
    XrLirInstruction effect = {.op = XR_LIR_MODULE_BEGIN, .type = XR_LIR_UNIT,
        .operands = &state, .operand_count = 1};
    OK(xr_lir_emit_effect(module, block, &effect));
    if (mode == 3)
        for (uint32_t n = 0; n < 300; ++n)
            (void)fixed_bytecode_value(module, block, XR_LIR_CONST, XR_LIR_I64, NULL, 0, n);
    XrLirValue reference = fixed_bytecode_value(module, block, XR_LIR_STATE_REFERENCE, XR_LIR_STATE_REF, &state, 1, 0);
    XrLirValue *operands = xr_malloc((size_t)count * sizeof(*operands));
    CHECK(operands);
    XrLirValue byte = fixed_bytecode_value(module, block, XR_LIR_CONST, XR_LIR_U8, NULL, 0, 37);
    for (uint32_t n = 0; n < count; ++n) operands[n] = byte;
    XrLirValue initial = fixed_bytecode_value(module, block, XR_LIR_FIXED_ARRAY_NEW, fixed, operands, count, 0);
    XrLirValue args[] = {state, initial};
    if (mode == 1) {
        effect = (XrLirInstruction){.op = XR_LIR_STATE_INITIALIZE, .type = XR_LIR_UNIT,
            .operands = args, .operand_count = 2};
        OK(xr_lir_emit_effect(module, block, &effect));
    } else {
        XrLirValue initializer = fixed_bytecode_value(module, block, XR_LIR_STATE_CONSTRUCT, XR_LIR_STATE_REF, args, 2, 0);
        if (mode != 2) {
            (void)fixed_bytecode_value(module, block, XR_LIR_STATE_READ, fixed, &initializer, 1, 0);
            effect = (XrLirInstruction){.op = XR_LIR_STATE_PUBLISH, .type = XR_LIR_UNIT,
                .operands = &initializer, .operand_count = 1};
            OK(xr_lir_emit_effect(module, block, &effect));
        }
    }
    XrLirValue snapshot = {0};
    XrLirInstruction read = {.op = XR_LIR_STATE_READ, .type = fixed, .operands = &reference, .operand_count = 1,
        .source = {"fixed-state.xr", 14, 45, 6, 45, 12}};
    OK(xr_lir_emit_value(module, block, &read, &snapshot));
    if (mode != 1 && mode != 2) {
        byte = fixed_bytecode_value(module, block, XR_LIR_CONST, XR_LIR_U8, NULL, 0, 91);
        for (uint32_t n = 0; n < count; ++n) operands[n] = byte;
        XrLirValue replacement = fixed_bytecode_value(module, block, XR_LIR_FIXED_ARRAY_NEW, fixed, operands, count, 0);
        XrLirValue values[] = {reference, replacement};
        effect = (XrLirInstruction){.op = XR_LIR_STATE_WRITE, .type = XR_LIR_UNIT,
            .operands = values, .operand_count = 2};
        OK(xr_lir_emit_effect(module, block, &effect));
    }
    xr_free(operands);
    XrLirValue current = fixed_bytecode_value(module, block, XR_LIR_STATE_READ, fixed, &reference, 1, 0);
    XrLirValue index = fixed_bytecode_value(module, block, XR_LIR_CONST, XR_LIR_I64, NULL, 0, count - 1), values[2];
    XrLirValue aggregates[] = {snapshot, current};
    for (uint32_t n = 0; n < 2; ++n) {
        XrLirValue get[] = {aggregates[n], index};
        byte = fixed_bytecode_value(module, block, XR_LIR_FIXED_ARRAY_GET, XR_LIR_U8, get, 2, 0);
        values[n] = fixed_bytecode_value(module, block, XR_LIR_INTCAST, XR_LIR_I64, &byte, 1, 0);
    }
    XrLirValue result = fixed_bytecode_value(module, block, XR_LIR_ADD, XR_LIR_I64, values, 2, 0);
    effect = (XrLirInstruction){.op = XR_LIR_RET, .type = XR_LIR_UNIT, .operands = &result, .operand_count = 1};
    OK(xr_lir_set_terminator(module, block, &effect));
    OK(xr_lir_module_finish(module, NULL));
    return module;
}
