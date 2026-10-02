/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * namespace_allocator.h - Observe the real ledger allocator and work edges
 */
#ifndef NAMESPACE_ALLOCATOR_H
#define NAMESPACE_ALLOCATOR_H
#include "base/xmalloc.h"
static size_t runtime_bytes, physical_total, physical_peak;
static bool record_physical;
static void *namespace_physical_malloc(size_t bytes) {
    void *pointer = xr_malloc(bytes);
    if (pointer && record_physical) {
        physical_total += bytes;
        if (runtime_bytes + bytes > physical_peak) physical_peak = runtime_bytes + bytes;
    }
    return pointer;
}
#undef xr_malloc
#define xr_malloc namespace_physical_malloc
#define xr_compile_resources_work namespace_real_work
#define xr_compile_resources_alloc namespace_real_alloc
#define xr_compile_resources_calloc namespace_real_calloc
#define xr_compile_resources_resize namespace_real_resize
#include "../../xir/xir_sdk_resource_test.h"
#undef xr_compile_resources_work
#undef xr_compile_resources_alloc
#undef xr_compile_resources_calloc
#undef xr_compile_resources_resize
static bool record_work;
static uint64_t work_edges[65536];
static size_t work_count;
static XrCompileResourceStatus namespace_record_work(XrCompileResources *r, XrCompileResourceStatus status) {
    (void)sdk_fixture_malloc; (void)sdk_fixture_free;
    if (record_work && status == XR_COMPILE_RESOURCE_OK) {
        XrCompileResourceStats stats = sdk_stats(r);
        if (!work_count || work_edges[work_count - 1] != stats.work) {
            CHECK(work_count < 65536); work_edges[work_count++] = stats.work;
        }
    }
    return status;
}
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *r, uint64_t work) {
    return namespace_record_work(r, namespace_real_work(r, work));
}
XR_FUNC XrCompileResourceStatus xr_compile_resources_alloc(XrCompileResources *r, size_t bytes, void **out) {
    return namespace_record_work(r, namespace_real_alloc(r, bytes, out));
}
XR_FUNC XrCompileResourceStatus xr_compile_resources_calloc(XrCompileResources *r, size_t n, size_t bytes, void **out) {
    return namespace_record_work(r, namespace_real_calloc(r, n, bytes, out));
}
XR_FUNC XrCompileResourceStatus xr_compile_resources_resize(XrCompileResources *r, void **memory, size_t bytes) {
    return namespace_record_work(r, namespace_real_resize(r, memory, bytes));
}
#endif // NAMESPACE_ALLOCATOR_H
