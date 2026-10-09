/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cross_module_coroutines_source_shape.h - Imported suspended callees retain identity
 *
 * KEY CONCEPT:
 *   Owned declarations preserve the import graph, receiver-free callee and
 *   original parameter returned after two explicit suspension instructions.
 */
#ifndef CROSS_MODULE_COROUTINES_SOURCE_SHAPE_H
#define CROSS_MODULE_COROUTINES_SOURCE_SHAPE_H
#include "xir/xxir_types.h"
#include "xir/xxir_nominal.h"
static unsigned cross_module_coroutines_family(const char *name) {
    if (!strcmp(name, "cross_module_coroutine")) return 1;
    if (!strcmp(name, "cross_module_static_coroutine")) return 2;
    return 0;
}
static uint32_t cross_module_coroutines_graph(const XrXirModule *m) {
    CHECK(m && m->declarations);
    const XrXirDeclarations *d = m->declarations;
    CHECK(d->module_count == 2 && d->root_module < 2 && !d->slot_count);
    const XrXirSourceModule *root = &d->modules[d->root_module];
    CHECK(root->dependency_count == 1 && root->dependencies && root->initializer < m->function_count);
    uint32_t library = root->dependencies[0];
    CHECK(library < 2 && library != d->root_module);
    const XrXirSourceModule *dependency = &d->modules[library];
    CHECK(!dependency->dependency_count && dependency->initializer < m->function_count);
    CHECK(root->initializer != dependency->initializer && root->initializer != d->entry_function);
    CHECK(dependency->initializer != d->entry_function && d->entry_function < m->function_count);
    CHECK(xr_xir_module_imports(d, d->root_module, library) && !xr_xir_module_imports(d, library, d->root_module));
    CHECK(d->functions[root->initializer].module == d->root_module && d->functions[dependency->initializer].module == library);
    return library;
}
static void cross_module_coroutines_child(const XrXirModule *m, uint32_t id, unsigned family) {
    const XrXirFunction *fn = &m->functions[id];
    const XrXirFunctionIdentity *identity = &m->declarations->functions[id];
    CHECK(fn->parameter_count == 1 && fn->parameters[0] == XR_XIR_I64 && fn->result == XR_XIR_I64);
    CHECK(identity->exported && !(identity->promises & XR_XIR_FUNCTION_NO_SUSPEND));
    CHECK(identity->member_access == XR_XIR_MEMBER_PUBLIC);
    if (family == 1) CHECK(identity->method_kind == XR_XIR_NON_MEMBER && !identity->nominal_owner);
    else {
        CHECK(family == 2 && identity->method_kind == XR_XIR_STATIC_METHOD && m->types && m->types->nominals);
        CHECK(identity->nominal_owner && identity->nominal_owner <= m->types->nominals->count);
        const XrXirNominalDeclaration *owner = &m->types->nominals->declarations[identity->nominal_owner - 1];
        CHECK(owner->kind == XR_XIR_NOMINAL_CLASS && owner->exported && !owner->field_count && !owner->parameter_count);
        CHECK(owner->name.length == 6 && !memcmp(owner->name.bytes, "Worker", 6));
    }
    unsigned suspends = 0, returns = 0;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_SUSPEND) { CHECK(!returns && op->type == XR_XIR_UNIT); ++suspends; }
        if (op->op == XR_XIR_RETURN) { ++returns; CHECK(suspends == 2 && !op->args[0]); }
    }
    CHECK(suspends == 2 && returns == 1);
}
static void cross_module_coroutines_source_shape(const XrXirModule *m, unsigned family) {
    CHECK(family == 1 || family == 2); uint32_t library = cross_module_coroutines_graph(m);
    uint32_t child = UINT32_MAX, answer = UINT32_MAX;
    for (uint32_t f = 0; f < m->function_count; ++f) {
        const XrXirFunction *fn = &m->functions[f];
        const XrXirFunctionIdentity *identity = &m->declarations->functions[f];
        if (fn->name_length == 5 && !memcmp(fn->name, "child", 5)) { CHECK(child == UINT32_MAX && identity->module == library); child = f; }
        if (fn->name_length == 6 && !memcmp(fn->name, "answer", 6)) { CHECK(answer == UINT32_MAX && identity->module == m->declarations->root_module && !identity->exported); answer = f; }
    }
    CHECK(child != UINT32_MAX && answer != UINT32_MAX && child != answer);
    cross_module_coroutines_child(m, child, family);
    const XrXirFunction *fn = &m->functions[answer]; CHECK(!fn->parameter_count && fn->result == XR_XIR_I64);
    unsigned calls = 0, returns = 0; uint32_t result = UINT32_MAX;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_CALL) {
            ++calls; CHECK(op->immediate == child && op->type == XR_XIR_I64 && op->args[1] == 1 && op->args[0] < fn->operand_count);
            uint32_t value = fn->operands[op->args[0]]; CHECK(value < i);
            const XrXirInstruction *literal = &fn->instructions[value];
            CHECK(literal->op == XR_XIR_CONST_INT && literal->type == XR_XIR_I64 && literal->immediate == 7); result = i;
        }
        if (op->op == XR_XIR_RETURN) { ++returns; CHECK(result != UINT32_MAX && op->args[0] == result); }
    }
    CHECK(calls == 1 && returns == 1);
}
#endif // CROSS_MODULE_COROUTINES_SOURCE_SHAPE_H
