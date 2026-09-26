/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_allocations.c - Allocation failure and physical release witnesses
 *
 * KEY CONCEPT:
 *   Compile the actual implementation against counted allocator wrappers and
 *   fail each allocation in turn, including post-transition verification.
 */

#include "base/xmalloc.h"
#include "xir/xxir.h"
#include <stdlib.h>

static const XrXirTarget fixture_target = {XR_XIR_ARCH_X86_64, XR_XIR_SCALAR_ABI_VERSION};

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "allocation check failed at line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static size_t calls, live, fail_at = SIZE_MAX;

static void *counted_malloc(size_t size) {
    if (calls++ == fail_at)
        return NULL;
    void *pointer = xr_malloc(size);
    if (pointer)
        ++live;
    return pointer;
}

static void *counted_calloc(size_t count, size_t size) {
    if (calls++ == fail_at)
        return NULL;
    void *pointer = xr_calloc(count, size);
    if (pointer)
        ++live;
    return pointer;
}

static void counted_free(void *pointer) {
    if (pointer) {
        CHECK(live > 0);
        --live;
    }
    xr_free(pointer);
}

static void *counted_realloc(void *pointer, size_t size) {
    if (calls++ == fail_at)
        return NULL;
    bool was_null = pointer == NULL;
    void *replacement = xr_realloc(pointer, size);
    if (replacement && was_null)
        ++live;
    return replacement;
}

#undef xr_malloc
#undef xr_calloc
#undef xr_free
#undef xr_realloc
#define xr_malloc(size) counted_malloc(size)
#define xr_calloc(count, size) counted_calloc(count, size)
#define xr_free(pointer) counted_free(pointer)
#define xr_realloc(pointer, size) counted_realloc(pointer, size)

#include "xir/xxir.c"
#include "xir/xxir_verify.c"
#include "xir/xxir_layout.c"
#include "xir/xxir_scalar.c"
#include "xir/xxir_vm.c"
#include "xir/xxir_emit_c.c"

int main(void) {
    XrXirInstruction ops[] = {
        {XR_XIR_CONST_I64, XR_XIR_I64, {0, 0}, {0, 0}, 42},
        {XR_XIR_COPY, XR_XIR_I64, {0, 0}, {0, 0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1, 0}, {0, 0}, 0},
    };
    XrXirBlock block = {0, 3};
    XrXirType parameter = XR_XIR_I64;
    XrXirFunction functions[] = {
        {"first", 5, NULL, 0, XR_XIR_I64, &block, 1, ops, 3},
        {"second", 6, &parameter, 1, XR_XIR_I64, &block, 1, ops, 3},
    };
    XrXirModule module = {XR_XIR_BUILT, functions, 2};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    XrXirBudget exact = xr_xir_default_budget();
    exact.metadata_bytes = sizeof(XrXirArtifact);
    for (size_t i = 0; i < 2; ++i)
        exact.metadata_bytes += sizeof(XrXirFunction) + functions[i].name_length +
            functions[i].parameter_count * sizeof(XrXirType) +
            sizeof(XrXirBlock) + 3 * sizeof(XrXirInstruction);
    --exact.metadata_bytes;
    CHECK(xr_xir_check(&module, &exact, &checked, NULL) == XR_XIR_BUDGET);
    CHECK(!checked && live == 0);
    ++exact.metadata_bytes;
    CHECK(xr_xir_check(&module, &exact, &checked, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    CHECK(live == 0);
    calls = 0;
    CHECK(xr_xir_check(&module, NULL, &checked, NULL) == XR_XIR_OK);
    size_t check_calls = calls;
    CHECK(live > 0);
    xr_xir_artifact_free(checked);
    CHECK(live == 0);
    for (size_t i = 0; i < check_calls; ++i) {
        calls = 0;
        fail_at = i;
        checked = NULL;
        CHECK(xr_xir_check(&module, NULL, &checked, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(checked == NULL);
        CHECK(live == 0);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_check(&module, NULL, &checked, NULL) == XR_XIR_OK);
    size_t checked_live = live;
    calls = 0;
    CHECK(xr_xir_lower(checked, &fixture_target, NULL, &lowered, NULL) == XR_XIR_OK);
    size_t lower_calls = calls;
    xr_xir_artifact_free(lowered);
    CHECK(live == checked_live);
    for (size_t i = 0; i < lower_calls; ++i) {
        calls = 0;
        fail_at = i;
        lowered = NULL;
        CHECK(xr_xir_lower(checked, &fixture_target, NULL, &lowered, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(lowered == NULL);
        CHECK(live == checked_live);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_verify(xr_xir_artifact_module(checked), NULL, NULL) == XR_XIR_OK);
    CHECK(xr_xir_lower(checked, &fixture_target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    size_t lowered_live = live;
    calls = 0;
    XrXirRunContext context = {3, 16, 0, 0, 0, 0};
    XrXirScalar result;
    CHECK(xr_xir_vm_run(lowered, 0, &context, NULL, 0, &result) == XR_XIR_RUN_OK);
    CHECK(result.payload == 42 && live == lowered_live);
    size_t run_calls = calls;
    for (size_t i = 0; i < run_calls; ++i) {
        calls = 0;
        fail_at = i;
        context = (XrXirRunContext) {3, 16, 0, 0, 0, 0};
        result = (XrXirScalar) {99, 99, 99};
        CHECK(xr_xir_vm_run(lowered, 0, &context, NULL, 0, &result) == XR_XIR_RUN_OUT_OF_MEMORY);
        CHECK(result.type == 0 && result.reserved == 0 && result.payload == 0);
        CHECK(context.live_bytes == 0 && context.allocations == context.frees);
        CHECK(live == lowered_live);
    }
    fail_at = SIZE_MAX;
    calls = 0;
    XrXirCSource source;
    CHECK(xr_xir_emit_c(lowered, "allocation", 65536, &source) == XR_XIR_OK);
    size_t emit_calls = calls;
    xr_xir_c_source_free(&source);
    CHECK(live == lowered_live);
    for (size_t i = 0; i < emit_calls; ++i) {
        calls = 0;
        fail_at = i;
        CHECK(xr_xir_emit_c(lowered, "allocation", 65536, &source) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!source.text && !source.length && live == lowered_live);
    }
    fail_at = SIZE_MAX;
    xr_xir_artifact_free(lowered);
    CHECK(live == 0);
    printf("XIR physical release passed at %zu check, %zu lower, %zu VM, %zu emit allocation sites\n",
           check_calls, lower_calls, run_calls, emit_calls);
    return 0;
}
