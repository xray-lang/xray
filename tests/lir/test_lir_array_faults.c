/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_array_faults.c - Complete Array metadata allocation failure sweeps
 */
#include "base/xmalloc.h"
#include "lir/xlir_verify.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)

static size_t attempt, fail_at = SIZE_MAX, live, live_bytes;
typedef union ArrayAllocation {
    long double alignment;
    void *pointer;
    size_t bytes;
} ArrayAllocation;

static void *array_allocate(size_t bytes) {
    if (attempt++ == fail_at || bytes > SIZE_MAX - sizeof(ArrayAllocation)) return NULL;
    ArrayAllocation *allocation = xr_malloc(sizeof(*allocation) + bytes);
    if (!allocation) return NULL;
    allocation->bytes = bytes;
    ++live; live_bytes += bytes;
    return allocation + 1;
}
static void *array_callocate(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return NULL;
    void *memory = array_allocate(count * size);
    if (memory) memset(memory, 0, count * size);
    return memory;
}
static void array_free(void *memory) {
    if (!memory) return;
    ArrayAllocation *allocation = (ArrayAllocation *)memory - 1;
    CHECK(live && live_bytes >= allocation->bytes);
    --live; live_bytes -= allocation->bytes;
    xr_free(allocation);
}

#undef xr_malloc
#undef xr_calloc
#undef xr_free
#define xr_malloc(bytes) array_allocate(bytes)
#define xr_calloc(count, size) array_callocate(count, size)
#define xr_free(memory) array_free(memory)
#include "lir/xlir.c"
#include "lir/xlir_rebuild.c"
#include "lir/xlir_inline.c"
#include "lir_array_fixture.h"

static void array_construction_sweep(XrLirType element) {
    XrLirModule *m = NULL;
    attempt = 0; fail_at = SIZE_MAX;
    OK(array_fixture_create(element, &m));
    size_t count = attempt;
    xr_lir_module_destroy(m);
    CHECK(!live && !live_bytes);
    for (size_t point = 0; point < count; ++point) {
        m = NULL; attempt = 0; fail_at = point;
        CHECK(array_fixture_create(element, &m) == XR_LIR_OUT_OF_MEMORY);
        CHECK(!m && attempt > point && !live && !live_bytes);
    }
    fail_at = SIZE_MAX;
    printf("Array element %u construction failure points: %zu\n", (unsigned)element, count);
}

static void array_catalog_sweep(void) {
    XrLirModule *m = NULL;
    attempt = 0; fail_at = SIZE_MAX;
    OK(xr_lir_array_module_create(NULL, &m));
    size_t count = attempt;
    OK(xr_lir_module_finish(m, NULL));
    xr_lir_module_destroy(m);
    CHECK(!live && !live_bytes);
    for (size_t point = 0; point < count; ++point) {
        m = NULL; attempt = 0; fail_at = point;
        CHECK(xr_lir_array_module_create(NULL, &m) == XR_LIR_OUT_OF_MEMORY);
        CHECK(!m && attempt > point && !live && !live_bytes);
    }
    fail_at = SIZE_MAX;
    printf("Array catalog failure points: %zu\n", count);
}

static void array_transform_sweep(bool inlining) {
    XrLirModule *input = NULL, *output = NULL;
    OK(array_fixture_create(XR_LIR_STRING, &input));
    size_t baseline = live, bytes = live_bytes;
    attempt = 0;
    OK(inlining ? xr_lir_inline(input, &output) : xr_lir_rebuild(input, &output));
    size_t count = attempt;
    xr_lir_module_destroy(output);
    CHECK(live == baseline && live_bytes == bytes);
    for (size_t point = 0; point < count; ++point) {
        output = NULL; attempt = 0; fail_at = point;
        XrLirStatus status = inlining ? xr_lir_inline(input, &output) : xr_lir_rebuild(input, &output);
        CHECK(status == XR_LIR_OUT_OF_MEMORY && !output && attempt > point);
        CHECK(live == baseline && live_bytes == bytes);
        CHECK(xr_lir_module_is_readable(input));
        OK(xr_lir_verify(input, NULL));
    }
    fail_at = SIZE_MAX;
    xr_lir_module_destroy(input);
    CHECK(!live && !live_bytes);
    printf("Array %s failure points: %zu\n", inlining ? "inline" : "rebuild", count);
}

int main(void) {
    array_catalog_sweep();
    array_construction_sweep(XR_LIR_I64);
    array_construction_sweep(XR_LIR_STRING);
    array_transform_sweep(false);
    array_transform_sweep(true);
    puts("Array metadata construction and transform allocation sweeps passed");
    return 0;
}
