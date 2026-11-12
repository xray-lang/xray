/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_nullable_type_cases.h - Typed layout and receiving-owner boundaries
 */
#include "lir_nullable_fixture.h"

static void nullable_type_owners(void) {
    const XrLirType leaves[] = {XR_LIR_UNIT, XR_LIR_I64};
    for (uint32_t n = 0; n < 2; ++n) {
        XrLirFunction root;
        XrLirModule *source = nullable_none_module(128, leaves[n], &root), *copies[3] = {0}, *target = NULL;
        XrLirFunctionView function;
        OK(xr_lir_function_view(source, root, &function));
        OK(xr_lir_module_create(NULL, &target));
        XrLirType fixed = 0, imported = 0;
        OK(xr_lir_fixed_array_type(target, XR_LIR_U8, 17, &fixed));
        OK(xr_lir_import_type(target, source, function.result, &imported));
        CHECK(imported != function.result);
        OK(xr_lir_rebuild(source, &copies[0]));
        OK(xr_lir_inline(source, &copies[1]));
        OK(xr_lir_sccp(source, &copies[2]));
        xr_lir_module_destroy(source);
        XrLirTypeLayout layout;
        OK(xr_lir_type_layout(target, imported, &layout));
        CHECK(layout.size == (n ? 1032u : 128u) && layout.alignment == (n ? 8u : 1u) &&
            layout.scalar_count == (n ? 129u : 128u) && !layout.managed);
        XrLirScalarPart scalar;
        OK(xr_lir_type_scalar(target, imported, layout.scalar_count - 1, &scalar));
        XrLirType expected_scalar = n ? XR_LIR_I64 : XR_LIR_U8;
        CHECK(scalar.type == expected_scalar && scalar.offset == (n ? 1024u : 127u));
        xr_lir_module_destroy(target);
        for (uint32_t c = 0; c < 3; ++c) {
            OK(xr_lir_verify(copies[c], NULL));
            CHECK(xr_lir_constructed_type_count(copies[c]) == 128);
            xr_lir_module_destroy(copies[c]);
        }
    }
    XrLirModule *module = NULL;
    OK(xr_lir_module_create(NULL, &module));
    XrLirType fixed, nullable, twice;
    OK(xr_lir_fixed_array_type(module, XR_LIR_U8, 17, &fixed));
    OK(xr_lir_nullable_type(module, fixed, &nullable));
    OK(xr_lir_nullable_type(module, nullable, &twice));
    XrLirTypeLayout layout;
    OK(xr_lir_type_layout(module, twice, &layout));
    CHECK(layout.size == 19 && layout.alignment == 1 && layout.scalar_count == 19);
    XrLirScalarPart part;
    OK(xr_lir_type_scalar(module, twice, 18, &part));
    CHECK(part.type == XR_LIR_U8 && part.offset == 18);
    xr_lir_module_destroy(module);
}

static void nullable_type_rejections(void) {
    const XrLirType invalid[] = {XR_LIR_STRING, XR_LIR_ARRAY, XR_LIR_PTR, XR_LIR_STATE_REF,
        XR_LIR_I64_CELL, (XrLirType)UINT32_MAX};
    for (uint32_t n = 0; n < sizeof(invalid) / sizeof(*invalid); ++n) {
        XrLirModule *module = NULL;
        OK(xr_lir_module_create(NULL, &module));
        XrLirType output = XR_LIR_I64;
        CHECK(xr_lir_nullable_type(module, invalid[n], &output) == XR_LIR_INVALID_ARGUMENT && output == XR_LIR_I64);
        xr_lir_module_destroy(module);
    }
    for (uint32_t n = 0; n < 3; ++n) {
        XrLirFunction root;
        XrLirModule *module = nullable_none_module(1, XR_LIR_I64, &root);
        if (n == 0) module->types[0].shape.element = XR_LIR_TYPE_COUNT;
        if (n == 1) module->types[0].layout.size++;
        if (n == 2) module->types[0].shape.count = 1;
        CHECK(xr_lir_verify(module, NULL) != XR_LIR_OK);
        xr_lir_module_destroy(module);
    }
}

static void nullable_instruction_rejections(void) {
    for (uint32_t scenario = 0; scenario < 5; ++scenario) {
        XrLirModule *module = NULL;
        OK(xr_lir_module_create(NULL, &module));
        XrLirType nullable;
        OK(xr_lir_nullable_type(module, XR_LIR_U64, &nullable));
        XrLirType scalar = XR_LIR_I64;
        XrLirFunction function = test_function(module, "invalid_nullable", &scalar, 1, scalar);
        XrLirBlock block = test_block(module, function, NULL, 0);
        OK(xr_lir_set_entry(module, function, block));
        XrLirValue input, output = {0};
        OK(xr_lir_function_parameter(module, function, 0, &input));
        XrLirInstruction instruction = {.op=XR_LIR_NULLABLE_SOME, .type=nullable,
            .operands=&input, .operand_count=1};
        if (scenario == 1) { instruction.op = XR_LIR_NULLABLE_IS_SOME; instruction.type = XR_LIR_I1; }
        if (scenario == 2) { instruction.op = XR_LIR_NULLABLE_UNWRAP; instruction.type = XR_LIR_I64; }
        if (scenario == 3) {
            instruction.op = XR_LIR_NULLABLE_NONE;
            instruction.operand_count = 0;
            instruction.type = XR_LIR_I64;
        }
        if (scenario == 4) instruction.operand_count = 0;
        CHECK(xr_lir_emit_value(module, block, &instruction, &output) != XR_LIR_OK && !output.id);
        xr_lir_module_destroy(module);
    }
}
