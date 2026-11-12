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
#include <stddef.h>
#include <stdlib.h>

#define REQUIRE(c)                                                                                 \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%d: %s\n", __LINE__, #c);                                             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static size_t allocation_index, fail_at = SIZE_MAX, live_allocations, live_bytes;
typedef union AllocationHeader {
    long double scalar_alignment;
    void *pointer_alignment;
    size_t bytes;
} AllocationHeader;
static void *injected_allocate(size_t size) {
    if (allocation_index++ == fail_at || size > SIZE_MAX - sizeof(AllocationHeader)) return NULL;
    AllocationHeader *allocation = xr_malloc(sizeof(*allocation) + size);
    if (!allocation) return NULL;
    allocation->bytes = size;++live_allocations;live_bytes += size;
    return allocation + 1;
}
static void injected_free(void *memory) {
    if (!memory) return;
    AllocationHeader *allocation = (AllocationHeader *)memory - 1;
    REQUIRE(live_allocations && live_bytes >= allocation->bytes);
    --live_allocations;live_bytes -= allocation->bytes;xr_free(allocation);
}

/* Inject into the production allocator calls without replacing graph logic. */
#undef xr_malloc
#define xr_malloc(size) injected_allocate(size)
#undef xr_free
#define xr_free(memory) injected_free(memory)
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

static void literal_allocation_failure(unsigned which) {
    fail_at = SIZE_MAX;allocation_index = 0;
    XrLirModule *module = NULL;REQUIRE(xr_lir_module_create(NULL, &module) == XR_LIR_OK);
    XrLirLiteral first = {0}, output = {0};
    if (which == 2) REQUIRE(xr_lir_add_literal(module, "kept", 4, &first) == XR_LIR_OK);
    fail_at = allocation_index + (which == 1 ? 1u : 0u);
    REQUIRE(xr_lir_add_literal(module, "bytes", 5, &output) == XR_LIR_OUT_OF_MEMORY);
    REQUIRE(!output.module && !output.id);
    REQUIRE(xr_lir_module_finish(module, NULL) == XR_LIR_OUT_OF_MEMORY);
    xr_lir_module_destroy(module);REQUIRE(!live_allocations && !live_bytes);
    fail_at = SIZE_MAX;module = NULL;
    REQUIRE(xr_lir_module_create(NULL, &module) == XR_LIR_OK);
    REQUIRE(xr_lir_add_literal(module, "bytes", 5, &output) == XR_LIR_OK);
    REQUIRE(xr_lir_module_finish(module, NULL) == XR_LIR_OK);
    const uint8_t *bytes = NULL;size_t length = 0;
    REQUIRE(xr_lir_literal_view(module, output, &bytes, &length) == XR_LIR_OK);
    REQUIRE(length == 5 && !memcmp(bytes, "bytes", 5));
    xr_lir_module_destroy(module);REQUIRE(!live_allocations && !live_bytes);
}

static XrLirStatus type_pool_build(XrLirModule **module) {
    XrLirStatus status = xr_lir_module_create(NULL, module);
    for (uint32_t n = 1; status == XR_LIR_OK && n <= 24; ++n) {
        XrLirType output = XR_LIR_I64;
        status = xr_lir_fixed_array_type(*module, XR_LIR_U8, n, &output);
        if (status != XR_LIR_OK) REQUIRE(output == XR_LIR_I64);
        for (uint32_t depth = 0; depth < 2 && status == XR_LIR_OK; ++depth) {
            XrLirType nullable = XR_LIR_I64;
            status = xr_lir_nullable_type(*module, output, &nullable);
            if (status != XR_LIR_OK) REQUIRE(nullable == XR_LIR_I64);
            output = nullable;
        }
    }
    return status;
}

static void type_pool_failures(void) {
    fail_at = SIZE_MAX;
    allocation_index = 0;
    XrLirModule *source = NULL;
    REQUIRE(type_pool_build(&source) == XR_LIR_OK);
    size_t construction_sites = allocation_index;
    REQUIRE(xr_lir_module_finish(source, NULL) == XR_LIR_OK);
    xr_lir_module_destroy(source);
    REQUIRE(!live_allocations && !live_bytes);
    for (size_t point = 0; point < construction_sites; ++point) {
        allocation_index = 0;
        fail_at = point;
        XrLirModule *module = NULL;
        REQUIRE(type_pool_build(&module) == XR_LIR_OUT_OF_MEMORY);
        if (module) REQUIRE(xr_lir_module_finish(module, NULL) == XR_LIR_OUT_OF_MEMORY);
        xr_lir_module_destroy(module);
        REQUIRE(!live_allocations && !live_bytes);
    }
    fail_at = SIZE_MAX;
    source = NULL;
    REQUIRE(type_pool_build(&source) == XR_LIR_OK);
    REQUIRE(xr_lir_module_finish(source, NULL) == XR_LIR_OK);
    size_t retained_count = live_allocations, retained_bytes = live_bytes;
    allocation_index = 0;
    XrLirModule *copy = NULL;
    REQUIRE(xr_lir_module_create(NULL, &copy) == XR_LIR_OK);
    REQUIRE(xr_lir_copy_types(copy, source) == XR_LIR_OK);
    size_t receiving_sites = allocation_index;
    xr_lir_module_destroy(copy);
    for (size_t point = 0; point < receiving_sites; ++point) {
        fail_at = point;
        allocation_index = 0;
        copy = NULL;
        XrLirStatus status = xr_lir_module_create(NULL, &copy);
        if (status == XR_LIR_OK) status = xr_lir_copy_types(copy, source);
        REQUIRE(status == XR_LIR_OUT_OF_MEMORY);
        if (copy) REQUIRE(xr_lir_module_finish(copy, NULL) == XR_LIR_OUT_OF_MEMORY);
        xr_lir_module_destroy(copy);
        REQUIRE(live_allocations == retained_count && live_bytes == retained_bytes);
    }
    fail_at = SIZE_MAX;
    REQUIRE(xr_lir_verify(source, NULL) == XR_LIR_OK);
    xr_lir_module_destroy(source);
    REQUIRE(!live_allocations && !live_bytes);
    printf("type pool construction/receiving failures covered %zu/%zu allocation sites\n",
        construction_sites, receiving_sites);
}

int main(void) {
    type_pool_failures();
    creation_failure(0);
    creation_failure(2);
    instruction_failure();
    REQUIRE(!live_allocations && !live_bytes);
    for (unsigned which = 0; which < 3; ++which) literal_allocation_failure(which);
    puts("representative LIR construction failures preserve unpublished outputs");
    return 0;
}
