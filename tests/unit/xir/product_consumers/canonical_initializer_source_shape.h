/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * canonical_initializer_source_shape.h - Original typed initializer output
 *
 * KEY CONCEPT:
 *   Exact SSA edges bind the original generic struct projection to PRINT;
 *   the canonical entry returns zero independently of initialization output.
 */
#ifndef CANONICAL_INITIALIZER_SOURCE_SHAPE_H
#define CANONICAL_INITIALIZER_SOURCE_SHAPE_H
#include "xir/xxir_types.h"
#include "xir/xxir_nominal.h"

static void canonical_initializer_literal(const XrXirFunction *fn, uint32_t id,
    uint32_t before, int64_t expected) {
    CHECK(!fn->parameter_count && id < before);
    const XrXirInstruction *op = &fn->instructions[id];
    CHECK(op->op == XR_XIR_CONST_INT && op->type == XR_XIR_I64 && op->immediate == expected);
}

static void canonical_initializer_box(const XrXirModule *m, XrXirType type) {
    CHECK(m->types && m->types->nominals && xr_xir_type_is_struct(m->types, type));
    const XrXirTypeNode *node = xr_xir_type_node(m->types, type);
    CHECK(node && node->kind == XR_XIR_TYPE_NOMINAL && node->nominal.argument_count == 1);
    CHECK(node->nominal.arguments[0] == XR_XIR_I64 && node->nominal.field_count == 1 && node->nominal.fields[0] == XR_XIR_I64);
    CHECK(node->nominal.declaration < m->types->nominals->count);
    const XrXirNominalDeclaration *box = &m->types->nominals->declarations[node->nominal.declaration];
    CHECK(box->kind == XR_XIR_NOMINAL_STRUCT && box->parameter_count == 1 && box->field_count == 1 && !box->exported);
    CHECK(box->name.length == 3 && !memcmp(box->name.bytes, "Box", 3));
    CHECK(box->fields[0].name.length == 5 && !memcmp(box->fields[0].name.bytes, "value", 5));
    CHECK(box->fields[0].type == XR_XIR_TYPE_PARAMETER_BASE);
}

static void canonical_initializer_source_shape(const XrXirModule *m) {
    const XrXirDeclarations *d = m->declarations;
    CHECK(d && d->module_count == 1 && !d->root_module && m->function_count == 2 && !d->slot_count);
    CHECK(!d->modules[0].dependency_count && d->entry_function < m->function_count);
    uint32_t initializer = d->modules[0].initializer;
    CHECK(initializer < m->function_count && initializer != d->entry_function);
    for (uint32_t f = 0; f < m->function_count; ++f) {
        CHECK(!d->functions[f].module && !d->functions[f].exported && !d->functions[f].test_role);
        CHECK(!m->functions[f].parameter_count);
    }
    const XrXirFunction *init = &m->functions[initializer], *entry = &m->functions[d->entry_function];
    CHECK(init->result == XR_XIR_UNIT && init->name_length == 5 && !memcmp(init->name, "$init", 5));
    CHECK(entry->result == XR_XIR_I64 && entry->name_length == 6 && !memcmp(entry->name, "$entry", 6));
    CHECK(entry->instruction_count == 2);
    canonical_initializer_literal(entry, 0, 1, 0);
    CHECK(entry->instructions[1].op == XR_XIR_RETURN && !entry->instructions[1].args[0]);
    uint32_t aggregate = UINT32_MAX, field = UINT32_MAX, sum = UINT32_MAX;
    unsigned news = 0, gets = 0, adds = 0, prints = 0, returns = 0;
    for (uint32_t i = 0; i < init->instruction_count; ++i) {
        const XrXirInstruction *op = &init->instructions[i];
        if (op->op == XR_XIR_STRUCT_NEW) {
            CHECK(!news++ && op->args[1] == 1 && op->args[0] < init->operand_count);
            canonical_initializer_literal(init, init->operands[op->args[0]], i, 41);
            canonical_initializer_box(m, op->type); aggregate = i;
        }
        if (op->op == XR_XIR_STRUCT_GET) {
            CHECK(!gets++ && aggregate != UINT32_MAX && op->type == XR_XIR_I64);
            CHECK(op->args[0] == aggregate && !op->args[1]); field = i;
        }
        if (op->op == XR_XIR_ADD_INT) {
            CHECK(!adds++ && field != UINT32_MAX && op->type == XR_XIR_I64 && op->args[0] == field);
            canonical_initializer_literal(init, op->args[1], i, 1); sum = i;
        }
        if (op->op == XR_XIR_PRINT) {
            CHECK(!prints++ && sum != UINT32_MAX && op->type == XR_XIR_UNIT);
            CHECK(op->args[1] == 1 && op->args[0] < init->operand_count && init->operands[op->args[0]] == sum);
        }
        if (op->op == XR_XIR_RETURN) { CHECK(prints == 1 && op->type == XR_XIR_UNIT); ++returns; }
    }
    CHECK(news == 1 && gets == 1 && adds == 1 && prints == 1 && returns == 1);
}
#endif // CANONICAL_INITIALIZER_SOURCE_SHAPE_H
