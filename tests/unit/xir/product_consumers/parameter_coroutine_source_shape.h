/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * parameter_coroutine_source_shape.h - A captured suspended body keeps its value
 *
 * KEY CONCEPT:
 *   The same captured callable reaches both original calls, with its captured
 *   factor preceding the ordinary argument in the owned function parameters.
 */
#ifndef PARAMETER_COROUTINE_SOURCE_SHAPE_H
#define PARAMETER_COROUTINE_SOURCE_SHAPE_H
#include "xir/xxir_types.h"
static void parameter_coroutine_signature(const XrXirModule *m, XrXirType type) {
    const XrXirTypeNode *s = xr_xir_callable_signature(m->types, type);
    CHECK(s && s->parameter_count == 1 && s->parameters[0].type == XR_XIR_I64 && s->result == XR_XIR_I64);
}
static uint32_t parameter_coroutine_named(const XrXirModule *m, const char *name) {
    uint32_t id = UINT32_MAX;
    for (uint32_t f = 0; f < m->function_count; ++f) {
        const XrXirFunction *fn = &m->functions[f];
        if (fn->name_length != strlen(name) || memcmp(fn->name, name, fn->name_length)) continue;
        CHECK(id == UINT32_MAX && fn->result == XR_XIR_I64 && !m->declarations->functions[f].exported); id = f;
    }
    CHECK(id != UINT32_MAX); return id;
}
static uint32_t parameter_coroutine_reference(const XrXirModule *m, const XrXirFunction *fn, uint32_t id, uint32_t before) {
    for (uint32_t step = 0; step < fn->instruction_count; ++step) {
        CHECK(!fn->parameter_count && id < before); const XrXirInstruction *op = &fn->instructions[id];
        parameter_coroutine_signature(m, op->type);
        if (op->op == XR_XIR_FUNCTION_REF) return id;
        CHECK(op->op == XR_XIR_FUNCTION_WEAKEN); before = id; id = op->args[0];
    }
    CHECK(false); return UINT32_MAX;
}
static void parameter_coroutine_child(const XrXirModule *m, uint32_t id) {
    CHECK(id < m->function_count); const XrXirFunction *fn = &m->functions[id];
    CHECK(fn->parameter_count == 2 && fn->parameters[0] == XR_XIR_I64 && fn->parameters[1] == XR_XIR_I64 && fn->result == XR_XIR_I64);
    CHECK(!(m->declarations->functions[id].promises & XR_XIR_FUNCTION_NO_SUSPEND));
    unsigned suspends = 0, multiplies = 0, returns = 0; uint32_t result = UINT32_MAX;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_SUSPEND) { ++suspends; CHECK(!multiplies && op->type == XR_XIR_UNIT); }
        if (op->op == XR_XIR_MUL_INT) { ++multiplies; CHECK(suspends == 1 && op->type == XR_XIR_I64 && op->args[0] == 1 && !op->args[1]); result = fn->parameter_count + i; }
        if (op->op == XR_XIR_RETURN) { ++returns; CHECK(result != UINT32_MAX && op->args[0] == result); }
    }
    CHECK(suspends == 1 && multiplies == 1 && returns == 1);
}
static void parameter_coroutine_source_shape(const XrXirModule *m) {
    CHECK(m && m->types && m->declarations);
    uint32_t apply = parameter_coroutine_named(m, "apply"), answer = parameter_coroutine_named(m, "answer"); CHECK(apply != answer);
    const XrXirFunction *fn = &m->functions[apply];
    CHECK(fn->parameter_count == 2 && fn->parameters[0] == XR_XIR_I64); parameter_coroutine_signature(m, fn->parameters[1]);
    unsigned calls = 0, returns = 0; uint32_t result = UINT32_MAX;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_CALL_INDIRECT) {
            ++calls; CHECK(op->type == XR_XIR_I64 && op->immediate == 1 && op->args[1] == 1 && op->args[0] < fn->operand_count);
            CHECK(!fn->operands[op->args[0]]); result = fn->parameter_count + i;
        }
        if (op->op == XR_XIR_RETURN) { ++returns; CHECK(result != UINT32_MAX && op->args[0] == result); }
    }
    CHECK(calls == 1 && returns == 1); fn = &m->functions[answer]; CHECK(!fn->parameter_count);
    uint32_t reference = UINT32_MAX, values[2] = {UINT32_MAX, UINT32_MAX}, sum = UINT32_MAX;
    unsigned direct = 0, indirect = 0, adds = 0; returns = 0;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_CALL || op->op == XR_XIR_CALL_INDIRECT) {
            bool first = op->op == XR_XIR_CALL; CHECK(op->type == XR_XIR_I64 && op->args[1] == (first ? 2u : 1u));
            CHECK(op->args[0] <= fn->operand_count && fn->operand_count - op->args[0] >= op->args[1]);
            const uint32_t *args = fn->operands + op->args[0]; CHECK(args[0] < i);
            const XrXirInstruction *literal = &fn->instructions[args[0]]; CHECK(literal->op == XR_XIR_CONST_INT && literal->type == XR_XIR_I64 && literal->immediate == (first ? 20 : 1));
            if (first) {
                CHECK(!direct++ && !indirect && op->immediate == apply); reference = parameter_coroutine_reference(m, fn, args[1], i); values[0] = i;
                const XrXirInstruction *packed = &fn->instructions[reference];
                CHECK(packed->immediate >= 0 && (uint64_t)packed->immediate < m->function_count && packed->immediate != apply && packed->immediate != answer);
                CHECK(packed->args[1] == 1 && packed->args[0] < fn->operand_count); uint32_t captured = fn->operands[packed->args[0]]; CHECK(captured < reference);
                const XrXirInstruction *factor = &fn->instructions[captured]; CHECK(factor->op == XR_XIR_CONST_INT && factor->type == XR_XIR_I64 && factor->immediate == 2);
                parameter_coroutine_child(m, (uint32_t)packed->immediate);
            } else {
                CHECK(direct == 1 && !indirect++ && op->immediate >= 0 && (uint64_t)op->immediate < i);
                CHECK(parameter_coroutine_reference(m, fn, (uint32_t)op->immediate, i) == reference); values[1] = i;
            }
        }
        if (op->op == XR_XIR_ADD_INT) { ++adds; CHECK(direct == 1 && indirect == 1 && op->type == XR_XIR_I64 && op->args[0] == values[0] && op->args[1] == values[1]); sum = i; }
        if (op->op == XR_XIR_RETURN) { ++returns; CHECK(sum != UINT32_MAX && op->args[0] == sum); }
    }
    CHECK(direct == 1 && indirect == 1 && adds == 1 && returns == 1);
}
#endif // PARAMETER_COROUTINE_SOURCE_SHAPE_H
