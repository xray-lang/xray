/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * class_alias_original_source_shape.h - Original class alias reference edges
 *
 * KEY CONCEPT:
 *   Cell reads retain the actual constructor identity across local aliases;
 *   mutation and self-assignment operands are checked without owner-bit claims.
 */
#ifndef CLASS_ALIAS_ORIGINAL_SOURCE_SHAPE_H
#define CLASS_ALIAS_ORIGINAL_SOURCE_SHAPE_H
#include "xir/xxir_types.h"
#include "xir/xxir_nominal.h"
static unsigned class_alias_original_family(const char *name) {
    const char *names[] = {"class_alias_escape", "class_field_self_assignment", "class_alias_borrow", "class_alias_transfer"};
    for (unsigned n = 0; n < 4; ++n) if (!strcmp(name, names[n])) return n + 1;
    return 0;
}
static const XrXirInstruction *class_alias_original_operand(const XrXirFunction *fn, uint32_t id, uint32_t before) {
    CHECK(!fn->parameter_count && id < before); return &fn->instructions[id];
}
static uint32_t class_alias_original_origin(const XrXirFunction *fn, uint32_t id, uint32_t before) {
    for (uint32_t step = 0; step < fn->instruction_count; ++step) {
        const XrXirInstruction *op = class_alias_original_operand(fn, id, before);
        if (op->op != XR_XIR_CELL_READ && op->op != XR_XIR_CELL_NEW) return id;
        before = id; id = op->args[0];
    }
    CHECK(false); return UINT32_MAX;
}
static int64_t class_alias_original_argument(const XrXirFunction *fn, const XrXirInstruction *call, uint32_t before) {
    CHECK(call->args[1] == 1 && call->args[0] < fn->operand_count);
    const XrXirInstruction *value = class_alias_original_operand(fn, fn->operands[call->args[0]], before);
    CHECK(value->op == XR_XIR_CONST_INT && value->type == XR_XIR_I64); return value->immediate;
}
static void class_alias_original_source_shape(const XrXirModule *m, unsigned family) {
    CHECK(m && m->types && m->types->nominals && m->declarations && family >= 1 && family <= 4);
    const XrXirNominalTable *nominals = m->types->nominals;
    CHECK(nominals->count == (family == 3 ? 1u : 2u));
    uint32_t constructors[2] = {UINT32_MAX, UINT32_MAX}, answer = UINT32_MAX;
    XrXirType classes[2] = {XR_XIR_UNIT, XR_XIR_UNIT};
    for (uint32_t n = 0; n < nominals->count; ++n) {
        const XrXirNominalDeclaration *d = &nominals->declarations[n];
        const char *name = n ? "Holder" : "Cell", *field = n ? "child" : "value";
        CHECK(d->kind == XR_XIR_NOMINAL_CLASS && !d->parameter_count && d->field_count == 1);
        CHECK(d->name.length == strlen(name) && !memcmp(d->name.bytes, name, d->name.length));
        CHECK(d->fields[0].name.length == strlen(field) && !memcmp(d->fields[0].name.bytes, field, d->fields[0].name.length));
        if (!n) CHECK(d->fields[0].type == XR_XIR_I64);
    }
    for (uint32_t f = 0; f < m->function_count; ++f) {
        const XrXirFunction *fn = &m->functions[f]; const XrXirFunctionIdentity *d = &m->declarations->functions[f];
        if (d->method_kind == XR_XIR_CONSTRUCTOR) {
            CHECK(d->nominal_owner >= 1 && d->nominal_owner <= nominals->count); uint32_t n = d->nominal_owner - 1;
            CHECK(constructors[n] == UINT32_MAX && fn->parameter_count == 1 && xr_xir_type_is_class(m->types, fn->result));
            constructors[n] = f; classes[n] = fn->result;
        }
        if (fn->name_length == 6 && !memcmp(fn->name, "answer", 6)) { CHECK(answer == UINT32_MAX && !d->exported); answer = f; }
    }
    CHECK(answer != UINT32_MAX && constructors[0] != UINT32_MAX && m->functions[constructors[0]].parameters[0] == XR_XIR_I64);
    if (family != 3) CHECK(constructors[1] != UINT32_MAX && m->functions[constructors[1]].parameters[0] == classes[0] && nominals->declarations[1].fields[0].type == classes[0]);
    const XrXirFunction *fn = &m->functions[answer]; CHECK(!fn->parameter_count && fn->result == XR_XIR_I64);
    uint32_t cell = UINT32_MAX, replacement = UINT32_MAX, holder = UINT32_MAX, child = UINT32_MAX, result = UINT32_MAX;
    unsigned cell_calls = 0, holder_calls = 0, scalar_sets = 0, child_sets = 0, gets = 0, returns = 0;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_CALL && op->immediate == constructors[0]) {
            CHECK(op->type == classes[0]); int64_t value = class_alias_original_argument(fn, op, i);
            if (!cell_calls) { CHECK(value == (family == 1 || family == 3 ? 1 : 42)); cell = i; }
            else { CHECK(family == 1 && cell_calls == 1 && value == 42); replacement = i; }
            ++cell_calls;
        }
        if (op->op == XR_XIR_CALL && family != 3 && op->immediate == constructors[1]) {
            ++holder_calls; CHECK(op->type == classes[1] && op->args[1] == 1 && op->args[0] < fn->operand_count && cell != UINT32_MAX);
            CHECK(class_alias_original_origin(fn, fn->operands[op->args[0]], i) == cell); holder = i;
        }
        if (op->op == XR_XIR_CLASS_SET) {
            CHECK(op->type == XR_XIR_UNIT && !op->immediate);
            const XrXirInstruction *object = class_alias_original_operand(fn, op->args[0], i);
            const XrXirInstruction *value = class_alias_original_operand(fn, op->args[1], i);
            if (object->type == classes[0]) {
                ++scalar_sets; CHECK(family == 1 || family == 3);
                CHECK(class_alias_original_origin(fn, op->args[0], i) == cell);
                CHECK(value->op == XR_XIR_CONST_INT && value->type == XR_XIR_I64 && value->immediate == (family == 1 ? 17 : 42));
            } else {
                ++child_sets; CHECK(family == 1 || family == 2);
                CHECK(object->type == classes[1] && class_alias_original_origin(fn, op->args[0], i) == holder && value->type == classes[0]);
                if (family == 1) CHECK(replacement != UINT32_MAX && class_alias_original_origin(fn, op->args[1], i) == replacement);
                else CHECK(value->op == XR_XIR_CLASS_GET && !value->immediate && class_alias_original_origin(fn, value->args[0], op->args[1]) == holder);
            }
        }
        if (op->op == XR_XIR_CLASS_GET) {
            ++gets; CHECK(!op->immediate);
            const XrXirInstruction *object = class_alias_original_operand(fn, op->args[0], i);
            if (op->type == classes[0]) { CHECK(family != 3 && object->type == classes[1] && class_alias_original_origin(fn, op->args[0], i) == holder); child = i; }
            else { CHECK(op->type == XR_XIR_I64 && object->type == classes[0]); CHECK(family == 3 ? class_alias_original_origin(fn, op->args[0], i) == cell : op->args[0] == child); result = i; }
        }
        if (op->op == XR_XIR_RETURN) { ++returns; CHECK(result != UINT32_MAX && op->args[0] == result); }
    }
    CHECK(cell_calls == (family == 1 ? 2u : 1u) && holder_calls == (family == 3 ? 0u : 1u));
    CHECK(scalar_sets == (family == 1 || family == 3 ? 1u : 0u) && child_sets == (family == 1 || family == 2 ? 1u : 0u));
    CHECK(gets == (family == 2 ? 3u : family == 3 ? 1u : 2u) && returns == 1);
}
#endif // CLASS_ALIAS_ORIGINAL_SOURCE_SHAPE_H
