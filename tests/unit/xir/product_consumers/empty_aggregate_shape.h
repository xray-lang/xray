/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * empty_aggregate_shape.h - Real zero-field construction with the original scalar result
 *
 * KEY CONCEPT:
 *   Empty aggregate storage is exercised by an actual retained constructor.
 */
#ifndef EMPTY_AGGREGATE_SHAPE_H
#define EMPTY_AGGREGATE_SHAPE_H
#include "xir/xxir_types.h"
#include "xir/xxir_nominal.h"
static void empty_aggregate_shape(const XrXirModule *m) {
    CHECK(m && m->declarations && m->types && m->types->nominals);
    unsigned constructors = 0, answers = 0;
    for (uint32_t f = 0; f < m->function_count; ++f) {
        const XrXirFunction *fn = &m->functions[f];
        if (fn->name_length != 6 || memcmp(fn->name, "answer", 6)) continue;
        ++answers; CHECK(!fn->parameter_count && fn->result == XR_XIR_I64 && !m->declarations->functions[f].exported);
        uint32_t scalar = UINT32_MAX, returned = UINT32_MAX;
        for (uint32_t i = 0; i < fn->instruction_count; ++i) {
            const XrXirInstruction *op = &fn->instructions[i];
            if (op->op == XR_XIR_STRUCT_NEW) {
                ++constructors; CHECK(!op->args[0] && !op->args[1]);
                const XrXirTypeNode *node = xr_xir_type_node(m->types, op->type);
                CHECK(node && node->kind == XR_XIR_TYPE_NOMINAL && xr_xir_type_is_struct(m->types, op->type));
                CHECK(!node->nominal.argument_count && !node->nominal.field_count);
                CHECK(node->nominal.declaration < m->types->nominals->count);
                const XrXirNominalDeclaration *d = &m->types->nominals->declarations[node->nominal.declaration];
                CHECK(d->kind == XR_XIR_NOMINAL_STRUCT && !d->field_count && !d->parameter_count);
                CHECK(d->name.length == 5 && !memcmp(d->name.bytes, "Empty", 5));
            }
            if (op->op == XR_XIR_CONST_INT && op->type == XR_XIR_I64 && op->immediate == 42) {
                CHECK(scalar == UINT32_MAX); scalar = i;
            }
            if (op->op == XR_XIR_RETURN) { CHECK(returned == UINT32_MAX); returned = op->args[0]; }
        }
        CHECK(scalar != UINT32_MAX && returned == scalar);
    }
    CHECK(answers == 1 && constructors == 1);
}
#endif // EMPTY_AGGREGATE_SHAPE_H
