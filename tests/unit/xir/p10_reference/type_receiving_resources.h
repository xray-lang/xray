/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * type_receiving_resources.h - Physical failures and exact receiving bounds
 */
#ifndef TYPE_RECEIVING_RESOURCES_H
#define TYPE_RECEIVING_RESOURCES_H
static XrXirValueStatus receiving_trial(const XrCompileResourceLimits *limits,
    XrCompileResourceStats *stats) {
    instance_compile_zero();
    XrCompileResources *resources=NULL;
    XrCompileResourceStatus opened=xr_compile_resources_new(limits,&resources);
    if (opened!=XR_COMPILE_RESOURCE_OK) {
        CHECK(!resources);instance_compile_zero();
        return opened==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_VALUE_OOM : XR_XIR_VALUE_LIMIT;
    }
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCompileResourceStats baseline={0};
    CHECK(xr_compile_resources_stats(resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL,.element=type(0)}};
    XrXirTypeNode shifted[]={{.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_BOOL},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_CELL,.element=type(1)}};
    XrXirTypes pools[]={{nodes,2,NULL,NULL},{shifted,3,NULL,NULL}};
    XrXirTypeArena *source=NULL,*target=NULL;
    XrXirValueStatus status=xr_xir_compile_type_arena_new(&context,&pools[0],&source);
    if (status==XR_XIR_VALUE_OK) status=xr_xir_compile_type_arena_new(&context,&pools[1],&target);
    if (status==XR_XIR_VALUE_OK) {
        XirTypeReceivingRoot root={type(1),type(2)};
        status=xir_compile_type_arena_receive(&context,target,source,&root,1);
        CHECK(xir_type_arena_same_type(source,type(1),target,type(2))==(status==XR_XIR_VALUE_OK));
    }
    CHECK(xr_compile_resources_stats(resources,stats)==XR_COMPILE_RESOURCE_OK);
    xr_xir_compile_type_arena_drop(target);xr_xir_compile_type_arena_drop(source);
    XrCompileResourceStats after={0};
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(resources);instance_compile_zero();return status;
}
static void receiving_resources(void) {
    const XrCompileResourceLimits bounds={64*1024*1024,8*1024*1024,UINT64_C(128000000)};
    XrCompileResourceStats normal={0},observed={0};
    instance_compile_attempts=0;
    CHECK(receiving_trial(&bounds,&normal)==XR_XIR_VALUE_OK);
    size_t sites=instance_compile_attempts;CHECK(sites && sites<20000);
    for (size_t i=0;i<sites;++i) {
        instance_compile_attempts=0;instance_compile_fail_at=i;instance_compile_injected=false;
        CHECK(receiving_trial(&bounds,&observed)==XR_XIR_VALUE_OOM && instance_compile_injected);
    }
    instance_compile_fail_at=SIZE_MAX;
    const XrCompileResourceLimits exact={normal.allocated_bytes,normal.peak_bytes,normal.work};
    CHECK(receiving_trial(&exact,&observed)==XR_XIR_VALUE_OK);
    CHECK(observed.allocated_bytes==normal.allocated_bytes && observed.peak_bytes==normal.peak_bytes && observed.work==normal.work);
    for (unsigned axis=0;axis<3;++axis) {
        XrCompileResourceLimits short_limit=exact;
        if (!axis) --short_limit.allocated_bytes;
        else if (axis==1) --short_limit.live_bytes;
        else --short_limit.work;
        CHECK(receiving_trial(&short_limit,&observed)==XR_XIR_VALUE_LIMIT);
    }
    printf("TYPE_RECEIVING_FI sites=%zu allocated=%llu peak=%llu work=%llu exact=PASS minus_one=3/3 physical=0/0\n",
        sites,(unsigned long long)normal.allocated_bytes,(unsigned long long)normal.peak_bytes,(unsigned long long)normal.work);
}
#endif // TYPE_RECEIVING_RESOURCES_H
