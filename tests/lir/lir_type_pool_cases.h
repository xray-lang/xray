/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_type_pool_cases.h - Exact aggregate shapes across receiving owners
 */
#include "lir/xlir_types.h"

static XrLirModule *type_pool_identity(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType type = 0;
    OK(xr_lir_fixed_array_type(m, XR_LIR_U8, 11, &type));
    XrLirIntrinsic intrinsic = {99, &type, 1, type, true, 0};
    XrLirRegistry registry = {&intrinsic, 1};
    OK(xr_lir_set_registry(m, &registry));
    XrLirFunction f = test_function(m, "fixed_identity", &type, 1, type);
    XrLirBlock entry = test_block(m, f, NULL, 0), done = test_block(m, f, &type, 1);
    XrLirValue input = {0}, result = {0};
    OK(xr_lir_function_parameter(m, f, 0, &input));
    OK(xr_lir_block_parameter(m, done, 0, &result));
    OK(xr_lir_set_entry(m, f, entry));
    test_branch(m, entry, done, &input, 1);
    test_return(m, done, result);
    OK(xr_lir_module_finish(m, NULL));
    return m;
}

static void type_pool_queries_and_receivers(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    const uint32_t counts[] = {1, 7, 8, 9, 11, 16, 17, 256, 65535};
    for (size_t n = 0; n < sizeof(counts) / sizeof(*counts); ++n) {
        XrLirType type = 0, again = 0;
        OK(xr_lir_fixed_array_type(m, XR_LIR_U8, counts[n], &type));
        OK(xr_lir_fixed_array_type(m, XR_LIR_U8, counts[n], &again));
        CHECK(type == again && type >= XR_LIR_TYPE_COUNT);
        XrLirTypeView view;
        XrLirTypeLayout layout;
        XrLirScalarPart leaf;
        OK(xr_lir_type_view(m, type, &view));
        OK(xr_lir_type_layout(m, type, &layout));
        OK(xr_lir_type_scalar(m, type, counts[n] - 1, &leaf));
        CHECK(view.kind == XR_LIR_TYPE_FIXED_ARRAY && view.element == XR_LIR_U8 && view.count == counts[n]);
        CHECK(layout.size == counts[n] && layout.alignment == 1 && layout.scalar_count == counts[n] && !layout.managed);
        CHECK(leaf.type == XR_LIR_U8 && leaf.offset == counts[n] - 1);
        leaf = (XrLirScalarPart){XR_LIR_I64, 77};
        CHECK(xr_lir_type_scalar(m, type, counts[n], &leaf) == XR_LIR_INVALID_ARGUMENT &&
            leaf.type == XR_LIR_I64 && leaf.offset == 77);
    }
    XrLirModule *receiver = NULL;
    OK(xr_lir_module_create(NULL, &receiver));
    XrLirType first = 0, imported = 0;
    OK(xr_lir_fixed_array_type(receiver, XR_LIR_U8, 11, &first));
    OK(xr_lir_import_type(receiver, m, XR_LIR_TYPE_COUNT, &imported));
    CHECK(imported != first);
    xr_lir_module_destroy(m);
    XrLirTypeView view;
    OK(xr_lir_type_view(receiver, imported, &view));
    CHECK(view.count == 1);
    OK(xr_lir_type_view(receiver, first, &view));
    CHECK(view.count == 11);
    xr_lir_module_destroy(receiver);

    XrLirModule *source = type_pool_identity(), *copies[3] = {0};
    OK(xr_lir_rebuild(source, &copies[0]));
    OK(xr_lir_inline(source, &copies[1]));
    OK(xr_lir_sccp(source, &copies[2]));
    xr_lir_module_destroy(source);
    for (size_t n = 0; n < 3; ++n) {
        OK(xr_lir_verify(copies[n], NULL));
        CHECK(xr_lir_constructed_type_count(copies[n]) == 1);
        XrLirFunction f;
        XrLirFunctionView function;
        OK(xr_lir_function_at(copies[n], 0, &f));
        OK(xr_lir_function_view(copies[n], f, &function));
        OK(xr_lir_type_view(copies[n], function.result, &view));
        CHECK(view.count == 11 && function.parameter_types[0] == function.result);
        XrLirIntrinsic intrinsic;
        OK(xr_lir_intrinsic_view(copies[n], 99, &intrinsic));
        CHECK(intrinsic.result == function.result && intrinsic.parameters[0] == function.result);
        xr_lir_module_destroy(copies[n]);
    }
}

