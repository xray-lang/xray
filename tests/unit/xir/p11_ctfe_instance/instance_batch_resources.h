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
    if (query) CHECK(xr_xir_compile_ctfe_instance_batch(&request,NULL,&owner)==XR_XIR_CTFE_OK);
    XirCtfeInstanceView views[2]={0};XrCompileResourceStats before=stats(&context);size_t first=source_fixture_compile_attempts;
    XrXirCtfeStatus status=query?xr_xir_compile_ctfe_instance_batch_views(owner,views,2, NULL):xr_xir_compile_ctfe_instance_batch(&request,NULL,&owner);
    CHECK(status==XR_XIR_CTFE_OK);XrCompileResourceStats after=stats(&context);
    InstanceCost cost={after.allocated_bytes-before.allocated_bytes,source_fixture_compile_attempts-first};CHECK(cost.sites);
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
            if (query) CHECK(xr_xir_compile_ctfe_instance_batch(&request,NULL,&owner)==XR_XIR_CTFE_OK);
            XirCtfeInstanceView views[2]={0};XrCompileResourceStats before=stats(&context);size_t first=source_fixture_compile_attempts;
            source_fixture_compile_injected=false;source_fixture_compile_fail_at=first+fault;
            XrXirCtfeStatus status=query?xr_xir_compile_ctfe_instance_batch_views(owner,views,2, NULL):xr_xir_compile_ctfe_instance_batch(&request,NULL,&owner);
            if (status!=XR_XIR_CTFE_OUT_OF_MEMORY) fprintf(stderr,"Batch FI %s query=%u point=%zu/%zu status=%u\n",kind,query,fault,cost.sites,status);
            CHECK(status==XR_XIR_CTFE_OUT_OF_MEMORY && source_fixture_compile_injected);
            CHECK(source_fixture_compile_attempts==first+fault+1 && stats(&context).held_bytes==before.held_bytes);
            CHECK(query?(!views[0].checked && !views[1].checked && !views[0].value && !views[1].value):!owner);
            source_fixture_compile_fail_at=SIZE_MAX;
            status=query?xr_xir_compile_ctfe_instance_batch_views(owner,views,2, NULL):xr_xir_compile_ctfe_instance_batch(&request,NULL,&owner);
            CHECK(status==XR_XIR_CTFE_OK);
            {
                if (!query) CHECK(xr_xir_compile_ctfe_instance_batch_views(owner,views,2, NULL)==XR_XIR_CTFE_OK);
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
    if (prepared!=XR_XIR_OK) return p11_ctfe_from_xir(prepared);
    uint32_t functions[2]={instance_named(closed,kind,0),instance_named(closed,kind,1)};
    XirCtfeInstanceBatchRequest request=instance_batch_request(closed,functions,2);XirCtfeInstanceBatch *owner=NULL;
    XrXirCtfeStatus status=xr_xir_compile_ctfe_instance_batch(&request,NULL,&owner);XirCtfeInstanceView views[2]={0};
    if (status==XR_XIR_CTFE_OK) status=xr_xir_compile_ctfe_instance_batch_views(owner,views,2, NULL);
    xr_xir_compile_ctfe_instance_batch_free(owner);xr_xir_compile_artifact_free(closed);return status;
}
static void instance_batch_bounds(const char *kind) {
    XrXirCompileContext census;context_new(&census);XrCompileResourceStats before=stats(&census);
    CHECK(instance_batch_job(&census,kind)==XR_XIR_CTFE_OK);XrCompileResourceStats used=stats(&census);
    CHECK(used.held_bytes==before.held_bytes);xr_compile_resources_release(census.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=caps;
        limits.memory_limit=used.peak_bytes-minus;
        XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits();XrCompileResourceStats initial=stats(&context);XrCompileResources *same=context.resources;
        XrXirCtfeStatus status=instance_batch_job(&context,kind);
        CHECK(status==(minus?XR_XIR_CTFE_MEMORY_LIMIT:XR_XIR_CTFE_OK) && stats(&context).held_bytes==initial.held_bytes);
        if (minus) CHECK(instance_batch_job(&context,kind)==XR_XIR_CTFE_MEMORY_LIMIT && stats(&context).held_bytes==initial.held_bytes);
        CHECK(context.resources==same);xr_compile_resources_release(context.resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    fprintf(stderr,"Instance batch bounds %s held exact/minus1 original-owner physical0 allocated=%llu peak=%llu\n",
        kind,(unsigned long long)used.allocated_bytes,(unsigned long long)used.peak_bytes);
}
