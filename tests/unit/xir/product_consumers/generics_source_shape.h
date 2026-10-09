/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * generics_source_shape.h - Original explicit and inferred specialization calls
 *
 * KEY CONCEPT:
 *   Two integer calls share one real identity specialization; the Boolean
 *   specialization returns its own parameter and controls the original sum.
 */
#ifndef GENERICS_SOURCE_SHAPE_H
#define GENERICS_SOURCE_SHAPE_H
#include "xir/xxir_types.h"
static void generics_source_callee(const XrXirModule *m, uint32_t id, XrXirType type) {
    CHECK(id < m->function_count); const XrXirFunction *fn = &m->functions[id];
    CHECK(fn->name_length > 9 && !memcmp(fn->name, "identity$", 9));
    CHECK(fn->parameter_count == 1 && fn->parameters[0] == type && fn->result == type);
    CHECK(!m->declarations->functions[id].exported && !m->declarations->functions[id].nominal_owner);
    CHECK(!m->generics || !m->generics[id].parameter_count);
    unsigned returns = 0;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        CHECK(op->op == XR_XIR_RETURN && !op->args[0]); ++returns;
    }
    CHECK(returns == 1);
}
static void generics_source_shape(const XrXirModule *m) {
    CHECK(m && m->declarations && m->declarations->module_count == 1 && !m->declarations->slot_count);
    uint32_t answer = UINT32_MAX;
    for (uint32_t f = 0; f < m->function_count; ++f) {
        const XrXirFunction *fn = &m->functions[f];
        if (fn->name_length == 6 && !memcmp(fn->name, "answer", 6)) {
            CHECK(answer == UINT32_MAX && !fn->parameter_count && fn->result == XR_XIR_I64 && !m->declarations->functions[f].exported); answer = f;
        }
    }
    CHECK(answer != UINT32_MAX); const XrXirFunction *fn = &m->functions[answer];
    uint32_t integer = UINT32_MAX, boolean = UINT32_MAX, values[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX}, sum = UINT32_MAX;
    unsigned calls = 0, adds = 0, branches = 0, returns = 0;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_CALL) {
            CHECK(calls < 3 && op->immediate >= 0 && (uint64_t)op->immediate < m->function_count && (uint32_t)op->immediate != answer);
            uint32_t target = (uint32_t)op->immediate; XrXirType type = calls < 2 ? XR_XIR_I64 : XR_XIR_BOOL;
            generics_source_callee(m, target, type); CHECK(op->type == type && op->args[1] == 1 && op->args[0] < fn->operand_count);
            uint32_t arg = fn->operands[op->args[0]]; CHECK(arg < i); const XrXirInstruction *value = &fn->instructions[arg];
            CHECK(value->type == type && value->op == (calls < 2 ? XR_XIR_CONST_INT : XR_XIR_CONST_BOOL));
            CHECK(value->immediate == (!calls ? 40 : 1));
            if (calls < 2) { CHECK(integer == UINT32_MAX || integer == target); integer = target; }
            else { CHECK(boolean == UINT32_MAX && integer != target); boolean = target; }
            values[calls++] = i;
        }
        if (op->op == XR_XIR_BRANCH) { ++branches; CHECK(calls == 3 && op->args[0] == values[2] && xr_xir_operand_type(fn, op->args[0]) == XR_XIR_BOOL); }
        if (op->op == XR_XIR_ADD_INT) {
            CHECK(calls == 3 && adds < 2 && op->type == XR_XIR_I64);
            if (!adds) CHECK(op->args[0] == values[0] && op->args[1] == values[1]);
            else { CHECK(op->args[0] == sum && op->args[1] < i); const XrXirInstruction *one = &fn->instructions[op->args[1]]; CHECK(one->op == XR_XIR_CONST_INT && one->type == XR_XIR_I64 && one->immediate == 1); }
            sum = i; ++adds;
        }
        if (op->op == XR_XIR_RETURN) {
            ++returns; CHECK(op->args[0] < i);
            if (returns == 1) CHECK(adds == 2 && op->args[0] == sum);
            else { const XrXirInstruction *zero = &fn->instructions[op->args[0]]; CHECK(zero->op == XR_XIR_CONST_INT && zero->type == XR_XIR_I64 && !zero->immediate); }
        }
    }
    CHECK(calls == 3 && adds == 2 && branches == 1 && returns == 2 && integer != boolean);
}
#endif // GENERICS_SOURCE_SHAPE_H
