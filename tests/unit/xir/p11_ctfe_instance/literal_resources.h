/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * literal_resources.h - Whole receiving jobs retain their original finite owner
 */

static XrXirStatus literal_job(XrXirCompileContext *context,const char *kind) {
    XrXirArtifact *closed=NULL,*received=NULL,*specialized=NULL,*lowered=NULL;
    XrXirStatus status=instance_source_operation(context,&closed,kind);XirCtfeInstanceBatch *batch=NULL;
    if (status==XR_XIR_OK) {
        uint32_t functions[2]={instance_named(closed,kind,0),instance_named(closed,kind,1)};
        XirCtfeInstanceBatchRequest request=instance_batch_request(closed,functions,2);
        status=p11_xir_from_ctfe(xr_xir_compile_ctfe_instance_batch(&request,NULL,&batch));
    }
    xr_xir_compile_artifact_free(closed);XirLiteralDefinitions result={0};uint32_t provider=UINT32_MAX;
    if (status==XR_XIR_OK) {
        XirLiteralRequest request={context,batch,2,NULL};status=xr_xir_compile_instance_literals(&request,&result,NULL);
        if (status==XR_XIR_OK) provider=result.bindings[0].provider;else CHECK(literal_empty(&result));
    }
    xr_xir_compile_ctfe_instance_batch_free(batch);XrXirCheckedPacket packet={0};
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(result.definition,&packet,NULL);
    xr_xir_compile_instance_literals_free(&result);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL);
    xr_xir_compile_checked_packet_free(&packet);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(received,&specialized,NULL, NULL);
    xr_xir_compile_artifact_free(received);uint32_t function=UINT32_MAX;
    if (status==XR_XIR_OK) {
        for (uint32_t f=0;f<specialized->module.function_count;++f)
            if (specialized->module.provenance->origins[f].function==provider) function=f;
        CHECK(function!=UINT32_MAX);
        const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
        status=xr_xir_compile_lower(specialized,&target, NULL,&lowered,NULL);
    }
    xr_xir_compile_artifact_free(specialized);
    if (status==XR_XIR_OK) {
        XrXirCtfeCallRequest call=import_request(lowered,"run",NULL);call.evaluation.function=function;XrXirValue value={0};
        status=p11_xir_from_ctfe(xr_xir_compile_ctfe_calls(&call,&value));
        if (status==XR_XIR_OK) literal_value(kind,&value);else CHECK(!value.type && !value.reserved && !value.payload);
        xr_xir_value_drop(&value);
    }
    xr_xir_compile_artifact_free(lowered);return status;
}
static void literal_resources(const char *kind,bool complete) {
    XrXirCompileContext census;context_new(&census);XrCompileResourceStats initial=stats(&census);
    size_t first=source_fixture_compile_attempts;CHECK(literal_job(&census,kind)==XR_XIR_OK);
    XrCompileResourceStats cost=stats(&census);size_t sites=source_fixture_compile_attempts-first;
    CHECK(sites && cost.held_bytes==initial.held_bytes);xr_compile_resources_release(census.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    for (size_t trial=0;trial<(complete?sites:2);++trial) {
        size_t fault=complete?trial:trial?sites-1:0;XrXirCompileContext context;context_new(&context);
        XrCompileResources *same=context.resources;XrCompileResourceStats before=stats(&context);first=source_fixture_compile_attempts;
        source_fixture_compile_injected=false;source_fixture_compile_fail_at=first+fault;
        XrXirStatus status=literal_job(&context,kind);
        if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"Typed literal FI %s actual=%zu/%zu status=%u\n",kind,fault+1,sites,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected);
        CHECK(source_fixture_compile_attempts==first+fault+1 && stats(&context).held_bytes==before.held_bytes);
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(literal_job(&context,kind)==XR_XIR_OK);

        CHECK(context.resources==same && stats(&context).held_bytes==before.held_bytes);
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
        if (!(trial%256) || trial+1==(complete?sites:2))
            fprintf(stderr,"Typed literal FI %s actual=%zu/%zu same-owner/live physical0\n",kind,fault+1,sites);
    }
    for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=caps;
        limits.memory_limit=cost.peak_bytes-minus;
        XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits();XrCompileResourceStats before=stats(&context);XrCompileResources *same=context.resources;
        CHECK(literal_job(&context,kind)==(minus?XR_XIR_MEMORY_LIMIT:XR_XIR_OK));
        if (minus) CHECK(literal_job(&context,kind)==XR_XIR_MEMORY_LIMIT);
        CHECK(context.resources==same && stats(&context).held_bytes==before.held_bytes);
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    fprintf(stderr,"Typed literal %s %s sites=%zu held exact/minus1 allocated=%llu peak=%llu\n",kind,
        complete?"COMPLETE_ALL_ORDINALS":"FIRST_LAST_ONLY",sites,
        (unsigned long long)cost.allocated_bytes,(unsigned long long)cost.peak_bytes);
}
