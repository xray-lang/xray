/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * register_vm_programs.h - Hand-assembled scalar workloads.
 *
 * KEY CONCEPT:
 *   Fixtures use the checked encoder, including jump fixups, so their layout
 *   is the same layout consumed by an eventual bytecode producer.
 */

#ifndef REGISTER_VM_PROGRAMS_H
#define REGISTER_VM_PROGRAMS_H

#include "vm/register/xrbc.h"
#include "base/xchecks.h"

static void emit(uint32_t *code, size_t pc, XrRbcOp op, uint32_t a, uint32_t b, uint32_t c) {
    XR_CHECK(xr_rbc_encode(code + pc, (XrRbcOperands) {op, a, b, c, 0}), "fixture encoding");
}

static void immediate(uint32_t *code, size_t pc, XrRbcOp op, uint32_t a, int64_t value) {
    XR_CHECK(xr_rbc_encode(code + pc, (XrRbcOperands) {op, a, 0, 0, value}), "fixture immediate");
}

static XrRbcFunction make_fib(uint32_t code[13]) {
    immediate(code, 0, XR_RBC_LOADI, 1, 2);
    emit(code, 1, XR_RBC_LT, 0, 1, 0);
    XR_CHECK(xr_rbc_jump(code + 2, 2, 12, 13), "fib base case");
    immediate(code, 3, XR_RBC_LOADI, 1, 1);
    emit(code, 4, XR_RBC_SUB, 5, 0, 1);
    immediate(code, 5, XR_RBC_CALL, 4, 0);
    emit(code, 6, XR_RBC_MOV, 3, 4, 0);
    immediate(code, 7, XR_RBC_LOADI, 1, 2);
    emit(code, 8, XR_RBC_SUB, 5, 0, 1);
    immediate(code, 9, XR_RBC_CALL, 4, 0);
    emit(code, 10, XR_RBC_ADD, 2, 3, 4);
    emit(code, 11, XR_RBC_RET, 2, 0, 0);
    emit(code, 12, XR_RBC_RET, 0, 0, 0);
    return (XrRbcFunction) {code, 13, NULL, 0, 6, 1, NULL, 0, 0};
}

static XrRbcFunction make_loop(uint32_t code[10]) {
    immediate(code, 0, XR_RBC_LOADI, 1, 0);
    immediate(code, 1, XR_RBC_LOADI, 2, 0);
    immediate(code, 2, XR_RBC_LOADI, 3, 1);
    emit(code, 3, XR_RBC_LT, 1, 0, 0);
    XR_CHECK(xr_rbc_jump(code + 4, 4, 6, 10), "loop body");
    emit(code, 5, XR_RBC_RET, 2, 0, 0);
    emit(code, 6, XR_RBC_ADD, 2, 2, 1);
    emit(code, 7, XR_RBC_ADD, 1, 1, 3);
    XR_CHECK(xr_rbc_jump(code + 8, 8, 3, 10), "loop backedge");
    emit(code, 9, XR_RBC_RET0, 0, 0, 0);
    return (XrRbcFunction) {code, 10, NULL, 0, 4, 1, NULL, 0, 0};
}

static XrRbcFunction make_countdown(uint32_t code[11]) {
    immediate(code, 0, XR_RBC_LOADI, 1, 0);
    emit(code, 1, XR_RBC_EQ, 0, 1, 0);
    XR_CHECK(xr_rbc_jump(code + 2, 2, 10, 11), "countdown base case");
    immediate(code, 3, XR_RBC_LOADI, 1, 1);
    emit(code, 4, XR_RBC_SUB, 3, 0, 1);
    immediate(code, 5, XR_RBC_CALL, 2, 0);
    emit(code, 6, XR_RBC_ADD, 0, 2, 1);
    emit(code, 7, XR_RBC_RET, 0, 0, 0);
    emit(code, 8, XR_RBC_RET0, 0, 0, 0);
    emit(code, 9, XR_RBC_RET0, 0, 0, 0);
    emit(code, 10, XR_RBC_RET, 0, 0, 0);
    return (XrRbcFunction) {code, 11, NULL, 0, 4, 1, NULL, 0, 0};
}

#endif  // REGISTER_VM_PROGRAMS_H
