/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_fixed_c_cases.h - By-value aggregates across calls, edges and State
 */
static XrLirValue fixed_c_emit(XrLirBlock b, XrLirOp op, XrLirType type,
    const XrLirValue *args, uint32_t count) {
    XrLirInstruction instruction = {.op = op, .type = type, .operands = args, .operand_count = count};
    XrLirValue value = {0};
    OK(xr_lir_emit_value(module, b, &instruction, &value));
    return value;
}

static XrLirValue fixed_c_call(XrLirBlock b, XrLirFunction callee, XrLirType type,
    XrLirValue argument) {
    XrLirInstruction instruction = {.op = XR_LIR_CALL, .type = type, .callee = callee,
        .operands = &argument, .operand_count = 1};
    XrLirValue value = {0};
    OK(xr_lir_emit_value(module, b, &instruction, &value));
    return value;
}

static XrLirFunction fixed_c_make(XrLirType fixed, uint32_t count) {
    XrLirType byte_type = XR_LIR_U8;
    XrLirFunction make = function("fixed_make", &byte_type, 1, fixed);
    XrLirBlock b = block(make, NULL, 0);
    OK(xr_lir_set_entry(module, make, b));
    XrLirValue seed = {0}, values[256];
    OK(xr_lir_function_parameter(module, make, 0, &seed));
    CHECK(count <= sizeof(values) / sizeof(*values));
    for (uint32_t n = 0; n < count; ++n) {
        XrLirValue arguments[] = {seed, constant(b, XR_LIR_U8, n)};
        values[n] = fixed_c_emit(b, XR_LIR_ADD, XR_LIR_U8, arguments, 2);
    }
    ret(b, fixed_c_emit(b, XR_LIR_FIXED_ARRAY_NEW, fixed, values, count));
    return make;
}

static XrLirValue fixed_c_get(XrLirBlock b, XrLirValue aggregate, XrLirValue index) {
    XrLirValue arguments[] = {aggregate, index}, byte = {0};
    XrLirInstruction get = {.op = XR_LIR_FIXED_ARRAY_GET, .type = XR_LIR_U8,
        .operands = arguments, .operand_count = 2,
        .source = {"fixed-get.xr", 12, 37, 4, 37, 12}};
    OK(xr_lir_emit_value(module, b, &get, &byte));
    return byte;
}

static XrLirValue fixed_c_widen(XrLirBlock b, XrLirValue byte) {
    return fixed_c_emit(b, XR_LIR_INTCAST, XR_LIR_I64, &byte, 1);
}

static void fixed_c_values(uint32_t count, bool returns_fixed, int panic) {
    XrLirType fixed = 0;
    OK(xr_lir_fixed_array_type(module, XR_LIR_U8, count, &fixed));
    XrLirFunction make = fixed_c_make(fixed, count);
    XrLirFunction forward = function("fixed_forward", &fixed, 1, fixed);
    XrLirBlock fb = block(forward, NULL, 0);
    OK(xr_lir_set_entry(module, forward, fb));
    XrLirValue input = {0};
    OK(xr_lir_function_parameter(module, forward, 0, &input));
    ret(fb, input);
    root = function("fixed_root", NULL, 0, returns_fixed ? fixed : XR_LIR_I64);
    XrLirBlock entry = block(root, NULL, 0);
    OK(xr_lir_set_entry(module, root, entry));
    XrLirValue a = fixed_c_call(entry, make, fixed, constant(entry, XR_LIR_U8, 3));
    a = fixed_c_call(entry, forward, fixed, a);
    if (returns_fixed) { ret(entry, a); return; }
    if (panic) {
        (void)fixed_c_get(entry, a, constant(entry, XR_LIR_I64, panic < 0 ? UINT64_MAX : count));
        ret(entry, constant(entry, XR_LIR_I64, 19));
        return;
    }
    XrLirValue other = fixed_c_call(entry, make, fixed, constant(entry, XR_LIR_U8, 9));
    XrLirType types[] = {fixed, fixed, XR_LIR_I64};
    XrLirBlock loop = block(root, types, 3), body = block(root, NULL, 0), exit_block = block(root, NULL, 0);
    XrLirValue initial[] = {a, other, constant(entry, XR_LIR_I64, 3)};
    branch(entry, loop, initial, 3);
    XrLirValue x = {0}, y = {0}, remaining = {0};
    OK(xr_lir_block_parameter(module, loop, 0, &x));
    OK(xr_lir_block_parameter(module, loop, 1, &y));
    OK(xr_lir_block_parameter(module, loop, 2, &remaining));
    XrLirValue zero = constant(loop, XR_LIR_I64, 0);
    XrLirValue condition = compare(loop, XR_LIR_GT, remaining, zero);
    conditional(loop, condition, (XrLirEdge){body, NULL, 0}, (XrLirEdge){exit_block, NULL, 0});
    XrLirValue next = operation(body, XR_LIR_SUB, XR_LIR_I64, remaining, constant(body, XR_LIR_I64, 1));
    XrLirValue swapped[] = {y, x, next};
    branch(body, loop, swapped, 3);
    XrLirValue index = constant(exit_block, XR_LIR_I64, count - 1);
    XrLirValue lhs = fixed_c_widen(exit_block, fixed_c_get(exit_block, x, index));
    XrLirValue rhs = fixed_c_widen(exit_block, fixed_c_get(exit_block, y, index));
    ret(exit_block, operation(exit_block, XR_LIR_ADD, XR_LIR_I64, lhs, rhs));
}

