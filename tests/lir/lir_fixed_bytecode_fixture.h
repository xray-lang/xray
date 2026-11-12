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
