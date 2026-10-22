/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_bytecode_alloc.c - Relocating compilation and representative OOMs.
 */
#include "../lir/lir_test_programs.h"
#include "base/xmalloc.h"
#include "vm/register/xrbc_lir.h"

static size_t attempts, fail_at = SIZE_MAX;
static struct { void *pointer; size_t size; } buffers[32];

static void *allocate(size_t size) {
    if (++attempts == fail_at) return NULL;
    return xr_malloc(size);
}

static void *allocate_zero(size_t count, size_t size) {
    if (++attempts == fail_at) return NULL;
    return xr_calloc(count, size);
}

static void release(void *pointer) {
    for (size_t i = 0; i < 32; ++i)
        if (buffers[i].pointer == pointer) {
            buffers[i].pointer = NULL;
            buffers[i].size = 0;
        }
    xr_free(pointer);
}

static void *relocate(void *old, size_t size) {
    if (++attempts == fail_at) return NULL;
    size_t i = 0;
    while (i < 32 && buffers[i].pointer != old) ++i;
    CHECK(i < 32);
    void *fresh = xr_malloc(size);
    CHECK(fresh);
    if (old) {
        memcpy(fresh, old, buffers[i].size < size ? buffers[i].size : size);
        xr_free(old);
    }
    buffers[i].pointer = fresh;
    buffers[i].size = size;
    return fresh;
}

#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(size) allocate(size)
#define xr_calloc(count, size) allocate_zero(count, size)
#define xr_realloc(old, size) relocate(old, size)
#define xr_free(pointer) release(pointer)
static bool fail_location_allocation;
static void *allocate_location(size_t size) {
    if (fail_location_allocation) {
        fail_location_allocation = false;
        return NULL;
    }
    return allocate(size);
}
#undef xr_malloc
#define xr_malloc(size) allocate_location(size)
#include "runtime/native/xnative_locations.c"
#undef xr_malloc
#define xr_malloc(size) allocate(size)
#include "vm/register/xrbc_lir.c"

int main(void) {
    XrLirModule *m = test_fib();
    XrLirFunction root;
    OK(xr_lir_function_at(m, 1, &root));
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic;
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    size_t total = attempts;
    xr_rbc_program_destroy(program);
    const size_t failures[] = {1, total / 2, total};
    for (size_t i = 0; i < 3; ++i) {
        attempts = 0; fail_at = failures[i]; program = NULL;
        CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && diagnostic.status == XR_RBC_COMPILE_OUT_OF_MEMORY);
        fail_at = SIZE_MAX;
        CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
        uint64_t n = 12, result = 0;
        CHECK(xr_rbc_program_run(program, &n, 1, &result) == XR_RBC_OK && result == 144);
        xr_rbc_program_destroy(program);
    }
    program = NULL;
    fail_location_allocation = true;
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OUT_OF_MEMORY);
    CHECK(!program && !fail_location_allocation && diagnostic.status == XR_RBC_COMPILE_OUT_OF_MEMORY);
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    CHECK(xr_rbc_program_locations(program));
    xr_rbc_program_destroy(program);
    xr_lir_module_destroy(m);
    puts("lir_bytecode_alloc: passed (representative compiler/owner failures, relocating buffers)");
    return 0;
}
