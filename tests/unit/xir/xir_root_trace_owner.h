/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_trace_owner.h - Physical allocation and finite trace ledger probes
 */
#ifndef XIR_ROOT_TRACE_OWNER_H
#define XIR_ROOT_TRACE_OWNER_H
#include "xir/xxir_internal.h"
#include "base/xmalloc.h"
typedef struct TraceAllocation { void *pointer; size_t bytes; } TraceAllocation;
typedef struct TraceMark { size_t blocks, bytes; } TraceMark;
static TraceAllocation trace_blocks[32768];
static size_t trace_live, trace_live_bytes, trace_attempts, trace_fail_at = SIZE_MAX;
static bool trace_injected;
static TraceMark trace_mark(void) { return (TraceMark){trace_live,trace_live_bytes}; }
static void trace_balanced(TraceMark mark) {
    CHECK(trace_live == mark.blocks && trace_live_bytes == mark.bytes);
}
static void *trace_malloc(size_t bytes) {
    if (trace_attempts++ == trace_fail_at) { trace_injected = true; return NULL; }
    void *memory = xr_malloc(bytes);
    if (memory) {
        CHECK(trace_live < 32768 && bytes <= SIZE_MAX - trace_live_bytes);
        trace_blocks[trace_live++] = (TraceAllocation){memory,bytes}; trace_live_bytes += bytes;
    }
    return memory;
}
static void trace_free(void *memory) {
    if (!memory) return;
    size_t i = 0;
    while (i < trace_live && trace_blocks[i].pointer != memory) ++i;
    CHECK(i < trace_live && trace_blocks[i].bytes <= trace_live_bytes);
    trace_live_bytes -= trace_blocks[i].bytes;
    trace_blocks[i] = trace_blocks[--trace_live]; xr_free(memory);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) trace_malloc(bytes)
#define xr_free(memory) trace_free(memory)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
static XrCompileResourceLimits trace_caps(void) {
    return (XrCompileResourceLimits){UINT64_C(67108864),UINT64_C(8388608),UINT64_C(128000000)};
}
static XrCompileResourceStats trace_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(context->resources,&stats) == XR_COMPILE_RESOURCE_OK);
    return stats;
}
static XrXirCompileContext trace_owner(XrCompileResourceLimits caps) {
    XrCompileResourceLimits bound = trace_caps();
    CHECK(caps.allocated_bytes <= bound.allocated_bytes && caps.live_bytes <= bound.live_bytes && caps.work <= bound.work);
    XrXirCompileContext context = {0};
    CHECK(xr_compile_resources_new(&caps,&context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits(); return context;
}
static void trace_owner_free(XrXirCompileContext *context, uint64_t baseline) {
    CHECK(trace_stats(context).live_bytes == baseline);
    xr_compile_resources_release(context->resources); *context = (XrXirCompileContext){0};
}
static void trace_stats_equal(XrCompileResourceStats actual, XrCompileResourceStats expected) {
    CHECK(actual.allocation_count == expected.allocation_count && actual.allocated_bytes == expected.allocated_bytes &&
        actual.live_bytes == expected.live_bytes && actual.peak_bytes == expected.peak_bytes && actual.work == expected.work);
}
static bool trace_competing_charge;
static size_t trace_work_position, trace_work_cut_at=SIZE_MAX;
/* Admission does not reserve the shared ledger. A deterministic competing
 * owner spends its remaining allowance through the real resource API. */
static XrCompileResourceStatus trace_charged_work(XrCompileResources *resources, uint64_t units) {
    if(trace_competing_charge && trace_work_position++==trace_work_cut_at) {
        XrCompileResourceStats stats={0};
        CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_resources_work(resources,resources->limits.work-stats.work)==XR_COMPILE_RESOURCE_OK);
        trace_competing_charge=false;
    }
    return xr_compile_resources_work(resources,units);
}
#endif // XIR_ROOT_TRACE_OWNER_H
