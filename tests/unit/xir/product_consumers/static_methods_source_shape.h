/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * static_methods_source_shape.h - Same-named static methods retain distinct owners
 *
 * KEY CONCEPT:
 *   Nominal owners distinguish two receiver-free methods with the same name.
 */
#ifndef STATIC_METHODS_SOURCE_SHAPE_H
#define STATIC_METHODS_SOURCE_SHAPE_H
#include "xir/xxir_types.h"
#include "xir/xxir_nominal.h"
static void static_methods_source_shape(const XrXirModule *m) {
    CHECK(m && m->declarations && m->types && m->types->nominals && m->types->nominals->count == 2);
    const char *names[] = {"First", "Second"};
    uint32_t methods[] = {UINT32_MAX, UINT32_MAX}, answer = UINT32_MAX;
    for (uint32_t n = 0; n < 2; ++n) {
        const XrXirNominalDeclaration *nominal = &m->types->nominals->declarations[n];
        CHECK(nominal->kind == XR_XIR_NOMINAL_CLASS && !nominal->field_count && !nominal->parameter_count);
        CHECK(nominal->name.length == strlen(names[n]) && !memcmp(nominal->name.bytes, names[n], nominal->name.length));
    }
    for (uint32_t f = 0; f < m->function_count; ++f) {
        const XrXirFunction *fn = &m->functions[f];
        const XrXirFunctionIdentity *identity = &m->declarations->functions[f];
        if (identity->method_kind == XR_XIR_STATIC_METHOD) {
            CHECK(identity->nominal_owner == 1 || identity->nominal_owner == 2);
            uint32_t owner = identity->nominal_owner - 1;
            CHECK(methods[owner] == UINT32_MAX && !fn->parameter_count && fn->result == XR_XIR_I64);
            CHECK(fn->name_length == 5 && !memcmp(fn->name, "value", 5) && identity->member_access == XR_XIR_MEMBER_PUBLIC);
            unsigned constants = 0, returns = 0; uint32_t value = UINT32_MAX;
            for (uint32_t i = 0; i < fn->instruction_count; ++i) {
                const XrXirInstruction *op = &fn->instructions[i];
                if (op->op == XR_XIR_CONST_INT) {
                    ++constants; CHECK(op->type == XR_XIR_I64 && op->immediate == (owner ? 23u : 19u)); value = i;
                }
                if (op->op == XR_XIR_RETURN) { ++returns; CHECK(value != UINT32_MAX && op->args[0] == value); }
            }
            CHECK(constants == 1 && returns == 1); methods[owner] = f;
        }
        if (fn->name_length == 6 && !memcmp(fn->name, "answer", 6)) { CHECK(answer == UINT32_MAX); answer = f; }
    }
    CHECK(methods[0] != UINT32_MAX && methods[1] != UINT32_MAX && methods[0] != methods[1] && answer != UINT32_MAX);
    const XrXirFunction *fn = &m->functions[answer];
    CHECK(!fn->parameter_count && fn->result == XR_XIR_I64 && !m->declarations->functions[answer].exported);
    unsigned calls = 0, adds = 0, returns = 0; uint32_t values[] = {UINT32_MAX, UINT32_MAX}, sum = UINT32_MAX;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_CALL) {
            CHECK(calls < 2 && op->type == XR_XIR_I64 && !op->args[1] && op->immediate == methods[calls]);
            values[calls++] = i;
        }
        if (op->op == XR_XIR_ADD_INT) {
            ++adds; CHECK(calls == 2 && op->type == XR_XIR_I64 && op->args[0] == values[0] && op->args[1] == values[1]); sum = i;
        }
        if (op->op == XR_XIR_RETURN) { ++returns; CHECK(sum != UINT32_MAX && op->args[0] == sum); }
    }
    CHECK(calls == 2 && adds == 1 && returns == 1);
}
#endif // STATIC_METHODS_SOURCE_SHAPE_H
