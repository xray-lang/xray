/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_inline_faults.c - Representative graph, plan and instance failures
 */
#include "lir/xlir_builder.h"
#include "lir/xlir_verify.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)
static void *real_calloc(size_t count, size_t size) { return xr_calloc(count, size); }
static void *fault_calloc(size_t count, size_t size);
static enum { NONE, GRAPH, PLAN, INSTANCE } fault;
#undef xr_calloc
#define xr_calloc(count, size) fault_calloc(count, size)
// Only the actual transform is overridden; the verifier and owning builder stay real.
#include "lir/xlir_inline.c"
static void *fault_calloc(size_t count, size_t size) {
    if ((fault == GRAPH && size == sizeof(InlineFunction)) ||
        (fault == PLAN && size == sizeof(bool)) ||
        (fault == INSTANCE && count == 4 && size == sizeof(XrLirValue))) {
        fault = NONE; return NULL;
    }
    return real_calloc(count, size);
}
int main(void) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m)); XrLirType type = XR_LIR_I64;
    XrLirFunctionSpec spec = {.name = "root", .name_length = 4, .result = type};
    XrLirFunction root, helper; OK(xr_lir_declare_function(m, &spec, &root));
    spec.name = "step"; spec.parameters = &type; spec.parameter_count = 1;
    OK(xr_lir_declare_function(m, &spec, &helper));
    OK(xr_lir_define_function(m, root)); OK(xr_lir_define_function(m, helper));
    XrLirBlock a, b; OK(xr_lir_add_block(m, root, NULL, 0, &a)); OK(xr_lir_add_block(m, helper, NULL, 0, &b));
    OK(xr_lir_set_entry(m, root, a)); OK(xr_lir_set_entry(m, helper, b));
    XrLirValue value, result, n, operands[2];
    XrLirInstruction v = {.op = XR_LIR_CONST, .type = type, .bits = 41}; OK(xr_lir_emit_value(m, a, &v, &value));
    v = (XrLirInstruction){.op = XR_LIR_CALL, .type = type, .callee = helper, .operands = &value, .operand_count = 1};
    OK(xr_lir_emit_value(m, a, &v, &result));
    v = (XrLirInstruction){.op = XR_LIR_RET, .type = XR_LIR_UNIT, .operands = &result, .operand_count = 1};
    OK(xr_lir_set_terminator(m, a, &v)); OK(xr_lir_function_parameter(m, helper, 0, &n));
    v = (XrLirInstruction){.op = XR_LIR_CONST, .type = type, .bits = 1}; OK(xr_lir_emit_value(m, b, &v, &value));
    operands[0] = n; operands[1] = value;
    v = (XrLirInstruction){.op = XR_LIR_ADD, .type = type, .operands = operands, .operand_count = 2};
    OK(xr_lir_emit_value(m, b, &v, &result));
    v = (XrLirInstruction){.op = XR_LIR_RET, .type = XR_LIR_UNIT, .operands = &result, .operand_count = 1};
    OK(xr_lir_set_terminator(m, b, &v)); OK(xr_lir_module_finish(m, NULL));
    const int failures[] = {GRAPH, PLAN, INSTANCE};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        fault = failures[i]; XrLirModule *out = NULL;
        CHECK(xr_lir_inline(m, &out) == XR_LIR_OUT_OF_MEMORY && !out && fault == NONE);
        CHECK(xr_lir_module_is_readable(m)); OK(xr_lir_verify(m, NULL));
        OK(xr_lir_inline(m, &out)); OK(xr_lir_verify(out, NULL)); xr_lir_module_destroy(out);
    }
    xr_lir_module_destroy(m); puts("Representative whole-receiver inline failures passed"); return 0;
}
