/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_c_state_cases.h - Reference and cache PHIs share parallel-copy ordering
 */
#ifndef LIR_C_STATE_CASES_H
#define LIR_C_STATE_CASES_H
#include "lir/xlir_state.h"

static void c_state_effect(XrLirBlock b, XrLirOp op, XrLirValue operand) {
    XrLirInstruction instruction = {.op=op, .type=XR_LIR_UNIT,
        .operands=&operand, .operand_count=1};
    OK(xr_lir_emit_effect(module, b, &instruction));
}

static void c_state_reference_cycle(uint64_t rounds) {
    XrLirStateModule owner = {0};
    XrLirStateSlot slots[] = {
        {.payload=XR_LIR_I64, .mutable=true, .size=8, .alignment=8},
        {.payload=XR_LIR_I64, .mutable=true, .offset=8, .size=8, .alignment=8}
    };
    XrLirStateLayout layout = {.modules=&owner, .module_count=1, .slots=slots,
        .slot_count=2, .payload_size=16, .payload_alignment=8};
    OK(xr_lir_set_state_layout(module, &layout));
    XrLirType state_type = XR_LIR_STATE;
    root = function("state_reference_cycle", &state_type, 1, XR_LIR_I64);
    OK(xr_lir_set_state_entry(module, root));
    XrLirBlock entry = block(root, NULL, 0);
    OK(xr_lir_set_entry(module, root, entry));
    XrLirValue state, references[2];
    OK(xr_lir_function_parameter(module, root, 0, &state));
    c_state_effect(entry, XR_LIR_MODULE_BEGIN, state);
    for (uint32_t slot = 0; slot < 2; ++slot) {
        XrLirValue operands[] = {state, constant(entry, XR_LIR_I64, 11u * (slot + 1))};
        XrLirInstruction instruction = {.op=XR_LIR_STATE_CONSTRUCT, .type=XR_LIR_STATE_REF,
            .operands=operands, .operand_count=2, .bits=slot};
        OK(xr_lir_emit_value(module, entry, &instruction, &references[slot]));
        c_state_effect(entry, XR_LIR_STATE_PUBLISH, references[slot]);
    }
    c_state_effect(entry, XR_LIR_MODULE_READY, state);
    XrLirType types[] = {XR_LIR_STATE_REF, XR_LIR_STATE_REF, XR_LIR_I64};
    XrLirBlock loop = block(root, types, 3), body = block(root, NULL, 0);
    XrLirBlock done = block(root, NULL, 0);
    XrLirValue carried[3];
    for (uint32_t p = 0; p < 3; ++p)
        OK(xr_lir_block_parameter(module, loop, p, &carried[p]));
    XrLirStateReference shape = {XR_LIR_I64, true};
    OK(xr_lir_set_state_reference(module, carried[0], &shape));
    OK(xr_lir_set_state_reference(module, carried[1], &shape));
    XrLirValue initial[] = {references[0], references[1], constant(entry, XR_LIR_I64, rounds)};
    branch(entry, loop, initial, 3);
    XrLirValue zero = constant(loop, XR_LIR_I64, 0);
    XrLirValue condition = compare(loop, XR_LIR_GT, carried[2], zero);
    conditional(loop, condition, (XrLirEdge){.target=body}, (XrLirEdge){.target=done});
    XrLirValue next = operation(body, XR_LIR_SUB, XR_LIR_I64, carried[2],
        constant(body, XR_LIR_I64, 1));
    XrLirValue swapped[] = {carried[1], carried[0], next};
    branch(body, loop, swapped, 3);
    XrLirValue write[] = {carried[0], constant(done, XR_LIR_I64, 33)};
    XrLirInstruction instruction = {.op=XR_LIR_STATE_WRITE, .type=XR_LIR_UNIT,
        .operands=write, .operand_count=2};
    OK(xr_lir_emit_effect(module, done, &instruction));
    XrLirValue a = operation(done, XR_LIR_STATE_READ, XR_LIR_I64, carried[0], (XrLirValue){0});
    XrLirValue b = operation(done, XR_LIR_STATE_READ, XR_LIR_I64, carried[1], (XrLirValue){0});
    XrLirValue scaled = operation(done, XR_LIR_MUL, XR_LIR_I64, a, constant(done, XR_LIR_I64, 100));
    ret(done, operation(done, XR_LIR_ADD, XR_LIR_I64, scaled, b));
}

#endif // LIR_C_STATE_CASES_H
