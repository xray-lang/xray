/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_sccp_faults.c - Representative partial analysis failures
 */
#include "lir_test_programs.h"
#include "lir/xlir_verify.h"
#include "base/xmalloc.h"

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
    XrLirFunction f = test_function(input, "root", NULL, 0, XR_LIR_I64);
    XrLirBlock b = test_block(input, f, NULL, 0); OK(xr_lir_set_entry(input, f, b));
    XrLirValue a = test_constant(input, b, XR_LIR_I64, 20);
    XrLirValue c = test_constant(input, b, XR_LIR_I64, 22);
    test_return(input, b, test_binary(input, b, XR_LIR_ADD, a, c, XR_LIR_PREDICATE_NONE));
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
