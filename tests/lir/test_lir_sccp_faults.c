/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_sccp_faults.c - Representative partial analysis failures
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
static enum { NONE, CELLS, NODES, USES } fault;
#undef xr_calloc
#define xr_calloc(count, size) fault_calloc(count, size)
// Only the actual analysis allocator is overridden, not the input verifier or copier.
#include "lir/xlir_sccp.c"

static void *fault_calloc(size_t count, size_t size) {
    if ((fault == CELLS && size == sizeof(SccpCell)) ||
        (fault == NODES && size == sizeof(SccpNode)) ||
        (fault == USES && size == sizeof(SccpUse))) {
        fault = NONE;
        return NULL;
    }
    return real_calloc(count, size);
}
int main(void) {
    XrLirModule *input = NULL; OK(xr_lir_module_create(NULL, &input));
    XrLirFunction f;
    XrLirFunctionSpec spec = {.name = "root", .name_length = 4, .result = XR_LIR_I64};
    OK(xr_lir_declare_function(input, &spec, &f)); OK(xr_lir_define_function(input, f));
    XrLirBlock b; OK(xr_lir_add_block(input, f, NULL, 0, &b)); OK(xr_lir_set_entry(input, f, b));
    XrLirValue operands[2], result;
    XrLirInstruction v = {.op = XR_LIR_CONST, .type = XR_LIR_I64, .bits = 20};
    OK(xr_lir_emit_value(input, b, &v, &operands[0]));
    v.bits = 22; OK(xr_lir_emit_value(input, b, &v, &operands[1]));
    v = (XrLirInstruction){.op = XR_LIR_ADD, .type = XR_LIR_I64, .operands = operands, .operand_count = 2};
    OK(xr_lir_emit_value(input, b, &v, &result));
    v = (XrLirInstruction){.op = XR_LIR_RET, .type = XR_LIR_UNIT, .operands = &result, .operand_count = 1};
    OK(xr_lir_set_terminator(input, b, &v));
    OK(xr_lir_module_finish(input, NULL));
    const int points[] = {CELLS, NODES, USES};
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
        fault = points[i];
        XrLirModule *output = NULL;
        CHECK(xr_lir_sccp(input, &output) == XR_LIR_OUT_OF_MEMORY);
        CHECK(fault == NONE && !output && xr_lir_module_is_readable(input));
        OK(xr_lir_verify(input, NULL));
        OK(xr_lir_sccp(input, &output)); OK(xr_lir_verify(output, NULL));
        xr_lir_module_destroy(output);
    }
    xr_lir_module_destroy(input);
    puts("Representative partial constant-propagation failures passed");
    return 0;
}
