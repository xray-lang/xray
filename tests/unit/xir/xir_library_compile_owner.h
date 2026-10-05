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
typedef XrXirStatus (*LibraryCompileOperation)(const XrXirCompileContext *,void *);
XR_FUNC void library_compile_operation_cases(const char *name,LibraryCompileOperation operation,void *fixture);
#if !defined(CONSUMER_KIND) || CONSUMER_KIND!=3
#include "xir_source_program_compile_owner.h"
typedef struct LibraryCompileOwner {
    XrXirCompileContext context;
    XrCompileResourceStats baseline;
    size_t blocks,bytes;
} LibraryCompileOwner;
static const XrCompileResourceLimits library_compile_limits={UINT64_C(67108864),UINT64_C(8388608),UINT64_C(128000000)};
static XrCompileResourceStats library_compile_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static XrXirStatus library_compile_owner_new(LibraryCompileOwner *owner,const XrCompileResourceLimits *limits) {
    CHECK(!owner->context.resources && source_program_compile_fail_at==SIZE_MAX);
    owner->blocks=source_program_compile_live;owner->bytes=source_program_compile_bytes;
    XrCompileResourceStatus status=xr_compile_resources_new(limits,&owner->context.resources);
    if(status!=XR_COMPILE_RESOURCE_OK)return status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    owner->context.limits=xr_xir_compile_default_limits();owner->baseline=library_compile_stats(&owner->context);return XR_XIR_OK;
}
static void library_compile_owner_drop(LibraryCompileOwner *owner) {
    CHECK(source_program_compile_fail_at==SIZE_MAX);
    if(owner->context.resources){CHECK(library_compile_stats(&owner->context).live_bytes==owner->baseline.live_bytes);xr_compile_resources_release(owner->context.resources);}
    CHECK(source_program_compile_live==owner->blocks && source_program_compile_bytes==owner->bytes);*owner=(LibraryCompileOwner){0};
}
XR_FUNC void library_compile_operation_cases(const char *name,LibraryCompileOperation operation,void *fixture) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    source_program_compile_attempts=0;CHECK(operation(&owner.context,fixture)==XR_XIR_OK);size_t sites=source_program_compile_attempts;CHECK(sites);
    XrCompileResourceStats required=library_compile_stats(&owner.context);CHECK(required.live_bytes==owner.baseline.live_bytes);library_compile_owner_drop(&owner);
    for(size_t site=0;site<sites;++site){
        CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
        source_program_compile_attempts=0;source_program_compile_fail_at=site;source_program_compile_injected=false;
        XrXirStatus status=operation(&owner.context,fixture);source_program_compile_fail_at=SIZE_MAX;
        if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"%s compiler fault %zu/%zu status%u\n",name,site,sites,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && source_program_compile_injected);library_compile_owner_drop(&owner);
    }
    for(unsigned axis=0;axis<3;++axis)for(unsigned less=0;less<2;++less){
        XrCompileResourceLimits limits=library_compile_limits;uint64_t exact=axis==0?required.allocated_bytes:axis==1?required.peak_bytes:required.work;
        uint64_t *cap=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;CHECK(exact);*cap=exact-less;
        XrXirStatus status=library_compile_owner_new(&owner,&limits);if(status==XR_XIR_OK)status=operation(&owner.context,fixture);
        if(status!=(less?XR_XIR_BUDGET:XR_XIR_OK))fprintf(stderr,"%s axis%u less%u exact%llu status%u\n",name,axis,less,(unsigned long long)exact,status);
        CHECK(status==(less?XR_XIR_BUDGET:XR_XIR_OK));library_compile_owner_drop(&owner);
    }
    fprintf(stderr,"%s compilerOOM=%zu allocated=%llu peak=%llu work=%llu threeaxes/physicalbaseline PASS\n",name,sites,(unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,(unsigned long long)required.work);
}
static void library_compile_observer_free(void) {
    CHECK(!source_program_compile_live&&!source_program_compile_bytes);
    source_program_owners_free();
}
#endif
#endif
