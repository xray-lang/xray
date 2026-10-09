/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * bool_conditions_source_shape.h - Original explicit nullable conditions
 *
 * KEY CONCEPT:
 *   Present zero and absent values remain distinct through typed Boolean control.
 */
#ifndef BOOL_CONDITIONS_SOURCE_SHAPE_H
#define BOOL_CONDITIONS_SOURCE_SHAPE_H
#include "xir/xxir_types.h"
static void bool_conditions_source_shape(const XrXirModule *m) {
    CHECK(m && m->types && m->declarations);
    uint32_t amount = UINT32_MAX, answer = UINT32_MAX;
    for (uint32_t f = 0; f < m->function_count; ++f) {
        const XrXirFunction *fn = &m->functions[f];
        if (fn->name_length == 6 && !memcmp(fn->name, "amount", 6)) { CHECK(amount == UINT32_MAX); amount = f; }
        if (fn->name_length == 6 && !memcmp(fn->name, "answer", 6)) { CHECK(answer == UINT32_MAX); answer = f; }
    }
    CHECK(amount != UINT32_MAX && answer != UINT32_MAX && amount != answer);
    const XrXirFunction *producer = &m->functions[amount];
    CHECK(producer->parameter_count == 1 && producer->parameters[0] == XR_XIR_BOOL &&
        xr_xir_nullable_element(m->types, producer->result) == XR_XIR_I64 && !m->declarations->functions[amount].exported);
    unsigned some = 0, none = 0;
    for (uint32_t i = 0; i < producer->instruction_count; ++i) {
        const XrXirInstruction *op = &producer->instructions[i];
        if (op->op == XR_XIR_NULLABLE_NONE) { ++none; CHECK(op->type == producer->result); }
        if (op->op == XR_XIR_NULLABLE_SOME) {
            ++some; CHECK(op->type == producer->result && op->args[0] >= 1 && op->args[0] - 1 < i);
            const XrXirInstruction *value = &producer->instructions[op->args[0] - 1];
            CHECK(value->op == XR_XIR_CONST_INT && value->type == XR_XIR_I64 && !value->immediate);
        }
    }
    CHECK(some == 1 && none == 1);
    const XrXirFunction *fn = &m->functions[answer];
    CHECK(!fn->parameter_count && fn->result == XR_XIR_I64 && !m->declarations->functions[answer].exported);
    unsigned calls = 0, presences = 0, branches = 0; bool values[2] = {false,false}, match29 = false;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_CALL && op->immediate == amount) {
            CHECK(calls < 2 && op->type == producer->result && op->args[1] == 1 && op->args[0] < fn->operand_count);
            uint32_t id = fn->operands[op->args[0]]; CHECK(id < i);
            const XrXirInstruction *argument = &fn->instructions[id];
            CHECK(argument->op == XR_XIR_CONST_BOOL && argument->type == XR_XIR_BOOL && argument->immediate == (int64_t)calls);
            values[calls++] = true;
        }
        if (op->op == XR_XIR_NULLABLE_IS_SOME) { ++presences; CHECK(op->type == XR_XIR_BOOL); }
        if (op->op == XR_XIR_BRANCH) { ++branches; CHECK(xr_xir_operand_type(fn, op->args[0]) == XR_XIR_BOOL); }
        if (op->op == XR_XIR_CONST_INT && op->type == XR_XIR_I64 && op->immediate == 29) match29 = true;
    }
    CHECK(calls == 2 && values[0] && values[1] && presences >= 5 && branches >= 5 && match29);
}
#endif // BOOL_CONDITIONS_SOURCE_SHAPE_H
