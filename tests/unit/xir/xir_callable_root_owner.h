/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_callable_root_owner.h - Physical and typed resource observations
 */
#ifndef XIR_CALLABLE_ROOT_OWNER_H
#define XIR_CALLABLE_ROOT_OWNER_H
#include "xir/xxir_internal.h"
#include "base/xmalloc.h"
typedef struct BoundAllocation { void *pointer; size_t bytes; } BoundAllocation;
typedef struct BoundMark { size_t count, bytes; } BoundMark;
static BoundAllocation bound_allocations[32768];
static size_t bound_count, bound_bytes, bound_attempts, bound_fail_at=SIZE_MAX;
static bool bound_injected;
static XrCompileResourceStats bound_creation;
static BoundMark bound_mark(void) { return (BoundMark){bound_count,bound_bytes}; }
static void bound_balanced(BoundMark mark) { CHECK(bound_count==mark.count && bound_bytes==mark.bytes); }
static void *bound_malloc(size_t bytes) {
    if(bound_attempts++==bound_fail_at) { bound_injected=true; return NULL; }
    void *pointer=xr_malloc(bytes);
    if(pointer) {
        CHECK(bound_count<32768 && bytes<=SIZE_MAX-bound_bytes);
        bound_allocations[bound_count++]=(BoundAllocation){pointer,bytes}; bound_bytes+=bytes;
    }
    return pointer;
}
static void bound_free(void *pointer) {
    if(!pointer)return;
    size_t i=0;while(i<bound_count && bound_allocations[i].pointer!=pointer)++i;
    CHECK(i<bound_count && bound_allocations[i].bytes<=bound_bytes);
    bound_bytes-=bound_allocations[i].bytes;
    bound_allocations[i]=bound_allocations[--bound_count];xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) bound_malloc(bytes)
#define xr_free(pointer) bound_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
static XrCompileResourceLimits bound_caps(void) {
    return (XrCompileResourceLimits){UINT64_C(67108864),UINT64_C(8388608),UINT64_C(128000000)};
}
static XrXirCompileContext bound_owner(XrCompileResourceLimits caps) {
    XrCompileResourceLimits ceiling=bound_caps();
    CHECK(caps.allocated_bytes<=ceiling.allocated_bytes && caps.live_bytes<=ceiling.live_bytes && caps.work<=ceiling.work);
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(context.resources,&bound_creation)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();return context;
}
static XrCompileResourceStats bound_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static void bound_owner_free(XrXirCompileContext *context) {
    CHECK(bound_stats(context).live_bytes==bound_creation.live_bytes);xr_compile_resources_release(context->resources);
    *context=(XrXirCompileContext){0};
}
#endif // XIR_CALLABLE_ROOT_OWNER_H
