/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_compile_owner.h - Finite compiler ownership and physical fault replays
 */
#ifndef XIR_LIBRARY_COMPILE_OWNER_H
#define XIR_LIBRARY_COMPILE_OWNER_H
#include "xir/xxir_compile_context.h"
static inline XrXirStatus library_compile_resource_status(XrCompileResourceStatus status) {
    switch(status) {
    case XR_COMPILE_RESOURCE_OK:return XR_XIR_OK;
    case XR_COMPILE_RESOURCE_MEMORY_LIMIT:return XR_XIR_MEMORY_LIMIT;
    case XR_COMPILE_RESOURCE_OUT_OF_MEMORY:return XR_XIR_OUT_OF_MEMORY;
    case XR_COMPILE_RESOURCE_OVERFLOW:return XR_XIR_OVERFLOW;
    case XR_COMPILE_RESOURCE_CANCELLED:return XR_XIR_CANCELLED;
    case XR_COMPILE_RESOURCE_DEADLINE:return XR_XIR_DEADLINE;
    case XR_COMPILE_RESOURCE_CLOCK_FAILURE:return XR_XIR_CLOCK_FAILURE;
    default:return XR_XIR_BAD_STRUCTURE;
    }
}
typedef XrXirStatus (*LibraryCompileOperation)(const XrXirCompileContext *,void *);
XR_FUNC void library_compile_operation_cases(const char *name,LibraryCompileOperation operation,void *fixture);
#if !defined(CONSUMER_KIND) || CONSUMER_KIND!=3
#include "xir_source_program_compile_owner.h"
typedef struct LibraryCompileOwner {
    XrXirCompileContext context;
    XrCompileResourceStats baseline;
    size_t blocks,bytes;
} LibraryCompileOwner;
static const XrCompileResourceLimits library_compile_limits={XR_COMPILE_MEMORY_FINITE,UINT64_C(8388608),true};
static XrCompileResourceStats library_compile_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static XrXirStatus library_compile_owner_new(LibraryCompileOwner *owner,const XrCompileResourceLimits *limits) {
    CHECK(!owner->context.resources && source_program_compile_fail_at==SIZE_MAX);
    owner->blocks=source_program_compile_live;owner->bytes=source_program_compile_bytes;
    XrCompileResourceStatus status=xr_compile_resources_new(limits,&owner->context.resources);
    if(status!=XR_COMPILE_RESOURCE_OK)return library_compile_resource_status(status);
    owner->context.limits=xr_xir_compile_default_limits();owner->baseline=library_compile_stats(&owner->context);return XR_XIR_OK;
}
static void library_compile_owner_drop(LibraryCompileOwner *owner) {
    CHECK(source_program_compile_fail_at==SIZE_MAX);
    if(owner->context.resources){CHECK(library_compile_stats(&owner->context).held_bytes==owner->baseline.held_bytes);xr_compile_resources_release(owner->context.resources);}
    CHECK(source_program_compile_live==owner->blocks && source_program_compile_bytes==owner->bytes);*owner=(LibraryCompileOwner){0};
}
XR_FUNC void library_compile_operation_cases(const char *name,LibraryCompileOperation operation,void *fixture) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    source_program_compile_attempts=0;CHECK(operation(&owner.context,fixture)==XR_XIR_OK);size_t sites=source_program_compile_attempts;CHECK(sites);
    XrCompileResourceStats required=library_compile_stats(&owner.context);CHECK(required.held_bytes==owner.baseline.held_bytes);library_compile_owner_drop(&owner);
    for(size_t site=0;site<sites;++site){
        CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
        source_program_compile_attempts=0;source_program_compile_fail_at=site;source_program_compile_injected=false;
        XrXirStatus status=operation(&owner.context,fixture);source_program_compile_fail_at=SIZE_MAX;
        if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"%s compiler fault %zu/%zu status%u\n",name,site,sites,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && source_program_compile_injected);library_compile_owner_drop(&owner);
    }
    for(unsigned boundary=0;boundary<3;++boundary){
        XrCompileResourceLimits limits=library_compile_limits;CHECK(required.peak_bytes>0 && required.peak_bytes<UINT64_MAX);
        limits.memory_limit=required.peak_bytes-1+boundary;
        XrXirStatus status=library_compile_owner_new(&owner,&limits);if(status==XR_XIR_OK)status=operation(&owner.context,fixture);
        XrXirStatus expected=boundary==0?XR_XIR_MEMORY_LIMIT:XR_XIR_OK;
        if(status!=expected)fprintf(stderr,"%s held boundary%u exact%llu status%u\n",name,boundary,(unsigned long long)required.peak_bytes,status);
        CHECK(status==expected);library_compile_owner_drop(&owner);
    }
    fprintf(stderr,"%s compilerOOM=%zu observed allocated=%llu peak=%llu allocations=%llu held-minus/exact/plus/physicalbaseline PASS\n",name,sites,(unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,(unsigned long long)required.allocation_count);
}
static void library_compile_observer_free(void) {
    CHECK(!source_program_compile_live&&!source_program_compile_bytes);
    source_program_owners_free();
}
#endif
#endif
