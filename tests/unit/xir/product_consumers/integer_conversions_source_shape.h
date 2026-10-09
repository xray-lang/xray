/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * integer_conversions_source_shape.h - Exact module constants and integer casts
 *
 * KEY CONCEPT:
 *   The unsigned byte and full-width bit pattern retain their declared types
 *   through module initialization, calls and detached Checked ownership.
 */
#ifndef INTEGER_CONVERSIONS_SOURCE_SHAPE_H
#define INTEGER_CONVERSIONS_SOURCE_SHAPE_H
#include "xir/xxir_types.h"
static uint32_t integer_conversions_helper(const XrXirModule *m, const char *name, XrXirType input) {
    uint32_t found = UINT32_MAX;
    for (uint32_t f = 0; f < m->function_count; ++f) {
        const XrXirFunction *fn = &m->functions[f];
        if (fn->name_length != strlen(name) || memcmp(fn->name, name, fn->name_length)) continue;
        CHECK(found == UINT32_MAX && fn->parameter_count == 1 && fn->parameters[0] == input && fn->result == XR_XIR_I64);
        CHECK(!m->declarations->functions[f].exported && !m->declarations->functions[f].nominal_owner);
        unsigned conversions = 0, returns = 0; uint32_t value = UINT32_MAX;
        for (uint32_t i = 0; i < fn->instruction_count; ++i) {
            const XrXirInstruction *op = &fn->instructions[i];
            if (op->op == XR_XIR_CONVERT_NUMBER) {
                ++conversions; CHECK(op->type == XR_XIR_I64 && !op->args[0]); value = i + 1;
            }
            if (op->op == XR_XIR_RETURN) { ++returns; CHECK(value != UINT32_MAX && op->args[0] == value); }
        }
        CHECK(conversions == 1 && returns == 1); found = f;
    }
    CHECK(found != UINT32_MAX); return found;
}
static void integer_conversions_source_shape(const XrXirModule *m) {
    CHECK(m && m->declarations && m->declarations->slot_count == 2);
    const XrXirDeclarations *d = m->declarations;
    uint32_t byte_slot = UINT32_MAX, high_slot = UINT32_MAX;
    for (uint32_t s = 0; s < d->slot_count; ++s) {
        CHECK(d->slots[s].module == d->root_module && d->slots[s].mutable == 1);
        if (d->slots[s].type == XR_XIR_U8) { CHECK(byte_slot == UINT32_MAX); byte_slot = s; }
        else { CHECK(d->slots[s].type == XR_XIR_U64 && high_slot == UINT32_MAX); high_slot = s; }
    }
    CHECK(byte_slot != UINT32_MAX && high_slot != UINT32_MAX);
    uint32_t init_index = d->modules[d->root_module].initializer;
    CHECK(init_index < m->function_count);
    const XrXirFunction *init = &m->functions[init_index]; CHECK(!init->parameter_count && init->result == XR_XIR_UNIT);
    unsigned byte_inits = 0, high_inits = 0, conversions = 0;
    for (uint32_t i = 0; i < init->instruction_count; ++i) {
        const XrXirInstruction *op = &init->instructions[i];
        if (op->op == XR_XIR_CONVERT_NUMBER) {
            ++conversions; CHECK(op->type == XR_XIR_U64 && op->args[0] < i);
            const XrXirInstruction *value = &init->instructions[op->args[0]];
            CHECK(value->op == XR_XIR_CONST_INT && value->type == XR_XIR_I64 && value->immediate == -1);
        }
        if (op->op == XR_XIR_SLOT_INIT) {
            CHECK(op->type == XR_XIR_UNIT && op->args[0] < i);
            const XrXirInstruction *value = &init->instructions[op->args[0]];
            if (op->immediate == byte_slot) {
                ++byte_inits; CHECK(value->op == XR_XIR_CONST_INT && value->type == XR_XIR_U8 && value->immediate == 255);
            } else {
                ++high_inits; CHECK(op->immediate == high_slot && value->op == XR_XIR_CONVERT_NUMBER && value->type == XR_XIR_U64);
            }
        }
    }
    CHECK(byte_inits == 1 && high_inits == 1 && conversions == 1);
    uint32_t widen = integer_conversions_helper(m, "widen", XR_XIR_U8);
    uint32_t bits = integer_conversions_helper(m, "bits", XR_XIR_U64); CHECK(widen != bits);
    const XrXirFunction *answer = NULL;
    for (uint32_t f = 0; f < m->function_count; ++f) {
        const XrXirFunction *fn = &m->functions[f];
        if (fn->name_length == 6 && !memcmp(fn->name, "answer", 6)) {
            CHECK(!answer && !fn->parameter_count && fn->result == XR_XIR_I64 && !d->functions[f].exported); answer = fn;
        }
    }
    CHECK(answer); unsigned calls = 0, sums = 0, loads = 0, returns = 0;
    uint32_t results[] = {UINT32_MAX, UINT32_MAX}, sum = UINT32_MAX;
    for (uint32_t i = 0; i < answer->instruction_count; ++i) {
        const XrXirInstruction *op = &answer->instructions[i];
        CHECK(op->op != XR_XIR_SLOT_INIT && op->op != XR_XIR_SLOT_STORE);
        if (op->op == XR_XIR_SLOT_LOAD) {
            ++loads; CHECK((op->immediate == byte_slot && op->type == XR_XIR_U8) ||
                (op->immediate == high_slot && op->type == XR_XIR_U64));
        }
        if (op->op == XR_XIR_CALL) {
            CHECK(calls < 2 && op->immediate == (calls ? bits : widen) && op->type == XR_XIR_I64 &&
                op->args[1] == 1 && op->args[0] < answer->operand_count);
            uint32_t argument = answer->operands[op->args[0]]; CHECK(argument < i);
            const XrXirInstruction *value = &answer->instructions[argument];
            CHECK(value->op == XR_XIR_SLOT_LOAD && value->immediate == (calls ? high_slot : byte_slot) &&
                value->type == (calls ? XR_XIR_U64 : XR_XIR_U8)); results[calls++] = i;
        }
        if (op->op == XR_XIR_ADD_INT) {
            CHECK(calls == 2 && sums < 2 && op->type == XR_XIR_I64);
            if (!sums) CHECK(op->args[0] == results[0] && op->args[1] == results[1]);
            else {
                CHECK(op->args[0] == sum && op->args[1] < i); const XrXirInstruction *one = &answer->instructions[op->args[1]];
                CHECK(one->op == XR_XIR_CONST_INT && one->type == XR_XIR_I64 && one->immediate == 1);
            }
            ++sums; sum = i;
        }
        if (op->op == XR_XIR_RETURN) { ++returns; CHECK(sums == 2 && op->args[0] == sum); }
    }
    CHECK(calls == 2 && sums == 2 && loads == 2 && returns == 1);
}
#endif // INTEGER_CONVERSIONS_SOURCE_SHAPE_H
