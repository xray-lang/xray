/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * invocation_allocator.h - Physical accounting around the unique ledger
 */
#ifndef INVOCATION_ALLOCATOR_H
#define INVOCATION_ALLOCATOR_H
#include "base/xmalloc.h"
static void *invocation_fixture_malloc(size_t bytes) { return xr_malloc(bytes); }
static void invocation_fixture_free(void *pointer) { xr_free(pointer); }
typedef struct InvocationAllocation { void *pointer; size_t bytes; } InvocationAllocation;
enum { INVOCATION_ALLOCATION_SLOTS = 131072 };
static InvocationAllocation invocation_allocations[INVOCATION_ALLOCATION_SLOTS];
static size_t runtime_attempts, runtime_fail_at = SIZE_MAX, runtime_live, runtime_bytes;
static void *invocation_test_malloc(size_t bytes) {
    if (runtime_attempts++ == runtime_fail_at) return NULL;
    void *pointer = invocation_fixture_malloc(bytes); if (!pointer) return NULL;
    size_t index = ((uintptr_t)pointer >> 4) & (INVOCATION_ALLOCATION_SLOTS - 1);
    for (size_t i = 0; i < INVOCATION_ALLOCATION_SLOTS; ++i, index = (index + 1) & (INVOCATION_ALLOCATION_SLOTS - 1)) {
        InvocationAllocation *entry = &invocation_allocations[index];
        if (!entry->pointer || entry->pointer == (void *)(uintptr_t)UINTPTR_MAX) {
            *entry = (InvocationAllocation){pointer, bytes}; ++runtime_live; runtime_bytes += bytes; return pointer;
        }
    }
    CHECK(false); return NULL;
}
static void invocation_test_free(void *pointer) {
    if (!pointer) return;
    size_t index = ((uintptr_t)pointer >> 4) & (INVOCATION_ALLOCATION_SLOTS - 1);
    for (size_t i = 0; i < INVOCATION_ALLOCATION_SLOTS; ++i, index = (index + 1) & (INVOCATION_ALLOCATION_SLOTS - 1)) {
        InvocationAllocation *entry = &invocation_allocations[index];
        if (entry->pointer == pointer) {
            --runtime_live; runtime_bytes -= entry->bytes;
            *entry = (InvocationAllocation){(void *)(uintptr_t)UINTPTR_MAX, 0}; invocation_fixture_free(pointer); return;
        }
        if (!entry->pointer) break;
    }
    CHECK(false);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc invocation_test_malloc
#define xr_free invocation_test_free
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free
#define xr_malloc invocation_fixture_malloc
#define xr_free invocation_fixture_free
static const XrCompileResourceLimits sdk_unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
static XrCompileResources *sdk_ledger(const XrCompileResourceLimits *limits) {
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(limits, &resources) == XR_COMPILE_RESOURCE_OK); return resources;
}
#endif // INVOCATION_ALLOCATOR_H
