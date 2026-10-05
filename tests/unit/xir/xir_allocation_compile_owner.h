/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_allocation_compile_owner.h - Physical finite compiler fault scopes
 *
 * KEY CONCEPT:
 *   The canonical allocator is counted, including its ledger and block headers.
 */
#ifndef XIR_ALLOCATION_COMPILE_OWNER_H
#define XIR_ALLOCATION_COMPILE_OWNER_H
#include "xir/xxir_compile_context.h"
typedef struct AllocationCompileOwner {
    XrXirCompileContext context;
    XrCompileResourceStats baseline;
    size_t blocks, bytes;
} AllocationCompileOwner;
static const XrCompileResourceLimits allocation_compile_limits={
    UINT64_C(67108864),UINT64_C(8388608),UINT64_C(128000000)};
static XrCompileResourceStats allocation_compile_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats;
}
static void allocation_compile_owner_new(AllocationCompileOwner *owner,const XrCompileResourceLimits *limits) {
    CHECK(!owner->context.resources && fail_at==SIZE_MAX);
    owner->blocks=live;owner->bytes=class_live_bytes;
    CHECK(xr_compile_resources_new(limits,&owner->context.resources)==XR_COMPILE_RESOURCE_OK);
    owner->context.limits=xr_xir_compile_default_limits();
    owner->baseline=allocation_compile_stats(&owner->context);
    CHECK(live==owner->blocks+1 && class_live_bytes>owner->bytes);
}
static void allocation_compile_owner_drop(AllocationCompileOwner *owner) {
    CHECK(fail_at==SIZE_MAX);
    XrCompileResourceStats stats=allocation_compile_stats(&owner->context);
    CHECK(stats.live_bytes==owner->baseline.live_bytes);
    xr_compile_resources_release(owner->context.resources);
    CHECK(live==owner->blocks && class_live_bytes==owner->bytes);
    *owner=(AllocationCompileOwner){0};
}
typedef XrXirStatus (*AllocationCompileOperation)(const XrXirCompileContext *,void *);
/* Operations return no escaping output; each actual fault replay owns a new ledger. */
static void allocation_compile_operation_cases(const char *name,AllocationCompileOperation operation,void *fixture) {
    AllocationCompileOwner owner={0};
    allocation_compile_owner_new(&owner,&allocation_compile_limits);
    calls=0;CHECK(operation(&owner.context,fixture)==XR_XIR_OK);
    size_t sites=calls;XrCompileResourceStats required=allocation_compile_stats(&owner.context);
    CHECK(required.live_bytes==owner.baseline.live_bytes);
    allocation_compile_owner_drop(&owner);
    for(size_t site=0;site<sites;++site) {
        allocation_compile_owner_new(&owner,&allocation_compile_limits);
        size_t baseline=live,bytes=class_live_bytes;
        calls=0;fail_at=site;
        XrXirStatus status=operation(&owner.context,fixture);
        fail_at=SIZE_MAX;
        if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"%s site %zu status %u\n",name,site,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && live==baseline && class_live_bytes==bytes);
        allocation_compile_owner_drop(&owner);
    }
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=allocation_compile_limits;
        uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        const uint64_t exact=axis==0?required.allocated_bytes:axis==1?required.peak_bytes:required.work;
        CHECK(exact>0);*bound=exact-minus;
        allocation_compile_owner_new(&owner,&limits);
        XrXirStatus status=operation(&owner.context,fixture);
        if(status!=(minus?XR_XIR_BUDGET:XR_XIR_OK))fprintf(stderr,"%s axis %u minus %u status %u exact %llu\n",name,axis,minus,status,(unsigned long long)exact);
        CHECK(status==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        allocation_compile_owner_drop(&owner);
    }
    printf("%s: %zu actual compiler faults, three-axis exact/minus-one, physical baseline restored\n",name,sites);
    calls=0;
}
#endif // XIR_ALLOCATION_COMPILE_OWNER_H
