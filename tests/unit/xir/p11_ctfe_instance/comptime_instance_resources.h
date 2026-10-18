/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * comptime_instance_resources.h - Actual Source occurrence allocation prefixes
 */
static XrXirStatus comptime_instances_job(XrXirCompileContext *context,const char *kind) {
    XirSourceComptimeInstances result={0};
    XrXirStatus status=comptime_instances_operation(context,comptime_instances_text(kind),&result,
        comptime_instances_limits(),XR_XIR_CTFE_MAX_CALL_DEPTH,false);
    XirCtfeInstanceView views[2]={0};XirSourceComptimeBinding bindings[2]={0};
    if (status==XR_XIR_OK) {
        CHECK(result.count==2);XrXirCtfeStatus queried=xr_xir_compile_source_comptime_instances_view(&result,views,bindings,2);
        status=queried==XR_XIR_CTFE_OK?XR_XIR_OK:queried==XR_XIR_CTFE_BUDGET?XR_XIR_BUDGET:
            queried==XR_XIR_CTFE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
    }
    xr_xir_compile_source_comptime_instances_free(&result);return status;
}
static InstanceCost comptime_instances_cost(const char *kind,bool query) {
    XrXirCompileContext context;context_new(&context);XirSourceComptimeInstances result={0};
    if (query) CHECK(comptime_instances_operation(&context,comptime_instances_text(kind),&result,
        comptime_instances_limits(),XR_XIR_CTFE_MAX_CALL_DEPTH,false)==XR_XIR_OK);
    XirCtfeInstanceView views[2]={0};XirSourceComptimeBinding bindings[2]={0};
    XrCompileResourceStats before=stats(&context);size_t first=source_fixture_compile_attempts;
    if (query) CHECK(xr_xir_compile_source_comptime_instances_view(&result,views,bindings,2)==XR_XIR_CTFE_OK);
    else CHECK(comptime_instances_operation(&context,comptime_instances_text(kind),&result,
        comptime_instances_limits(),XR_XIR_CTFE_MAX_CALL_DEPTH,false)==XR_XIR_OK);
    XrCompileResourceStats after=stats(&context);
    InstanceCost cost={after.work-before.work,after.allocated_bytes-before.allocated_bytes,source_fixture_compile_attempts-first};CHECK(cost.sites);
    xr_xir_compile_source_comptime_instances_free(&result);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);return cost;
}
static void comptime_instances_faults(const char *kind,bool prefix) {
    for (unsigned query=0;query<2;++query) {
        InstanceCost cost=comptime_instances_cost(kind,query!=0);size_t trials=prefix?2:cost.sites;
        for (size_t trial=0;trial<trials;++trial) {
            size_t fault=prefix?(trial?cost.sites-1:0):trial;XrXirCompileContext context;context_new(&context);
            XrCompileResources *same=context.resources;XirSourceComptimeInstances result={0};
            if (query) CHECK(comptime_instances_operation(&context,comptime_instances_text(kind),&result,
                comptime_instances_limits(),XR_XIR_CTFE_MAX_CALL_DEPTH,false)==XR_XIR_OK);
            XirCtfeInstanceView views[2]={0};XirSourceComptimeBinding bindings[2]={0};
            XrCompileResourceStats before=stats(&context);size_t first=source_fixture_compile_attempts;
            source_fixture_compile_injected=false;source_fixture_compile_fail_at=first+fault;
            XrXirStatus status=XR_XIR_OK;
            if (query) {
                CHECK(xr_xir_compile_source_comptime_instances_view(&result,views,bindings,2)==XR_XIR_CTFE_OUT_OF_MEMORY);
                CHECK(!views[0].checked && !views[1].checked && !bindings[0].helper && !bindings[1].helper);
            } else {
                status=comptime_instances_operation(&context,comptime_instances_text(kind),&result,
                    comptime_instances_limits(),XR_XIR_CTFE_MAX_CALL_DEPTH,false);
                if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"Typed comptime FI %s point=%zu/%zu status=%u\n",kind,fault,cost.sites,status);
                CHECK(status==XR_XIR_OUT_OF_MEMORY && !result.owner && !result.count);
            }
            CHECK(source_fixture_compile_injected && source_fixture_compile_attempts==first+fault+1 && stats(&context).live_bytes==before.live_bytes);
            source_fixture_compile_fail_at=SIZE_MAX;XrCompileResourceStats failed=stats(&context);
            bool exhausted=cost.work>caps.work-failed.work || cost.allocated>caps.allocated_bytes-failed.allocated_bytes;
            if (query) {
                XrXirCtfeStatus retried=xr_xir_compile_source_comptime_instances_view(&result,views,bindings,2);
                CHECK(retried==(exhausted?XR_XIR_CTFE_BUDGET:XR_XIR_CTFE_OK));
                if (exhausted) CHECK(xr_xir_compile_source_comptime_instances_view(&result,views,bindings,2)==XR_XIR_CTFE_BUDGET);
            } else {
                status=comptime_instances_operation(&context,comptime_instances_text(kind),&result,
                    comptime_instances_limits(),XR_XIR_CTFE_MAX_CALL_DEPTH,false);
                CHECK(status==(exhausted?XR_XIR_BUDGET:XR_XIR_OK));
                if (exhausted) CHECK(comptime_instances_operation(&context,comptime_instances_text(kind),&result,
                    comptime_instances_limits(),XR_XIR_CTFE_MAX_CALL_DEPTH,false)==XR_XIR_BUDGET && !result.owner);
            }
            if (!exhausted) CHECK(stats(&context).work-failed.work==cost.work && stats(&context).allocated_bytes-failed.allocated_bytes==cost.allocated);
            if (query) CHECK(stats(&context).live_bytes==before.live_bytes);
            xr_xir_compile_source_comptime_instances_free(&result);CHECK(context.resources==same);
            if (!query) CHECK(stats(&context).live_bytes==before.live_bytes);
            xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
            if (!(trial%256) || trial+1==trials) fprintf(stderr,"Typed comptime FI %s query=%u actual=%zu/%zu same-owner/live physical0\n",kind,query,fault+1,cost.sites);
        }
        fprintf(stderr,"Typed comptime %s query=%u %s actualFI=%zu selected=%zu\n",kind,query,prefix?"FIRST_LAST_ONLY":"COMPLETE_ALL_ORDINALS",cost.sites,trials);
    }
}
static void comptime_instances_bounds(const char *kind) {
    XrXirCompileContext census;context_new(&census);XrCompileResourceStats before=stats(&census);
    CHECK(comptime_instances_job(&census,kind)==XR_XIR_OK);XrCompileResourceStats used=stats(&census);
    CHECK(used.live_bytes==before.live_bytes);xr_compile_resources_release(census.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=caps;
        if (!axis) limits.work=used.work-minus;else if (axis==1) limits.allocated_bytes=used.allocated_bytes-minus;else limits.live_bytes=used.peak_bytes-minus;
        XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits();XrCompileResourceStats initial=stats(&context);XrCompileResources *same=context.resources;
        XrXirStatus status=comptime_instances_job(&context,kind);
        CHECK(status==(minus?XR_XIR_BUDGET:XR_XIR_OK) && stats(&context).live_bytes==initial.live_bytes);
        if (minus) CHECK(comptime_instances_job(&context,kind)==XR_XIR_BUDGET && stats(&context).live_bytes==initial.live_bytes);
        CHECK(context.resources==same);xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    fprintf(stderr,"Typed comptime bounds %s threeaxes exact/minus1 original-owner physical0 work=%llu allocated=%llu peak=%llu\n",
        kind,(unsigned long long)used.work,(unsigned long long)used.allocated_bytes,(unsigned long long)used.peak_bytes);
}
