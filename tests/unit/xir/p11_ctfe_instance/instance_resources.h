/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * instance_resources.h - Real allocation failures retain one finite instance job
 */
typedef struct InstanceCost { uint64_t work,allocated;size_t sites; } InstanceCost;
static InstanceCost instance_factory_cost(const char *kind,unsigned ordinal,bool query) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *closed=instance_source(&context,kind);
    XirCtfeInstanceRequest request=instance_request(closed,instance_named(closed,kind,ordinal));
    XirCtfeInstanceResult *owner=NULL;
    if (query) CHECK(xr_xir_compile_ctfe_instance(&request,&owner)==XR_XIR_CTFE_OK);
    XrCompileResourceStats before=stats(&context);size_t first=source_fixture_compile_attempts;
    XirCtfeInstanceView view={0};XrXirCtfeStatus status=query?xr_xir_compile_ctfe_instance_view(owner,&view):
        xr_xir_compile_ctfe_instance(&request,&owner);
    CHECK(status==XR_XIR_CTFE_OK);XrCompileResourceStats after=stats(&context);
    InstanceCost cost={after.work-before.work,after.allocated_bytes-before.allocated_bytes,source_fixture_compile_attempts-first};
    CHECK(cost.sites);xr_xir_compile_ctfe_instance_free(owner);
    xr_xir_compile_artifact_free(closed);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);return cost;
}
static void instance_faults(const char *kind,unsigned ordinal,bool prefix) {
    for (unsigned query=0;query<2;++query) {
        InstanceCost cost=instance_factory_cost(kind,ordinal,query!=0);size_t trials=prefix?(cost.sites==1?1:2):cost.sites;
        for (size_t trial=0;trial<trials;++trial) {
            size_t fault=prefix?(trial?cost.sites-1:0):trial;
            XrXirCompileContext context;context_new(&context);XrCompileResources *same=context.resources;
            XrXirArtifact *closed=instance_source(&context,kind);
            XirCtfeInstanceRequest request=instance_request(closed,instance_named(closed,kind,ordinal));
            XirCtfeInstanceResult *owner=NULL;
            if (query) CHECK(xr_xir_compile_ctfe_instance(&request,&owner)==XR_XIR_CTFE_OK);
            XrCompileResourceStats before=stats(&context);size_t first=source_fixture_compile_attempts;
            source_fixture_compile_injected=false;source_fixture_compile_fail_at=first+fault;
            XirCtfeInstanceView view={0};XrXirCtfeStatus status=query?xr_xir_compile_ctfe_instance_view(owner,&view):
                xr_xir_compile_ctfe_instance(&request,&owner);
            if (status!=XR_XIR_CTFE_OUT_OF_MEMORY) fprintf(stderr,"Instance FI %s[%u] query=%u point=%zu/%zu status=%u\n",kind,ordinal,query,fault,cost.sites,status);
            CHECK(status==XR_XIR_CTFE_OUT_OF_MEMORY && source_fixture_compile_injected);
            CHECK(source_fixture_compile_attempts==first+fault+1 && stats(&context).live_bytes==before.live_bytes);
            CHECK(query?(!view.checked && !view.lowered && !view.definition && !view.value):!owner);
            source_fixture_compile_fail_at=SIZE_MAX;XrCompileResourceStats failed=stats(&context);
            bool exhausted=cost.work>caps.work-failed.work || cost.allocated>caps.allocated_bytes-failed.allocated_bytes;
            status=query?xr_xir_compile_ctfe_instance_view(owner,&view):xr_xir_compile_ctfe_instance(&request,&owner);
            CHECK(status==(exhausted?XR_XIR_CTFE_BUDGET:XR_XIR_CTFE_OK));
            if (exhausted) {
                CHECK(query?(!view.checked && !view.lowered && !view.definition && !view.value):!owner);
                status=query?xr_xir_compile_ctfe_instance_view(owner,&view):xr_xir_compile_ctfe_instance(&request,&owner);
                CHECK(status==XR_XIR_CTFE_BUDGET);
            } else {
                CHECK(stats(&context).work-failed.work==cost.work && stats(&context).allocated_bytes-failed.allocated_bytes==cost.allocated);
                if (!query) CHECK(xr_xir_compile_ctfe_instance_view(owner,&view)==XR_XIR_CTFE_OK);
                CHECK(view.checked && view.definition && view.value && xr_xir_value_valid(view.value));
            }
            xr_xir_compile_ctfe_instance_free(owner);
            CHECK(context.resources==same);
            xr_xir_compile_artifact_free(closed);xr_compile_resources_release(context.resources);
            CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
            if (!(trial%256) || trial+1==trials) fprintf(stderr,"Instance FI %s[%u] query=%u actual=%zu/%zu same-owner/live physical0\n",kind,ordinal,query,fault+1,cost.sites);
        }
        fprintf(stderr,"Instance %s[%u] query=%u %s actualFI=%zu selected=%zu\n",kind,ordinal,query,prefix?"FIRST_LAST_ONLY":"COMPLETE_ALL_ORDINALS",cost.sites,trials);
    }
}
static XrXirCtfeStatus instance_complete_job(XrXirCompileContext *context,const char *kind,unsigned ordinal) {
    XrXirArtifact *closed=NULL;XrXirStatus prepared=instance_source_operation(context,&closed,kind);
    if (prepared!=XR_XIR_OK) return prepared==XR_XIR_BUDGET?XR_XIR_CTFE_BUDGET:
        prepared==XR_XIR_OUT_OF_MEMORY?XR_XIR_CTFE_OUT_OF_MEMORY:XR_XIR_CTFE_BAD_ARTIFACT;
    XirCtfeInstanceRequest request=instance_request(closed,instance_named(closed,kind,ordinal));XirCtfeInstanceResult *owner=NULL;
    XrXirCtfeStatus status=xr_xir_compile_ctfe_instance(&request,&owner);
    XirCtfeInstanceView view={0};
    if (status==XR_XIR_CTFE_OK) status=xr_xir_compile_ctfe_instance_view(owner,&view);
    xr_xir_compile_ctfe_instance_free(owner);xr_xir_compile_artifact_free(closed);return status;
}
static void instance_bounds(const char *kind,unsigned ordinal) {
    XrXirCompileContext census;context_new(&census);XrCompileResourceStats before=stats(&census);
    CHECK(instance_complete_job(&census,kind,ordinal)==XR_XIR_CTFE_OK);XrCompileResourceStats used=stats(&census);
    CHECK(used.live_bytes==before.live_bytes);xr_compile_resources_release(census.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=caps;
        if (!axis) limits.work=used.work-minus;
        else if (axis==1) limits.allocated_bytes=used.allocated_bytes-minus;
        else limits.live_bytes=used.peak_bytes-minus;
        XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits();XrCompileResourceStats initial=stats(&context);XrCompileResources *same=context.resources;
        XrXirCtfeStatus status=instance_complete_job(&context,kind,ordinal);
        if (status!=(minus?XR_XIR_CTFE_BUDGET:XR_XIR_CTFE_OK)) fprintf(stderr,"Instance bound %s[%u] axis=%u minus=%u status=%u\n",kind,ordinal,axis,minus,status);
        CHECK(status==(minus?XR_XIR_CTFE_BUDGET:XR_XIR_CTFE_OK) && stats(&context).live_bytes==initial.live_bytes);
        if (minus) CHECK(instance_complete_job(&context,kind,ordinal)==XR_XIR_CTFE_BUDGET && stats(&context).live_bytes==initial.live_bytes);
        CHECK(context.resources==same);xr_compile_resources_release(context.resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    fprintf(stderr,"Instance bounds %s[%u] threeaxes exact/minus1 original-owner physical0 work=%llu allocated=%llu peak=%llu\n",
        kind,ordinal,(unsigned long long)used.work,(unsigned long long)used.allocated_bytes,(unsigned long long)used.peak_bytes);
}
static void instance_tamper(void) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *closed=instance_source(&context,"scalar");
    XirCtfeInstanceRequest request=instance_request(closed,instance_named(closed,"scalar",0));XirCtfeInstanceResult *owner=NULL;
    CHECK(xr_xir_compile_ctfe_instance(&request,&owner)==XR_XIR_CTFE_OK);
    XirCtfeInstanceView view={0};CHECK(xr_xir_compile_ctfe_instance_view(owner,&view)==XR_XIR_CTFE_OK);
    XrXirType *arguments=(XrXirType *)view.checked->module.provenance->origins[request.function].arguments;
    XrXirType original=*arguments;*arguments=original==XR_XIR_I8?XR_XIR_STRING:XR_XIR_I8;
    XirCtfeInstanceView refused={0};XrCompileResourceStats before=stats(&context);
    CHECK(xr_xir_compile_ctfe_instance_view(owner,&refused)==XR_XIR_CTFE_BAD_ARTIFACT && !refused.checked && !refused.value);
    CHECK(stats(&context).live_bytes==before.live_bytes);*arguments=original;
    CHECK(xr_xir_compile_ctfe_instance_view(owner,&refused)==XR_XIR_CTFE_OK);
    XrXirFunction *from=&((XrXirFunction *)view.definition->module.functions)[view.definition_function];
    XrXirFunction *to=&((XrXirFunction *)view.checked->module.functions)[request.function];
    XrXirInstruction *a=NULL,*b=NULL;
    for (uint32_t i=0;i<from->instruction_count;++i) if (from->instructions[i].op==XR_XIR_CONST_INT) {
        CHECK(!a);a=&((XrXirInstruction *)from->instructions)[i];b=&((XrXirInstruction *)to->instructions)[i];
    }
    CHECK(a && b && a->immediate==42 && b->immediate==42);++a->immediate;++b->immediate;
    refused=(XirCtfeInstanceView){0};
    CHECK(xr_xir_compile_ctfe_instance_view(owner,&refused)==XR_XIR_CTFE_BAD_ARTIFACT && !refused.checked && !refused.value);
    CHECK(stats(&context).live_bytes==before.live_bytes);--a->immediate;--b->immediate;
    CHECK(xr_xir_compile_ctfe_instance_view(owner,&refused)==XR_XIR_CTFE_OK && refused.value->payload==42);
    XirCtfeInstanceView occupied={.instance_function=1},unchanged=occupied;
    CHECK(xr_xir_compile_ctfe_instance_view(owner,&occupied)==XR_XIR_CTFE_BAD_ARGUMENT && !memcmp(&occupied,&unchanged,sizeof(occupied)));
    xr_xir_compile_ctfe_instance_free(owner);xr_xir_compile_artifact_free(closed);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
