/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * instance_batch_resources.h - Atomic batch allocation prefixes and owner bounds
 */
static InstanceCost instance_batch_cost(const char *kind,bool query) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *closed=instance_source(&context,kind);
    uint32_t functions[2]={instance_named(closed,kind,0),instance_named(closed,kind,1)};
    XirCtfeInstanceBatchRequest request=instance_batch_request(closed,functions,2);XirCtfeInstanceBatch *owner=NULL;
    if (query) CHECK(xr_xir_compile_ctfe_instance_batch(&request,&owner)==XR_XIR_CTFE_OK);
    XirCtfeInstanceView views[2]={0};XrCompileResourceStats before=stats(&context);size_t first=source_fixture_compile_attempts;
    XrXirCtfeStatus status=query?xr_xir_compile_ctfe_instance_batch_views(owner,views,2):xr_xir_compile_ctfe_instance_batch(&request,&owner);
    CHECK(status==XR_XIR_CTFE_OK);XrCompileResourceStats after=stats(&context);
    InstanceCost cost={after.work-before.work,after.allocated_bytes-before.allocated_bytes,source_fixture_compile_attempts-first};CHECK(cost.sites);
    xr_xir_compile_ctfe_instance_batch_free(owner);xr_xir_compile_artifact_free(closed);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);return cost;
}
static void instance_batch_faults(const char *kind,bool prefix) {
    for (unsigned query=0;query<2;++query) {
        InstanceCost cost=instance_batch_cost(kind,query!=0);size_t trials=prefix?2:cost.sites;
        for (size_t trial=0;trial<trials;++trial) {
            size_t fault=prefix?(trial?cost.sites-1:0):trial;XrXirCompileContext context;context_new(&context);
            XrCompileResources *same=context.resources;XrXirArtifact *closed=instance_source(&context,kind);
            uint32_t functions[2]={instance_named(closed,kind,0),instance_named(closed,kind,1)};
            XirCtfeInstanceBatchRequest request=instance_batch_request(closed,functions,2);XirCtfeInstanceBatch *owner=NULL;
            if (query) CHECK(xr_xir_compile_ctfe_instance_batch(&request,&owner)==XR_XIR_CTFE_OK);
            XirCtfeInstanceView views[2]={0};XrCompileResourceStats before=stats(&context);size_t first=source_fixture_compile_attempts;
            source_fixture_compile_injected=false;source_fixture_compile_fail_at=first+fault;
            XrXirCtfeStatus status=query?xr_xir_compile_ctfe_instance_batch_views(owner,views,2):xr_xir_compile_ctfe_instance_batch(&request,&owner);
            if (status!=XR_XIR_CTFE_OUT_OF_MEMORY) fprintf(stderr,"Batch FI %s query=%u point=%zu/%zu status=%u\n",kind,query,fault,cost.sites,status);
            CHECK(status==XR_XIR_CTFE_OUT_OF_MEMORY && source_fixture_compile_injected);
            CHECK(source_fixture_compile_attempts==first+fault+1 && stats(&context).live_bytes==before.live_bytes);
            CHECK(query?(!views[0].checked && !views[1].checked && !views[0].value && !views[1].value):!owner);
            source_fixture_compile_fail_at=SIZE_MAX;XrCompileResourceStats failed=stats(&context);
            bool exhausted=cost.work>caps.work-failed.work || cost.allocated>caps.allocated_bytes-failed.allocated_bytes;
            status=query?xr_xir_compile_ctfe_instance_batch_views(owner,views,2):xr_xir_compile_ctfe_instance_batch(&request,&owner);
            CHECK(status==(exhausted?XR_XIR_CTFE_BUDGET:XR_XIR_CTFE_OK));
            if (exhausted) {
                CHECK(query?(!views[0].checked && !views[1].checked):!owner);
                CHECK((query?xr_xir_compile_ctfe_instance_batch_views(owner,views,2):xr_xir_compile_ctfe_instance_batch(&request,&owner))==XR_XIR_CTFE_BUDGET);
            } else {
                CHECK(stats(&context).work-failed.work==cost.work && stats(&context).allocated_bytes-failed.allocated_bytes==cost.allocated);
                if (!query) CHECK(xr_xir_compile_ctfe_instance_batch_views(owner,views,2)==XR_XIR_CTFE_OK);
                CHECK(views[0].checked && views[1].checked && xr_xir_value_valid(views[0].value) && xr_xir_value_valid(views[1].value));
            }
            xr_xir_compile_ctfe_instance_batch_free(owner);CHECK(context.resources==same);
            xr_xir_compile_artifact_free(closed);xr_compile_resources_release(context.resources);
            CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
            if (!(trial%256) || trial+1==trials) fprintf(stderr,"Batch FI %s query=%u actual=%zu/%zu same-owner/live physical0\n",kind,query,fault+1,cost.sites);
        }
        fprintf(stderr,"Instance batch %s query=%u %s actualFI=%zu selected=%zu\n",kind,query,prefix?"FIRST_LAST_ONLY":"COMPLETE_ALL_ORDINALS",cost.sites,trials);
    }
}
static XrXirCtfeStatus instance_batch_job(XrXirCompileContext *context,const char *kind) {
    XrXirArtifact *closed=NULL;XrXirStatus prepared=instance_source_operation(context,&closed,kind);
    if (prepared!=XR_XIR_OK) return prepared==XR_XIR_BUDGET?XR_XIR_CTFE_BUDGET:
        prepared==XR_XIR_OUT_OF_MEMORY?XR_XIR_CTFE_OUT_OF_MEMORY:XR_XIR_CTFE_BAD_ARTIFACT;
    uint32_t functions[2]={instance_named(closed,kind,0),instance_named(closed,kind,1)};
    XirCtfeInstanceBatchRequest request=instance_batch_request(closed,functions,2);XirCtfeInstanceBatch *owner=NULL;
    XrXirCtfeStatus status=xr_xir_compile_ctfe_instance_batch(&request,&owner);XirCtfeInstanceView views[2]={0};
    if (status==XR_XIR_CTFE_OK) status=xr_xir_compile_ctfe_instance_batch_views(owner,views,2);
    xr_xir_compile_ctfe_instance_batch_free(owner);xr_xir_compile_artifact_free(closed);return status;
}
static void instance_batch_bounds(const char *kind) {
    XrXirCompileContext census;context_new(&census);XrCompileResourceStats before=stats(&census);
    CHECK(instance_batch_job(&census,kind)==XR_XIR_CTFE_OK);XrCompileResourceStats used=stats(&census);
    CHECK(used.live_bytes==before.live_bytes);xr_compile_resources_release(census.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=caps;
        if (!axis) limits.work=used.work-minus;else if (axis==1) limits.allocated_bytes=used.allocated_bytes-minus;else limits.live_bytes=used.peak_bytes-minus;
        XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits();XrCompileResourceStats initial=stats(&context);XrCompileResources *same=context.resources;
        XrXirCtfeStatus status=instance_batch_job(&context,kind);
        CHECK(status==(minus?XR_XIR_CTFE_BUDGET:XR_XIR_CTFE_OK) && stats(&context).live_bytes==initial.live_bytes);
        if (minus) CHECK(instance_batch_job(&context,kind)==XR_XIR_CTFE_BUDGET && stats(&context).live_bytes==initial.live_bytes);
        CHECK(context.resources==same);xr_compile_resources_release(context.resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    fprintf(stderr,"Instance batch bounds %s threeaxes exact/minus1 original-owner physical0 work=%llu allocated=%llu peak=%llu\n",
        kind,(unsigned long long)used.work,(unsigned long long)used.allocated_bytes,(unsigned long long)used.peak_bytes);
}
