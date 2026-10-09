/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * multi_safepoint_source_shape.h - Two original yields precede the fixed result
 *
 * KEY CONCEPT:
 *   Owned Checked instructions preserve both suspension points and the
 *   actual returned constant after all Source producers are destroyed.
 */
#ifndef MULTI_SAFEPOINT_SOURCE_SHAPE_H
#define MULTI_SAFEPOINT_SOURCE_SHAPE_H
static void multi_safepoint_source_shape(const XrXirModule *m) {
    CHECK(m && m->declarations); const XrXirDeclarations *d = m->declarations;
    CHECK(d->module_count == 1 && !d->root_module && !d->slot_count);
    CHECK(!d->modules[0].dependency_count && d->modules[0].initializer < m->function_count);
    CHECK(d->entry_function < m->function_count && d->entry_function != d->modules[0].initializer);
    uint32_t answer = UINT32_MAX, adapter = UINT32_MAX;
    for (uint32_t f = 0; f < m->function_count; ++f) {
        const XrXirFunction *fn = &m->functions[f];
        if (fn->name_length == 6 && !memcmp(fn->name, "answer", 6)) {
            CHECK(answer == UINT32_MAX && !d->functions[f].exported && !d->functions[f].module);
            CHECK(!(d->functions[f].promises & XR_XIR_FUNCTION_NO_SUSPEND)); answer = f;
        }
        if (fn->name_length == 14 && !memcmp(fn->name, "consumerAnswer", 14)) {
            CHECK(adapter == UINT32_MAX && d->functions[f].exported && !d->functions[f].module); adapter = f;
        }
    }
    CHECK(answer != UINT32_MAX && adapter != UINT32_MAX && answer != adapter);
    const XrXirFunction *fn = &m->functions[answer]; CHECK(!fn->parameter_count && fn->result == XR_XIR_I64);
    unsigned suspends = 0, constants = 0, returns = 0; uint32_t value = UINT32_MAX;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_SUSPEND) { ++suspends; CHECK(!constants && !returns && op->type == XR_XIR_UNIT); }
        if (op->op == XR_XIR_CONST_INT) { ++constants; CHECK(suspends == 2 && op->type == XR_XIR_I64 && op->immediate == 42); value = i; }
        if (op->op == XR_XIR_RETURN) { ++returns; CHECK(value != UINT32_MAX && op->args[0] == value); }
    }
    CHECK(suspends == 2 && constants == 1 && returns == 1); fn = &m->functions[adapter];
    CHECK(!fn->parameter_count && fn->result == XR_XIR_I64); unsigned calls = 0; returns = 0; value = UINT32_MAX;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_CALL) { ++calls; CHECK(op->immediate == answer && op->type == XR_XIR_I64 && !op->args[1]); value = i; }
        if (op->op == XR_XIR_RETURN) { ++returns; CHECK(value != UINT32_MAX && op->args[0] == value); }
    }
    CHECK(calls == 1 && returns == 1);
}
#endif // MULTI_SAFEPOINT_SOURCE_SHAPE_H
