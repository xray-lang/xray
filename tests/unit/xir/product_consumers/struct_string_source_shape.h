/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * struct_string_source_shape.h - Original nested value struct String copies
 *
 * KEY CONCEPT:
 *   Original field identities, constructors and writes survive source owner death.
 */
#ifndef STRUCT_STRING_SOURCE_SHAPE_H
#define STRUCT_STRING_SOURCE_SHAPE_H
#include "xir/xxir_types.h"
#include "xir/xxir_nominal.h"
static bool struct_string_literal(XrXirLiteral literal, const char *text) {
    return literal.length == strlen(text) && !memcmp(literal.bytes, text, literal.length);
}
static const XrXirInstruction *struct_string_operand(const XrXirFunction *fn, uint32_t id, uint32_t before) {
    CHECK(!fn->parameter_count && id < before); return &fn->instructions[id];
}
static bool struct_string_value(const XrXirModule *m, const XrXirInstruction *op, const char *text) {
    CHECK(op->op == XR_XIR_CONST_STRING && op->type == XR_XIR_STRING && op->immediate >= 0 &&
        (uint64_t)op->immediate < m->declarations->literal_count);
    return struct_string_literal(m->declarations->literals[op->immediate], text);
}
static void struct_string_source_shape(const XrXirModule *m) {
    CHECK(m && m->types && m->types->nominals && m->declarations);
    const XrXirNominalTable *nominals = m->types->nominals; CHECK(nominals->count == 2);
    uint32_t report = UINT32_MAX, envelope = UINT32_MAX;
    for (uint32_t n = 0; n < nominals->count; ++n) {
        const XrXirNominalDeclaration *d = &nominals->declarations[n];
        CHECK(d->kind == XR_XIR_NOMINAL_STRUCT && !d->parameter_count && d->field_count == 3 && !d->exported);
        const char *names[3];
        if (struct_string_literal(d->name, "Report")) { report = n; names[0] = "label"; names[1] = "footer"; names[2] = "code"; }
        else { CHECK(struct_string_literal(d->name, "Envelope")); envelope = n; names[0] = "prefix"; names[1] = "report"; names[2] = "suffix"; }
        for (unsigned f = 0; f < 3; ++f) CHECK(struct_string_literal(d->fields[f].name, names[f]));
        CHECK(d->fields[0].type == XR_XIR_STRING && d->fields[2].type == (n == report ? XR_XIR_I64 : XR_XIR_STRING));
        if (n == report) CHECK(d->fields[1].type == XR_XIR_STRING);
    }
    CHECK(report != UINT32_MAX && envelope != UINT32_MAX && report != envelope);
    const XrXirTypeNode *nested = xr_xir_type_node(m->types, nominals->declarations[envelope].fields[1].type);
    CHECK(nested && nested->kind == XR_XIR_TYPE_NOMINAL && nested->nominal.declaration == report);
    unsigned answers = 0;
    for (uint32_t f = 0; f < m->function_count; ++f) {
        const XrXirFunction *fn = &m->functions[f];
        if (fn->name_length != 6 || memcmp(fn->name, "answer", 6)) continue;
        ++answers; CHECK(!fn->parameter_count && fn->result == XR_XIR_I64 && !m->declarations->functions[f].exported);
        unsigned constructors[2] = {0}, writes = 0, equalities = 0;
        for (uint32_t i = 0; i < fn->instruction_count; ++i) {
            const XrXirInstruction *op = &fn->instructions[i];
            if (op->op == XR_XIR_STRUCT_NEW) {
                const XrXirTypeNode *node = xr_xir_type_node(m->types, op->type);
                CHECK(node && node->kind == XR_XIR_TYPE_NOMINAL && node->nominal.field_count == 3 && !node->nominal.argument_count);
                bool is_report = node->nominal.declaration == report; CHECK(is_report || node->nominal.declaration == envelope);
                ++constructors[is_report ? 0 : 1];
                CHECK(op->args[1] == 3 && fn->operand_count >= 3 && op->args[0] <= fn->operand_count - 3);
                const uint32_t *args = fn->operands + op->args[0];
                CHECK(struct_string_value(m, struct_string_operand(fn, args[0], i), is_report ? "ready" : "start"));
                if (is_report) {
                    CHECK(struct_string_value(m, struct_string_operand(fn, args[1], i), "end"));
                    const XrXirInstruction *code = struct_string_operand(fn, args[2], i);
                    CHECK(code->op == XR_XIR_CONST_INT && code->type == XR_XIR_I64 && code->immediate == 40);
                } else CHECK(struct_string_value(m, struct_string_operand(fn, args[2], i), "stop"));
            }
            if (op->op == XR_XIR_PLACE_WRITE) {
                CHECK(writes < 3 && op->type == XR_XIR_UNIT);
                const XrXirInstruction *place = struct_string_operand(fn, op->args[0], i);
                const XrXirInstruction *value = struct_string_operand(fn, op->args[1], i);
                CHECK(place->op == XR_XIR_FIELD_PLACE && place->immediate == (writes ? 2 : 0));
                CHECK(place->type == (writes == 1 ? XR_XIR_I64 : XR_XIR_STRING));
                if (writes == 1) CHECK(value->op == XR_XIR_CONST_INT && value->type == XR_XIR_I64 && value->immediate == 42);
                else CHECK(struct_string_value(m, value, writes ? "done" : "go"));
                ++writes;
            }
            if (op->op == XR_XIR_EQUAL && xr_xir_operand_type(fn, op->args[0]) == XR_XIR_STRING) {
                ++equalities; CHECK(op->type == XR_XIR_BOOL && !op->immediate &&
                    xr_xir_operand_type(fn, op->args[1]) == XR_XIR_STRING);
            }
        }
        CHECK(constructors[0] == 1 && constructors[1] == 1 && writes == 3 && equalities == 12);
    }
    CHECK(answers == 1);
}
#endif // STRUCT_STRING_SOURCE_SHAPE_H