static void fixed_c_state(bool unpublished, bool returns_fixed) {
    XrLirType fixed = 0, state_type = XR_LIR_STATE;
    OK(xr_lir_fixed_array_type(module, XR_LIR_U8, 17, &fixed));
    XrLirStateModule owner = {0};
    XrLirStateSlot slot = {.payload = fixed, .mutable = true, .size = 17, .alignment = 1};
    XrLirStateLayout layout = {.modules = &owner, .module_count = 1, .slots = &slot,
        .slot_count = 1, .payload_size = 17, .payload_alignment = 1};
    OK(xr_lir_set_state_layout(module, &layout));
    XrLirFunction make = fixed_c_make(fixed, 17);
    root = function("fixed_state", &state_type, 1, returns_fixed ? fixed : XR_LIR_I64);
    OK(xr_lir_set_state_entry(module, root));
    XrLirBlock b = block(root, NULL, 0);
    OK(xr_lir_set_entry(module, root, b));
    XrLirValue state = {0}, reference = {0}, snapshot = {0};
    OK(xr_lir_function_parameter(module, root, 0, &state));
    XrLirInstruction instruction = {.op = XR_LIR_MODULE_BEGIN, .type = XR_LIR_UNIT,
        .operands = &state, .operand_count = 1};
    OK(xr_lir_emit_effect(module, b, &instruction));
    if (!unpublished) {
        XrLirValue initial = fixed_c_call(b, make, fixed, constant(b, XR_LIR_U8, 3));
        XrLirValue args[] = {state, initial};
        reference = fixed_c_emit(b, XR_LIR_STATE_CONSTRUCT, XR_LIR_STATE_REF, args, 2);
        instruction = (XrLirInstruction){.op = XR_LIR_STATE_PUBLISH, .type = XR_LIR_UNIT,
            .operands = &reference, .operand_count = 1};
        OK(xr_lir_emit_effect(module, b, &instruction));
        instruction.op = XR_LIR_MODULE_READY; instruction.operands = &state;
        OK(xr_lir_emit_effect(module, b, &instruction));
    }
    reference = fixed_c_emit(b, XR_LIR_STATE_REFERENCE, XR_LIR_STATE_REF, &state, 1);
    instruction = (XrLirInstruction){.op = XR_LIR_STATE_READ, .type = fixed,
        .operands = &reference, .operand_count = 1,
        .source = {"fixed-state.xr", 14, 45, 6, 45, 12}};
    OK(xr_lir_emit_value(module, b, &instruction, &snapshot));
    if (unpublished) { ret(b, constant(b, XR_LIR_I64, 17)); return; }
    XrLirValue replacement = fixed_c_call(b, make, fixed, constant(b, XR_LIR_U8, 9));
    XrLirValue args[] = {reference, replacement};
    instruction = (XrLirInstruction){.op = XR_LIR_STATE_WRITE, .type = XR_LIR_UNIT,
        .operands = args, .operand_count = 2};
    OK(xr_lir_emit_effect(module, b, &instruction));
    if (returns_fixed) { ret(b, snapshot); return; }
    XrLirValue current = fixed_c_emit(b, XR_LIR_STATE_READ, fixed, &reference, 1);
    XrLirValue zero = constant(b, XR_LIR_I64, 0);
    XrLirValue a = fixed_c_widen(b, fixed_c_get(b, snapshot, zero));
    XrLirValue other = fixed_c_widen(b, fixed_c_get(b, current, zero));
    ret(b, operation(b, XR_LIR_ADD, XR_LIR_I64, a, other));
}

static void fixed_c_fixture(const char *name) {
    if (!strcmp(name, "fixed-state")) fixed_c_state(false, false);
    else if (!strcmp(name, "fixed-state-return")) fixed_c_state(false, true);
    else if (!strcmp(name, "fixed-unpublished")) fixed_c_state(true, false);
    else if (!strcmp(name, "fixed-negative")) fixed_c_values(11, false, -1);
    else if (!strcmp(name, "fixed-bounds")) fixed_c_values(11, false, 1);
    else if (!strcmp(name, "fixed-return")) fixed_c_values(17, true, 0);
    else {
        unsigned count = 0; char extra = 0;
        CHECK(sscanf(name, "fixed-%u%c", &count, &extra) == 1);
        CHECK(count && count <= 256);
        fixed_c_values(count, false, 0);
    }
}
