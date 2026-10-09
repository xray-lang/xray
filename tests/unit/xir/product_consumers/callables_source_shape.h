/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * callables_source_shape.h - Original named and anonymous callable identities
 *
 * KEY CONCEPT:
 *   The original function references remain typed owned descriptors after
 *   producers die; indirect calls consume the actual callable parameter.
 */
#ifndef CALLABLES_SOURCE_SHAPE_H
#define CALLABLES_SOURCE_SHAPE_H
#include "xir/xxir_types.h"
static void callables_source_signature(const XrXirModule *m, XrXirType type) {
    const XrXirTypeNode *s = xr_xir_callable_signature(m->types, type);
    CHECK(s && s->parameter_count == 1 && s->parameters[0].type == XR_XIR_I64 && s->result == XR_XIR_I64);
}
static uint32_t callables_source_named(const XrXirModule *m, const char *name) {
    uint32_t id = UINT32_MAX;
    for (uint32_t f = 0; f < m->function_count; ++f) {
        const XrXirFunction *fn = &m->functions[f];
        if (fn->name_length != strlen(name) || memcmp(fn->name, name, fn->name_length)) continue;
        CHECK(id == UINT32_MAX && fn->result == XR_XIR_I64 && !m->declarations->functions[f].exported); id = f;
    }
    CHECK(id != UINT32_MAX); return id;
}
static void callables_source_math(const XrXirModule *m, uint32_t id, bool twice) {
    CHECK(id < m->function_count); const XrXirFunction *fn = &m->functions[id];
    CHECK(fn->parameter_count == 1 && fn->parameters[0] == XR_XIR_I64 && fn->result == XR_XIR_I64);
    unsigned constants = 0, arithmetic = 0, returns = 0; uint32_t literal = UINT32_MAX, result = UINT32_MAX;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_CONST_INT) { ++constants; CHECK(op->type == XR_XIR_I64 && op->immediate == (twice ? 2 : 1)); literal = fn->parameter_count + i; }
        if (op->op == XR_XIR_MUL_INT || op->op == XR_XIR_ADD_INT) {
            ++arithmetic; CHECK(op->op == (twice ? XR_XIR_MUL_INT : XR_XIR_ADD_INT) && op->type == XR_XIR_I64);
            CHECK(literal != UINT32_MAX && !op->args[0] && op->args[1] == literal); result = fn->parameter_count + i;
        }
        if (op->op == XR_XIR_RETURN) { ++returns; CHECK(result != UINT32_MAX && op->args[0] == result); }
    }
    CHECK(constants == 1 && arithmetic == 1 && returns == 1);
}
static uint32_t callables_source_reference(const XrXirModule *m, const XrXirFunction *fn, uint32_t id, uint32_t before) {
    for (uint32_t step = 0; step < fn->instruction_count; ++step) {
        CHECK(!fn->parameter_count && id < before); const XrXirInstruction *op = &fn->instructions[id];
        callables_source_signature(m, op->type);
        if (op->op == XR_XIR_FUNCTION_REF) { CHECK(op->immediate >= 0 && (uint64_t)op->immediate < m->function_count); return (uint32_t)op->immediate; }
        CHECK(op->op == XR_XIR_FUNCTION_WEAKEN); before = id; id = op->args[0];
    }
    CHECK(false); return UINT32_MAX;
}
static void callables_source_shape(const XrXirModule *m) {
    CHECK(m && m->types && m->declarations);
    uint32_t apply = callables_source_named(m, "apply"), twice = callables_source_named(m, "twice"), answer = callables_source_named(m, "answer");
    CHECK(apply != twice && apply != answer && twice != answer); callables_source_math(m, twice, true);
    const XrXirFunction *fn = &m->functions[apply];
    CHECK(fn->parameter_count == 2 && fn->parameters[0] == XR_XIR_I64); callables_source_signature(m, fn->parameters[1]);
    unsigned indirect = 0, returns = 0; uint32_t result = UINT32_MAX;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_CALL_INDIRECT) {
            ++indirect; CHECK(op->type == XR_XIR_I64 && op->immediate == 1 && op->args[1] == 1 && op->args[0] < fn->operand_count);
            CHECK(!fn->operands[op->args[0]]); result = fn->parameter_count + i;
        }
        if (op->op == XR_XIR_RETURN) { ++returns; CHECK(result != UINT32_MAX && op->args[0] == result); }
    }
    CHECK(indirect == 1 && returns == 1); fn = &m->functions[answer]; CHECK(!fn->parameter_count);
    uint32_t values[2] = {UINT32_MAX, UINT32_MAX}, sum = UINT32_MAX, anonymous = UINT32_MAX;
    unsigned calls = 0, adds = 0; returns = 0;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_CALL) {
            CHECK(calls < 2 && op->immediate == apply && op->type == XR_XIR_I64 && op->args[1] == 2);
            CHECK(op->args[0] <= fn->operand_count && fn->operand_count - op->args[0] >= 2);
            const uint32_t *args = fn->operands + op->args[0]; CHECK(args[0] < i);
            const XrXirInstruction *literal = &fn->instructions[args[0]]; CHECK(literal->op == XR_XIR_CONST_INT && literal->type == XR_XIR_I64 && literal->immediate == (calls ? 5 : 21));
            CHECK(xr_xir_operand_type(fn, args[1]) == m->functions[apply].parameters[1]);
            uint32_t reference = callables_source_reference(m, fn, args[1], i);
            if (!calls) CHECK(reference == twice);
            else { CHECK(reference != twice && reference != apply && reference != answer); anonymous = reference; callables_source_math(m, reference, false); }
            values[calls++] = i;
        }
        if (op->op == XR_XIR_ADD_INT) { ++adds; CHECK(calls == 2 && op->type == XR_XIR_I64 && op->args[0] == values[0] && op->args[1] == values[1]); sum = i; }
        if (op->op == XR_XIR_RETURN) { ++returns; CHECK(sum != UINT32_MAX && op->args[0] == sum); }
    }
    CHECK(calls == 2 && adds == 1 && returns == 1 && anonymous != UINT32_MAX);
}
#endif // CALLABLES_SOURCE_SHAPE_H
