/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * constructor_folding_source_shape.h - Original constructor field operands
 *
 * KEY CONCEPT:
 *   Actual field operands preserve literal initialization, declaration order
 *   and crossed parameters rather than merely finding unrelated constants.
 */
#ifndef CONSTRUCTOR_FOLDING_SOURCE_SHAPE_H
#define CONSTRUCTOR_FOLDING_SOURCE_SHAPE_H
#include "xir/xxir_types.h"
#include "xir/xxir_nominal.h"
typedef struct ConstructorFoldingIds { uint32_t constructors[3], answer; XrXirType classes[3]; } ConstructorFoldingIds;
static void constructor_folding_literal(const XrXirFunction *fn, uint32_t id, uint32_t before, int64_t expected) {
    CHECK(id >= fn->parameter_count && id - fn->parameter_count < before);
    const XrXirInstruction *op = &fn->instructions[id - fn->parameter_count];
    CHECK(op->op == XR_XIR_CONST_INT && op->type == XR_XIR_I64 && op->immediate == expected);
}
static uint32_t constructor_folding_field_value(const XrXirFunction *fn, uint32_t id, uint32_t before) {
    CHECK(id >= fn->parameter_count && id - fn->parameter_count < before);
    uint32_t read = id - fn->parameter_count; const XrXirInstruction *op = &fn->instructions[read];
    CHECK(op->op == XR_XIR_LOCAL_READ && op->args[0] >= fn->parameter_count && op->args[0] - fn->parameter_count < read);
    const XrXirInstruction *place = &fn->instructions[op->args[0] - fn->parameter_count];
    CHECK(place->op == XR_XIR_LOCAL_UNINIT && place->type == op->type);
    for (uint32_t i = read; i; --i) {
        const XrXirInstruction *write = &fn->instructions[i - 1];
        if (write->op == XR_XIR_LOCAL_WRITE && write->args[0] == op->args[0]) return write->args[1];
    }
    CHECK(false); return UINT32_MAX;
}
static void constructor_folding_constructor(const XrXirModule *m, uint32_t id, unsigned n) {
    const XrXirFunction *fn = &m->functions[id]; CHECK(fn->parameter_count == n && xr_xir_type_is_class(m->types, fn->result));
    for (uint32_t p = 0; p < n; ++p) CHECK(fn->parameters[p] == XR_XIR_I64);
    unsigned news = 0, returns = 0, places = 0, writes = 0; uint32_t local[2] = {UINT32_MAX, UINT32_MAX}; uint32_t value = UINT32_MAX;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_LOCAL_UNINIT) {
            CHECK(places < (n ? 2u : 1u) && op->type == (n == 1 && places == 1 ? XR_XIR_BOOL : XR_XIR_I64));
            local[places++] = fn->parameter_count + i;
        }
        if (op->op == XR_XIR_LOCAL_WRITE) {
            CHECK(writes < (n ? 2u : 1u) && op->args[0] == local[n ? 1u - writes : 0u]); ++writes;
        }
        if (op->op == XR_XIR_CLASS_NEW) {
            ++news; CHECK(op->type == fn->result && op->args[1] == (n ? 2u : 1u));
            CHECK(op->args[0] <= fn->operand_count && fn->operand_count - op->args[0] >= op->args[1]);
            uint32_t fields[2] = {UINT32_MAX, UINT32_MAX};
            for (uint32_t f = 0; f < op->args[1]; ++f) fields[f] = constructor_folding_field_value(fn, fn->operands[op->args[0] + f], i);
            if (!n) constructor_folding_literal(fn, fields[0], i, 0);
            else if (n == 1) {
                CHECK(!fields[0] && fields[1] >= fn->parameter_count && fields[1] - fn->parameter_count < i);
                const XrXirInstruction *truth = &fn->instructions[fields[1] - fn->parameter_count];
                CHECK(truth->op == XR_XIR_CONST_BOOL && truth->type == XR_XIR_BOOL && truth->immediate == 1);
            } else CHECK(n == 2 && fields[0] == 1 && !fields[1]);
            value = fn->parameter_count + i;
        }
        if (op->op == XR_XIR_RETURN) { ++returns; CHECK(value != UINT32_MAX && op->args[0] == value); }
    }
    CHECK(news == 1 && returns == 1 && places == (n ? 2u : 1u) && writes == places);
}
static ConstructorFoldingIds constructor_folding_ids(const XrXirModule *m) {
    CHECK(m && m->types && m->types->nominals && m->declarations && m->types->nominals->count == 3);
    ConstructorFoldingIds ids = {{UINT32_MAX, UINT32_MAX, UINT32_MAX}, UINT32_MAX, {0}};
    const char *names[] = {"Counter", "Pair", "Swap"}, *fields[3][2] = {{"value", NULL}, {"a", "b"}, {"first", "second"}};
    for (uint32_t n = 0; n < 3; ++n) {
        const XrXirNominalDeclaration *d = &m->types->nominals->declarations[n];
        CHECK(d->kind == XR_XIR_NOMINAL_CLASS && !d->parameter_count && d->field_count == (n ? 2u : 1u));
        CHECK(d->name.length == strlen(names[n]) && !memcmp(d->name.bytes, names[n], d->name.length));
        for (uint32_t f = 0; f < d->field_count; ++f) {
            CHECK(d->fields[f].name.length == strlen(fields[n][f]) && !memcmp(d->fields[f].name.bytes, fields[n][f], d->fields[f].name.length));
            CHECK(d->fields[f].type == (n == 1 && f == 1 ? XR_XIR_BOOL : XR_XIR_I64));
        }
    }
    for (uint32_t f = 0; f < m->function_count; ++f) {
        const XrXirFunction *fn = &m->functions[f]; const XrXirFunctionIdentity *d = &m->declarations->functions[f];
        if (d->method_kind == XR_XIR_CONSTRUCTOR) {
            CHECK(d->nominal_owner >= 1 && d->nominal_owner <= 3); uint32_t n = d->nominal_owner - 1;
            CHECK(ids.constructors[n] == UINT32_MAX); ids.constructors[n] = f; ids.classes[n] = fn->result;
            constructor_folding_constructor(m, f, n);
        }
        if (fn->name_length == 6 && !memcmp(fn->name, "answer", 6)) { CHECK(ids.answer == UINT32_MAX && !d->exported); ids.answer = f; }
    }
    CHECK(ids.answer != UINT32_MAX); for (uint32_t n = 0; n < 3; ++n) CHECK(ids.constructors[n] != UINT32_MAX);
    return ids;
}
static uint32_t constructor_folding_origin(const XrXirFunction *fn, uint32_t id, uint32_t before) {
    for (uint32_t step = 0; step < fn->instruction_count; ++step) {
        CHECK(!fn->parameter_count && id < before); const XrXirInstruction *op = &fn->instructions[id];
        if (op->op != XR_XIR_CELL_READ && op->op != XR_XIR_CELL_NEW) return id;
        before = id; id = op->args[0];
    }
    CHECK(false); return UINT32_MAX;
}
static void constructor_folding_source_shape(const XrXirModule *m) {
    ConstructorFoldingIds ids = constructor_folding_ids(m); const XrXirFunction *fn = &m->functions[ids.answer];
    CHECK(!fn->parameter_count && fn->result == XR_XIR_I64);
    uint32_t objects[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX}, fields[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    uint32_t sums[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX}, multiply = UINT32_MAX;
    unsigned calls = 0, gets = 0, adds = 0, multiplies = 0, returns = 0;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_CALL) {
            CHECK(calls < 3 && op->immediate == ids.constructors[calls] && op->type == ids.classes[calls] && op->args[1] == calls);
            CHECK(op->args[0] <= fn->operand_count && fn->operand_count - op->args[0] >= op->args[1]);
            for (uint32_t p = 0; p < op->args[1]; ++p) constructor_folding_literal(fn, fn->operands[op->args[0] + p], i, calls == 1 ? 5 : p ? 10 : 1);
            objects[calls++] = i;
        }
        if (op->op == XR_XIR_CLASS_GET) {
            CHECK(gets < 4 && op->type == XR_XIR_I64); uint32_t n = gets < 2 ? gets : 2;
            CHECK(objects[n] != UINT32_MAX && op->immediate == (gets == 3 ? 1 : 0));
            CHECK(xr_xir_operand_type(fn, op->args[0]) == ids.classes[n] && constructor_folding_origin(fn, op->args[0], i) == objects[n]); fields[gets++] = i;
        }
        if (op->op == XR_XIR_MUL_INT) {
            ++multiplies; CHECK(gets == 3 && op->type == XR_XIR_I64 && op->args[0] == fields[2]); constructor_folding_literal(fn, op->args[1], i, 100); multiply = i;
        }
        if (op->op == XR_XIR_ADD_INT) {
            CHECK(adds < 3 && op->type == XR_XIR_I64);
            CHECK(op->args[0] == (adds ? sums[adds - 1] : fields[0]) && op->args[1] == (!adds ? fields[1] : adds == 1 ? multiply : fields[3]));
            CHECK(op->args[0] != UINT32_MAX && op->args[1] != UINT32_MAX); sums[adds++] = i;
        }
        if (op->op == XR_XIR_RETURN) { ++returns; CHECK(adds == 3 && op->args[0] == sums[2]); }
    }
    CHECK(calls == 3 && gets == 4 && adds == 3 && multiplies == 1 && returns == 1);
}
#endif // CONSTRUCTOR_FOLDING_SOURCE_SHAPE_H
