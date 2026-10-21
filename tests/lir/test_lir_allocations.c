/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_allocations.c - Representative construction failures never publish
 */
#include "base/xchecks.h"
#include "base/xmalloc.h"
#include "lir/xlir_builder.h"
#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(c)                                                                                 \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%d: %s\n", __LINE__, #c);                                             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static size_t allocation_index, fail_at = SIZE_MAX;

static void *injected_allocate(size_t size) {
    if (allocation_index++ == fail_at)
        return NULL;
    return xr_malloc(size);
}

/* Inject into the production allocator calls without replacing graph logic. */
#undef xr_malloc
#define xr_malloc(size) injected_allocate(size)
#include "lir/xlir.c"

static void creation_failure(size_t point) {
    XrLirType type = XR_LIR_I64;
    XrLirIntrinsic intrinsic = {19, &type, 1, type, true};
    XrLirRegistry registry = {&intrinsic, 1};
    XrLirModule *module = NULL;
    allocation_index = 0;
    fail_at = point;
    REQUIRE(xr_lir_module_create(&registry, &module) == XR_LIR_OUT_OF_MEMORY);
    REQUIRE(module == NULL);
}

static void instruction_failure(void) {
    fail_at = SIZE_MAX;
    XrLirModule *module = NULL;
    REQUIRE(xr_lir_module_create(NULL, &module) == XR_LIR_OK);
    XrLirFunctionSpec spec = {.name = "owned", .name_length = 5, .result = XR_LIR_UNIT};
    XrLirFunction function = {0};
    XrLirBlock block = {0};
    REQUIRE(xr_lir_declare_function(module, &spec, &function) == XR_LIR_OK);
    REQUIRE(xr_lir_define_function(module, function) == XR_LIR_OK);
    REQUIRE(xr_lir_add_block(module, function, NULL, 0, &block) == XR_LIR_OK);
    REQUIRE(xr_lir_set_entry(module, function, block) == XR_LIR_OK);
    XrLirInstruction instruction = {.op = XR_LIR_CONST,
                                     .type = XR_LIR_I64,
                                     .bits = 7,
                                     .source = {"fault.xr", 8, 1, 1, 1, 4}};
    XrLirValue value = {0};
    fail_at = allocation_index;
    REQUIRE(xr_lir_emit_value(module, block, &instruction, &value) == XR_LIR_OUT_OF_MEMORY);
    REQUIRE(!value.id && !value.module);
    XrLirDiagnostic diagnostic;
    REQUIRE(xr_lir_module_finish(module, &diagnostic) == XR_LIR_OUT_OF_MEMORY);
    REQUIRE(diagnostic.status == XR_LIR_OUT_OF_MEMORY && diagnostic.message[0]);
    xr_lir_module_destroy(module);
}

int main(void) {
    creation_failure(0);
    creation_failure(2);
    instruction_failure();
    puts("representative LIR construction failures preserve unpublished outputs");
    return 0;
}
