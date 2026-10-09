/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * original_arrays_source_shape.h - Original ordered and runtime-sized Arrays
 *
 * KEY CONCEPT:
 *   Original narrow elements and runtime counts retain their actual call SSA
 *   identities; typed default values are checked independently of execution.
 */
#ifndef ORIGINAL_ARRAYS_SOURCE_SHAPE_H
#define ORIGINAL_ARRAYS_SOURCE_SHAPE_H
#include "xir/xxir_types.h"
static unsigned original_arrays_family(const char *name) {
    return !strcmp(name, "narrow_array") ? 1u : !strcmp(name, "array_runtime_length") ? 2u : 0u;
}
static uint32_t original_arrays_function(const XrXirModule *m, const char *name, XrXirType result) {
    uint32_t found = UINT32_MAX;
    for (uint32_t f = 0; f < m->function_count; ++f) {
        const XrXirFunction *fn = &m->functions[f];
        if (fn->name_length != strlen(name) || memcmp(fn->name, name, fn->name_length)) continue;
        CHECK(found == UINT32_MAX && !fn->parameter_count && fn->result == result && !m->declarations->functions[f].exported);
        found = f;
    }
    CHECK(found != UINT32_MAX); return found;
}
static void original_arrays_narrow_shape(const XrXirModule *m, uint32_t helper, uint32_t answer) {
    const XrXirDeclarations *d = m->declarations; CHECK(d->slot_count == 2);
    uint32_t visits = UINT32_MAX;
    for (uint32_t s = 0; s < d->slot_count; ++s) {
        CHECK(d->slots[s].module == d->root_module);
        if (d->slots[s].type == XR_XIR_I64) { CHECK(visits == UINT32_MAX && d->slots[s].mutable); visits = s; }
        else CHECK(!d->slots[s].mutable && xr_xir_array_element(m->types, d->slots[s].type) == XR_XIR_U8);
    }
    CHECK(visits != UINT32_MAX); const XrXirFunction *next = &m->functions[helper];
    unsigned stores = 0, conversions = 0;
    for (uint32_t i = 0; i < next->instruction_count; ++i) {
        const XrXirInstruction *op = &next->instructions[i];
        if (op->op == XR_XIR_SLOT_STORE) {
            ++stores; CHECK(op->immediate == visits && op->args[0] < i);
            const XrXirInstruction *add = &next->instructions[op->args[0]];
            CHECK(add->op == XR_XIR_ADD_INT && add->type == XR_XIR_I64 && add->args[0] < op->args[0] && add->args[1] < op->args[0]);
            const XrXirInstruction *load = &next->instructions[add->args[0]], *one = &next->instructions[add->args[1]];
            CHECK(load->op == XR_XIR_SLOT_LOAD && load->immediate == visits && load->type == XR_XIR_I64);
            CHECK(one->op == XR_XIR_CONST_INT && one->type == XR_XIR_I64 && one->immediate == 1);
        }
        if (op->op == XR_XIR_CONVERT_NUMBER) { ++conversions; CHECK(op->type == XR_XIR_U8 && xr_xir_operand_type(next, op->args[0]) == XR_XIR_I64); }
    }
    CHECK(stores == 1 && conversions == 1);
    const XrXirFunction *fn = &m->functions[answer]; uint32_t results[3]; unsigned calls = 0, ordered = 0, signed_arrays = 0;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_CALL && op->immediate == helper) { CHECK(calls < 3 && op->type == XR_XIR_U8 && !op->args[1]); results[calls++] = i; }
        if (op->op != XR_XIR_ARRAY_NEW) continue;
        CHECK(op->args[0] <= fn->operand_count && op->args[1] <= fn->operand_count - op->args[0]);
        XrXirType type = xr_xir_array_element(m->types, op->type);
        if (type == XR_XIR_U8) {
            ++ordered; CHECK(calls == 3 && op->args[1] == 3);
            for (unsigned a = 0; a < 3; ++a) CHECK(fn->operands[op->args[0] + a] == results[a] && results[a] < i);
        } else {
            ++signed_arrays; CHECK(type == XR_XIR_I8 && op->args[1] == 2);
            for (unsigned a = 0; a < 2; ++a) {
                uint32_t id = fn->operands[op->args[0] + a]; CHECK(id < i);
                const XrXirInstruction *value = &fn->instructions[id];
                CHECK(value->op == XR_XIR_CONST_INT && value->type == XR_XIR_I8 && value->immediate == (a ? 127 : -128));
            }
        }
    }
    CHECK(calls == 3 && ordered == 1 && signed_arrays == 1);
}
static void original_arrays_runtime_shape(const XrXirModule *m, uint32_t helper, uint32_t answer) {
    const XrXirFunction *length = &m->functions[helper]; unsigned threes = 0, returns = 0; uint32_t three = UINT32_MAX;
    for (uint32_t i = 0; i < length->instruction_count; ++i) {
        const XrXirInstruction *op = &length->instructions[i];
        if (op->op == XR_XIR_CONST_INT) { ++threes; CHECK(op->type == XR_XIR_I64 && op->immediate == 3); three = i; }
        if (op->op == XR_XIR_RETURN) { ++returns; CHECK(three != UINT32_MAX && op->args[0] == three); }
    }
    CHECK(threes == 1 && returns == 1);
    const XrXirFunction *fn = &m->functions[answer]; const XrXirType types[] = {XR_XIR_U8, XR_XIR_BOOL, XR_XIR_F64};
    unsigned calls = 0, repeats = 0, sets = 0; uint32_t counts[3];
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_CALL && op->immediate == helper) { CHECK(calls < 3 && op->type == XR_XIR_I64 && !op->args[1]); counts[calls++] = i; }
        if (op->op == XR_XIR_ARRAY_REPEAT) {
            CHECK(repeats < 3 && calls == repeats + 1 && xr_xir_array_element(m->types, op->type) == types[repeats]);
            CHECK(op->args[0] == counts[repeats] && op->args[1] < i); const XrXirInstruction *zero = &fn->instructions[op->args[1]];
            CHECK(zero->type == types[repeats] && !zero->immediate && zero->op ==
                (!repeats ? XR_XIR_CONST_INT : repeats == 1 ? XR_XIR_CONST_BOOL : XR_XIR_CONST_FLOAT)); ++repeats;
        }
        if (op->op == XR_XIR_ARRAY_SET) {
            ++sets; CHECK(op->type == XR_XIR_UNIT && op->args[1] == 3 && op->args[0] <= fn->operand_count && fn->operand_count - op->args[0] >= 3);
            const uint32_t *args = fn->operands + op->args[0]; CHECK(args[0] < i && args[1] < i && args[2] < i);
            const XrXirInstruction *place = &fn->instructions[args[0]], *index = &fn->instructions[args[1]], *value = &fn->instructions[args[2]];
            CHECK(place->op == XR_XIR_CELL_PLACE && xr_xir_array_element(m->types, place->type) == XR_XIR_U8);
            CHECK(index->op == XR_XIR_CONST_INT && index->type == XR_XIR_I64 && index->immediate == 1);
            CHECK(value->op == XR_XIR_CONST_INT && value->type == XR_XIR_U8 && value->immediate == 42);
        }
    }
    CHECK(calls == 3 && repeats == 3 && sets == 1);
}
static void original_arrays_source_shape(const XrXirModule *m, unsigned family) {
    CHECK(m && m->declarations && m->types && (family == 1 || family == 2));
    uint32_t answer = original_arrays_function(m, "answer", XR_XIR_I64);
    uint32_t helper = original_arrays_function(m, family == 1 ? "next" : "length", family == 1 ? XR_XIR_U8 : XR_XIR_I64);
    CHECK(answer != helper);
    if (family == 1) original_arrays_narrow_shape(m, helper, answer);
    else original_arrays_runtime_shape(m, helper, answer);
}
#endif // ORIGINAL_ARRAYS_SOURCE_SHAPE_H
