/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_rebuild_faults.c - Representative receiver and scratch failures
 *
 * KEY CONCEPT:
 *   Compile the actual builder and rebuilder under one allocator override.
 *   Source ownership remains live when receiver construction cannot publish.
 */
#include "lir/xlir_builder.h"
#include "lir/xlir_rebuild.h"
#include "lir/xlir_verify.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)

static void *real_malloc(size_t size) { return xr_malloc(size); }
static void *real_calloc(size_t count, size_t size) { return xr_calloc(count, size); }
static void *fault_malloc(size_t size);
static void *fault_calloc(size_t count, size_t size);
static enum { NONE, RECEIVER, FUNCTION_MAP, METADATA } fault;
#undef xr_malloc
#undef xr_calloc
#define xr_malloc(size) fault_malloc(size)
#define xr_calloc(count, size) fault_calloc(count, size)
#include "lir/xlir.c"
#include "lir/xlir_rebuild.c"

static void *fault_malloc(size_t size) {
    if ((fault == RECEIVER && size == sizeof(XrLirModule)) || (fault == METADATA && size == 401)) {
        fault = NONE;
        return NULL;
    }
    return real_malloc(size);
}
static void *fault_calloc(size_t count, size_t size) {
    if (fault == FUNCTION_MAP && count == 2 && size == sizeof(XrLirFunction)) {
        fault = NONE;
        return NULL;
    }
    return real_calloc(count, size);
}

int main(void) {
    XrLirModule *input = NULL;
    CHECK(xr_lir_module_create(NULL, &input) == XR_LIR_OK);
    XrLirFunction root;
    XrLirFunctionSpec spec = {.name = "root", .name_length = 4, .result = XR_LIR_I64};
    CHECK(xr_lir_declare_function(input, &spec, &root) == XR_LIR_OK);
    CHECK(xr_lir_define_function(input, root) == XR_LIR_OK);
    XrLirBlock block;
    CHECK(xr_lir_add_block(input, root, NULL, 0, &block) == XR_LIR_OK);
    CHECK(xr_lir_set_entry(input, root, block) == XR_LIR_OK);
    char path[401]; memset(path, 'p', 400); path[400] = 0;
    XrLirInstruction instruction = {.op = XR_LIR_CONST, .type = XR_LIR_I64,
        .bits = 42, .source = {path, 400, 1, 1, 1, 2}};
    XrLirValue value;
    CHECK(xr_lir_emit_value(input, block, &instruction, &value) == XR_LIR_OK);
    instruction = (XrLirInstruction){.op = XR_LIR_RET, .type = XR_LIR_UNIT,
        .operands = &value, .operand_count = 1};
    CHECK(xr_lir_set_terminator(input, block, &instruction) == XR_LIR_OK);
    CHECK(xr_lir_module_finish(input, NULL) == XR_LIR_OK);
    const int failures[] = {RECEIVER, FUNCTION_MAP, METADATA};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        fault = failures[i];
        XrLirModule *copy = NULL;
        CHECK(xr_lir_rebuild(input, &copy) == XR_LIR_OUT_OF_MEMORY);
        CHECK(fault == NONE && !copy && xr_lir_module_is_readable(input));
        CHECK(xr_lir_verify(input, NULL) == XR_LIR_OK);
        CHECK(xr_lir_rebuild(input, &copy) == XR_LIR_OK);
        CHECK(copy && xr_lir_verify(copy, NULL) == XR_LIR_OK);
        xr_lir_module_destroy(copy);
    }
    xr_lir_module_destroy(input);
    puts("Representative reconstruction failures passed");
    return 0;
}
