/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_fixed_array_op_cases.h - Exact construction and trapping byte extraction
 */
#include "lir/xlir_string_elide.h"

static XrLirModule *fixed_ops_module(uint32_t count, bool negative_index) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType fixed = 0;
    OK(xr_lir_fixed_array_type(m, XR_LIR_U8, count, &fixed));
    XrLirType types[] = {XR_LIR_U8, XR_LIR_I64};
    XrLirFunction root = test_function(m, "fixed_length", types, 2, XR_LIR_I64);
    XrLirBlock block = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, block));
    XrLirValue byte = {0}, index = {0}, value = {0}, ignored = {0}, operands[256];
    OK(xr_lir_function_parameter(m, root, 0, &byte));
    OK(xr_lir_function_parameter(m, root, 1, &index));
    CHECK(count <= sizeof(operands) / sizeof(*operands));
    for (uint32_t n = 0; n < count; ++n) operands[n] = byte;
    XrLirInstruction instruction = {.op = XR_LIR_FIXED_ARRAY_NEW, .type = fixed,
        .operands = operands, .operand_count = count};
    OK(xr_lir_emit_value(m, block, &instruction, &value));
    if (negative_index) index = test_constant(m, block, XR_LIR_I64, UINT64_MAX);
    XrLirValue arguments[] = {value, index};
    char path[] = "fixed-get.xr";
    instruction = (XrLirInstruction){.op = XR_LIR_FIXED_ARRAY_GET, .type = XR_LIR_U8,
        .operands = arguments, .operand_count = 2,
        .source = {path, sizeof(path) - 1, 37, 4, 37, 12}};
    OK(xr_lir_emit_value(m, block, &instruction, &ignored));
    memset(path, 'x', sizeof(path) - 1);
    test_return(m, block, test_constant(m, block, XR_LIR_I64, count));
    OK(xr_lir_module_finish(m, NULL));
    return m;
}

static void fixed_ops_preserved(const XrLirModule *m, uint32_t count) {
    uint32_t created = 0, extracted = 0;
    for (uint32_t f = 0; f < xr_lir_function_count(m); ++f) {
        XrLirFunction function = {0};
        XrLirFunctionView view;
        OK(xr_lir_function_at(m, f, &function));
        OK(xr_lir_function_view(m, function, &view));
        for (uint32_t b = 0; b < view.block_count; ++b) {
            XrLirBlock block;
            XrLirBlockView body;
            OK(xr_lir_block_at(m, function, b, &block));
            OK(xr_lir_block_view(m, block, &body));
            for (uint32_t i = 0; i < body.instruction_count; ++i) {
                XrLirInstructionView instruction;
                OK(xr_lir_instruction_view(m, block, i, &instruction));
                if (instruction.op == XR_LIR_FIXED_ARRAY_NEW) {
                    XrLirTypeView shape;
                    OK(xr_lir_type_view(m, instruction.type, &shape));
                    CHECK(shape.count == count && instruction.operand_count == count);
                    ++created;
                }
                if (instruction.op == XR_LIR_FIXED_ARRAY_GET) {
                    CHECK(instruction.type == XR_LIR_U8 && instruction.source.line == 37 &&
                        instruction.source.column == 4 && instruction.source.path_length == 12 &&
                        !memcmp(instruction.source.path, "fixed-get.xr", 12));
                    ++extracted;
                }
            }
        }
    }
    CHECK(created == 1 && extracted == 1);
}

static void fixed_array_operations(void) {
    const uint32_t counts[] = {1, 11, 256};
    for (size_t n = 0; n < sizeof(counts) / sizeof(*counts); ++n) {
        XrLirModule *source = fixed_ops_module(counts[n], n == 1), *copies[4] = {0};
        OK(xr_lir_rebuild(source, &copies[0]));
        OK(xr_lir_inline(source, &copies[1]));
        OK(xr_lir_sccp(source, &copies[2]));
        OK(xr_lir_string_elide(source, &copies[3]));
        xr_lir_module_destroy(source);
        for (size_t i = 0; i < 4; ++i) {
            OK(xr_lir_verify(copies[i], NULL));
            fixed_ops_preserved(copies[i], counts[n]);
            xr_lir_module_destroy(copies[i]);
        }
    }
}

static void fixed_array_operation_rejections(void) {
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirType fixed = 0;
        OK(xr_lir_fixed_array_type(m, XR_LIR_U8, 11, &fixed));
        XrLirType types[] = {XR_LIR_U8, XR_LIR_U64, XR_LIR_I64};
        XrLirFunction root = test_function(m, "bad_fixed", types, 3, XR_LIR_I64);
        XrLirBlock block = test_block(m, root, NULL, 0);
        OK(xr_lir_set_entry(m, root, block));
        XrLirValue byte = {0}, wide = {0}, index = {0}, value = {0}, operands[11];
        OK(xr_lir_function_parameter(m, root, 0, &byte));
        OK(xr_lir_function_parameter(m, root, 1, &wide));
        OK(xr_lir_function_parameter(m, root, 2, &index));
        for (uint32_t n = 0; n < 11; ++n) operands[n] = byte;
        if (scenario == 1) operands[0] = wide;
        XrLirInstruction instruction = {.op = XR_LIR_FIXED_ARRAY_NEW, .type = fixed,
            .operands = operands, .operand_count = scenario == 0 ? 10u : 11u};
        XrLirStatus status = xr_lir_emit_value(m, block, &instruction, &value);
        if (scenario < 2) CHECK(status != XR_LIR_OK && !value.id);
        else {
            OK(status);
            XrLirValue arguments[] = {scenario == 2 ? byte : value, scenario == 3 ? wide : index};
            instruction = (XrLirInstruction){.op = XR_LIR_FIXED_ARRAY_GET,
                .type = scenario == 4 ? XR_LIR_I64 : XR_LIR_U8, .operands = arguments,
                .operand_count = 2, .bits = scenario == 5 ? 1u : 0u};
            XrLirValue output = {0};
            CHECK(xr_lir_emit_value(m, block, &instruction, &output) != XR_LIR_OK && !output.id);
        }
        xr_lir_module_destroy(m);
    }
}
