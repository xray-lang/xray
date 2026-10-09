/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * class_identity_source_shape.h - Original Array append retains class identity
 *
 * KEY CONCEPT:
 *   The appended reference and later field mutation share the same local root.
 */
#ifndef CLASS_IDENTITY_SOURCE_SHAPE_H
#define CLASS_IDENTITY_SOURCE_SHAPE_H
#include "xir/xxir_types.h"
#include "xir/xxir_nominal.h"
static const XrXirInstruction *class_identity_operand(const XrXirFunction *fn, uint32_t id, uint32_t before) {
    CHECK(!fn->parameter_count && id < before); return &fn->instructions[id];
}
static void class_identity_source_shape(const XrXirModule *m) {
    CHECK(m && m->declarations && m->types && m->types->nominals && m->types->nominals->count == 1);
    const XrXirNominalDeclaration *holder = &m->types->nominals->declarations[0];
    CHECK(holder->kind == XR_XIR_NOMINAL_CLASS && holder->name.length == 6 && !memcmp(holder->name.bytes, "Holder", 6));
    CHECK(!holder->parameter_count && holder->field_count == 1 && holder->fields[0].name.length == 5 &&
        !memcmp(holder->fields[0].name.bytes, "value", 5) && holder->fields[0].type == XR_XIR_I64);
    uint32_t constructor = UINT32_MAX, answer = UINT32_MAX; XrXirType class_type = XR_XIR_UNIT;
    for (uint32_t f = 0; f < m->function_count; ++f) {
        const XrXirFunction *fn = &m->functions[f];
        if (m->declarations->functions[f].method_kind == XR_XIR_CONSTRUCTOR) {
            CHECK(constructor == UINT32_MAX && m->declarations->functions[f].nominal_owner == 1);
            CHECK(fn->parameter_count == 1 && fn->parameters[0] == XR_XIR_I64 && xr_xir_type_is_class(m->types, fn->result));
            constructor = f; class_type = fn->result;
        }
        if (fn->name_length == 6 && !memcmp(fn->name, "answer", 6)) { CHECK(answer == UINT32_MAX); answer = f; }
    }
    CHECK(constructor != UINT32_MAX && answer != UINT32_MAX && constructor != answer);
    const XrXirFunction *fn = &m->functions[answer];
    CHECK(!fn->parameter_count && fn->result == XR_XIR_I64 && !m->declarations->functions[answer].exported);
    unsigned calls = 0, arrays = 0, pushes = 0, sets = 0, gets = 0, fields = 0, returns = 0;
    uint32_t original = UINT32_MAX, array = UINT32_MAX, element = UINT32_MAX, field = UINT32_MAX;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_ARRAY_NEW) { ++arrays; CHECK(!op->args[0] && !op->args[1] && xr_xir_array_element(m->types, op->type) == class_type); }
        if (op->op == XR_XIR_CALL && op->immediate == constructor) {
            ++calls; CHECK(op->type == class_type && op->args[1] == 1 && op->args[0] < fn->operand_count);
            const XrXirInstruction *value = class_identity_operand(fn, fn->operands[op->args[0]], i);
            CHECK(value->op == XR_XIR_CONST_INT && value->type == XR_XIR_I64 && value->immediate == 19);
        }
        if (op->op == XR_XIR_ARRAY_PUSH) {
            ++pushes; CHECK(op->type == XR_XIR_UNIT);
            const XrXirInstruction *place = class_identity_operand(fn, op->args[0], i);
            const XrXirInstruction *value = class_identity_operand(fn, op->args[1], i);
            CHECK(place->op == XR_XIR_CELL_PLACE && xr_xir_array_element(m->types, place->type) == class_type);
            CHECK(value->op == XR_XIR_CELL_READ && value->type == class_type); original = value->args[0]; array = place->args[0];
        }
        if (op->op == XR_XIR_CLASS_SET) {
            ++sets; CHECK(pushes == 1 && op->type == XR_XIR_UNIT && !op->immediate);
            const XrXirInstruction *object = class_identity_operand(fn, op->args[0], i);
            const XrXirInstruction *value = class_identity_operand(fn, op->args[1], i);
            CHECK(object->op == XR_XIR_CELL_READ && object->type == class_type && object->args[0] == original);
            CHECK(value->op == XR_XIR_CONST_INT && value->type == XR_XIR_I64 && value->immediate == 42);
        }
        if (op->op == XR_XIR_ARRAY_GET) {
            ++gets; CHECK(sets == 1 && op->type == class_type); element = i;
            const XrXirInstruction *place = class_identity_operand(fn, op->args[0], i);
            const XrXirInstruction *index = class_identity_operand(fn, op->args[1], i);
            CHECK(place->op == XR_XIR_CELL_PLACE && place->args[0] == array);
            CHECK(index->op == XR_XIR_CONST_INT && index->type == XR_XIR_I64 && !index->immediate);
        }
        if (op->op == XR_XIR_CLASS_GET) { ++fields; CHECK(op->type == XR_XIR_I64 && !op->immediate && op->args[0] == element); field = i; }
        if (op->op == XR_XIR_RETURN) { ++returns; CHECK(field != UINT32_MAX && op->args[0] == field); }
    }
    CHECK(calls == 1 && arrays == 1 && pushes == 1 && sets == 1 && gets == 1 && fields == 1 && returns == 1);
}
#endif // CLASS_IDENTITY_SOURCE_SHAPE_H