static void type_pool_rejections(void) {
    const uint32_t counts[] = {0, 65536, UINT32_MAX};
    for (size_t n = 0; n < sizeof(counts) / sizeof(*counts); ++n) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirType output = XR_LIR_I64;
        CHECK(xr_lir_fixed_array_type(m, XR_LIR_U8, counts[n], &output) == XR_LIR_INVALID_ARGUMENT);
        CHECK(output == XR_LIR_I64 && !xr_lir_module_is_readable(m));
        xr_lir_module_destroy(m);
    }
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType output = XR_LIR_I64;
    CHECK(xr_lir_fixed_array_type(m, XR_LIR_I64, 11, &output) == XR_LIR_INVALID_ARGUMENT && output == XR_LIR_I64);
    xr_lir_module_destroy(m);
    m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType type = 0;
    OK(xr_lir_fixed_array_type(m, XR_LIR_U8, 11, &type));
    XrLirFunction f = test_function(m, "bad_constant", NULL, 0, type);
    XrLirBlock block = test_block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, block));
    XrLirInstruction instruction = {.op = XR_LIR_CONST, .type = type};
    XrLirValue value = {0};
    CHECK(xr_lir_emit_value(m, block, &instruction, &value) != XR_LIR_OK && !value.id);
    xr_lir_module_destroy(m);
}

static void type_pool_calls_and_state(void) {
    for (unsigned wrong = 0; wrong < 2; ++wrong) {
        XrLirModule *m = type_pool_identity();
        /* A finished source supplies its pool to a fresh writable receiver. */
        XrLirModule *target = NULL;
        OK(xr_lir_module_create(NULL, &target));
        OK(xr_lir_copy_types(target, m));
        xr_lir_module_destroy(m);
        XrLirType fixed = XR_LIR_TYPE_COUNT, actual = fixed;
        if (wrong) OK(xr_lir_fixed_array_type(target, XR_LIR_U8, 7, &actual));
        XrLirFunction callee = test_function(target, "callee", &fixed, 1, fixed);
        XrLirBlock body = test_block(target, callee, NULL, 0);
        XrLirValue input = {0};
        OK(xr_lir_function_parameter(target, callee, 0, &input));
        OK(xr_lir_set_entry(target, callee, body));
        test_return(target, body, input);
        XrLirFunction caller = test_function(target, "caller", &actual, 1, fixed);
        XrLirBlock block = test_block(target, caller, NULL, 0);
        OK(xr_lir_function_parameter(target, caller, 0, &input));
        OK(xr_lir_set_entry(target, caller, block));
        XrLirInstruction call = {.op = XR_LIR_CALL, .type = fixed, .callee = callee,
            .operands = &input, .operand_count = 1};
        XrLirValue output = {0};
        XrLirStatus status = xr_lir_emit_value(target, block, &call, &output);
        if (wrong) CHECK(status != XR_LIR_OK && !output.id);
        else {
            OK(status);
            test_return(target, block, output);
            OK(xr_lir_module_finish(target, NULL));
        }
        xr_lir_module_destroy(target);
    }

    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType fixed = 0;
    OK(xr_lir_fixed_array_type(m, XR_LIR_U8, 11, &fixed));
    XrLirStateModule module = {0};
    XrLirStateSlot slot = {.payload = fixed, .size = 11, .alignment = 1};
    XrLirStateLayout layout = {.modules = &module, .module_count = 1, .slots = &slot,
        .slot_count = 1, .payload_size = 11, .payload_alignment = 1};
    OK(xr_lir_set_state_layout(m, &layout));
    XrLirType parameters[] = {fixed, XR_LIR_STATE};
    XrLirFunction root = test_function(m, "state_fixed", parameters, 2, fixed);
    OK(xr_lir_set_state_entry(m, root));
    XrLirBlock block = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, block));
    XrLirValue input = {0}, state = {0};
    OK(xr_lir_function_parameter(m, root, 0, &input));
    OK(xr_lir_function_parameter(m, root, 1, &state));
    XrLirInstruction instruction = {.op = XR_LIR_MODULE_BEGIN, .type = XR_LIR_UNIT,
        .operands = &state, .operand_count = 1};
    OK(xr_lir_emit_effect(m, block, &instruction));
    XrLirValue operands[] = {state, input};
    instruction = (XrLirInstruction){.op = XR_LIR_STATE_INITIALIZE, .type = XR_LIR_UNIT,
        .operands = operands, .operand_count = 2};
    OK(xr_lir_emit_effect(m, block, &instruction));
    instruction = (XrLirInstruction){.op = XR_LIR_MODULE_READY, .type = XR_LIR_UNIT,
        .operands = &state, .operand_count = 1};
    OK(xr_lir_emit_effect(m, block, &instruction));
    instruction.op = XR_LIR_STATE_REFERENCE;
    instruction.type = XR_LIR_STATE_REF;
    XrLirValue reference = {0}, result = {0};
    OK(xr_lir_emit_value(m, block, &instruction, &reference));
    instruction = (XrLirInstruction){.op = XR_LIR_STATE_READ, .type = fixed,
        .operands = &reference, .operand_count = 1};
    OK(xr_lir_emit_value(m, block, &instruction, &result));
    test_return(m, block, result);
    OK(xr_lir_module_finish(m, NULL));
    XrLirModule *copy = NULL;
    OK(xr_lir_rebuild(m, &copy));
    xr_lir_module_destroy(m);
    OK(xr_lir_state_layout_view(copy, &layout));
    XrLirTypeView shape;
    OK(xr_lir_type_view(copy, layout.slots[0].payload, &shape));
    CHECK(shape.count == 11 && layout.slots[0].size == 11);
    xr_lir_module_destroy(copy);
}
