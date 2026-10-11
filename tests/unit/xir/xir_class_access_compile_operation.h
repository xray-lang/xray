/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_access_compile_operation.h - Whole Source operations on one finite ledger
 */
#ifndef XIR_CLASS_ACCESS_COMPILE_OPERATION_H
#define XIR_CLASS_ACCESS_COMPILE_OPERATION_H
typedef struct AccessCompileTrial { const AccessSourceCase *fixture; AccessCompileCost single; } AccessCompileTrial;
static XrXirStatus access_compile_one(const XrXirCompileContext *context,const AccessSourceCase *fixture) {
    XrXirProgram *program=NULL;CellSourceEntries entries={UINT32_MAX,UINT32_MAX,UINT32_MAX};
    XrXirStatus status=cell_source_program(context,fixture->name,&program,&entries,false);
    if (status==XR_XIR_OK) {
        CHECK(!fixture->rejected && program);
        cell_runtime_fault_count=0;access_runtime_retry=false;
        AccessRuntimeCost runtime=access_runtime_operation(program,&entries,fixture,ACCESS_RUN,NULL,0);
        if (runtime.failure==XR_XIR_CALL_OOM) status=XR_XIR_OUT_OF_MEMORY;
        else if (runtime.failure==XR_XIR_CALL_LIMIT) status=XR_XIR_BUDGET;
        else CHECK(runtime.complete && runtime.failure==XR_XIR_CALL_READY);
    } else CHECK(!program);
    xr_xir_compile_program_drop(program);
    return status;
}
static void access_compile_endpoint(const AccessSourceCase *fixture,XrXirStatus status) {
    if (fixture->rejected)
        CHECK(status!=XR_XIR_OK && status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY);
    else CHECK(status==XR_XIR_OK);
}
static XrCompileResourceStats access_compile_stats(XrCompileResources *resources) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static XrXirStatus access_owner_new(const XrCompileResourceLimits *limits,XrXirCompileContext *context) {
    XrCompileResourceStatus status=xr_compile_resources_new(limits,&context->resources);
    if (status==XR_COMPILE_RESOURCE_OK) {context->limits=xr_xir_compile_default_limits();return XR_XIR_OK;}
    CHECK(!context->resources);
    return status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
        status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
}
static bool access_compile_fits(const AccessCompileTrial *trial,const XrCompileResourceLimits *limits,
    const XrCompileResourceStats *used) {
    const XrCompileResourceStats *single=&trial->single.stats,*initial=&trial->single.initial;
    CHECK(single->allocated_bytes>=initial->allocated_bytes && single->work>=initial->work);
    CHECK(used->allocated_bytes<=limits->allocated_bytes && used->work<=limits->work);
    return single->allocated_bytes-initial->allocated_bytes<=limits->allocated_bytes-used->allocated_bytes &&
        single->work-initial->work<=limits->work-used->work && single->peak_bytes<=limits->live_bytes;
}
static AccessCompileCost access_compile_operation(const AccessCompileTrial *trial,
    const XrCompileResourceLimits *selected,bool retry) {
    access_physical_zero();
    const XrCompileResourceLimits limits=selected?*selected:access_compile_limits();
    AccessCompileCost cost={0};XrXirCompileContext context={0};
    instance_compile_attempts=0;instance_compile_fail_at=SIZE_MAX;instance_compile_injected=false;
    access_compile_site_count=access_compile_hits=0;access_compile_recording=true;
    cost.status=access_owner_new(&limits,&context);
    if (cost.status==XR_XIR_OUT_OF_MEMORY && retry) {
        CHECK(access_compile_hits==1 && !context.resources);cost.first=cost.status;cost.bootstrap=true;
        cost.status=access_owner_new(&limits,&context);
    }
    if (cost.status!=XR_XIR_OK) goto finish;
    cost.initial=access_compile_stats(context.resources);
    size_t owned_blocks=instance_compile_live,owned_bytes=instance_compile_bytes;
    cost.status=access_compile_one(&context,trial->fixture);
    cost.stats=access_compile_stats(context.resources);
    CHECK(cost.stats.live_bytes==cost.initial.live_bytes && instance_compile_live==owned_blocks && instance_compile_bytes==owned_bytes);
    if (retry && cost.status==XR_XIR_OUT_OF_MEMORY) {
        CHECK(access_compile_hits==1 && context.resources);cost.first=cost.status;cost.same_owner=true;
        XrCompileResources *same=context.resources;XrCompileResourceStats failed=cost.stats;
        cost.fits=access_compile_fits(trial,&limits,&failed);
        size_t sites=instance_compile_attempts;
        cost.status=access_compile_one(&context,trial->fixture);cost.stats=access_compile_stats(same);
        CHECK(context.resources==same && cost.stats.live_bytes==cost.initial.live_bytes);
        CHECK(cost.stats.allocated_bytes>=failed.allocated_bytes && cost.stats.work>=failed.work);
        CHECK(instance_compile_live==owned_blocks && instance_compile_bytes==owned_bytes);
        if (cost.fits) {
            access_compile_endpoint(trial->fixture,cost.status);
            CHECK(cost.stats.work-failed.work==trial->single.stats.work-trial->single.initial.work &&
                cost.stats.allocated_bytes-failed.allocated_bytes==trial->single.stats.allocated_bytes-trial->single.initial.allocated_bytes &&
                instance_compile_attempts-sites==trial->single.sites-1);
        } else CHECK(cost.status==XR_XIR_BUDGET);
    }
    if (cost.status!=XR_XIR_BUDGET && cost.status!=XR_XIR_OUT_OF_MEMORY) {
        access_compile_endpoint(trial->fixture,cost.status);cost.complete=true;
    }
 finish:
    if (context.resources) {cost.stats=access_compile_stats(context.resources);xr_compile_resources_release(context.resources);}
    cost.sites=instance_compile_attempts;cost.hits=access_compile_hits;
    CHECK(access_compile_site_count==cost.sites);
    access_compile_recording=false;instance_compile_fail_at=SIZE_MAX;access_physical_zero();
    return cost;
}
#endif // XIR_CLASS_ACCESS_COMPILE_OPERATION_H
