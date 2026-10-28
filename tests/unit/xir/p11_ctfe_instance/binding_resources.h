/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * binding_resources.h - Canonical receiving failure prefixes and correspondence
 *
 * KEY CONCEPT:
 *   Failure prefixes, retries and final cleanup retain the original finite owner.
 */
static XrXirStatus binding_job(XrXirCompileContext *context) {
    XrXirArtifact *source=NULL,*received=NULL,*closed=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};
    XrXirStatus status=binding_source(context,&source,0);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(source,&packet,NULL);
    xr_xir_compile_artifact_free(source);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL);
    xr_xir_compile_checked_packet_free(&packet);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(received,&closed,NULL);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(received);
    return status;
}
static void binding_resources(bool complete) {
    XrXirCompileContext census;context_new(&census);XrCompileResourceStats initial=stats(&census);
    size_t first=source_fixture_compile_attempts;CHECK(binding_job(&census)==XR_XIR_OK);
    XrCompileResourceStats cost=stats(&census);size_t sites=source_fixture_compile_attempts-first;
    CHECK(sites && cost.held_bytes==initial.held_bytes);xr_compile_resources_release(census.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    for (size_t trial=0;trial<(complete?sites:2);++trial) {
        size_t fault=complete?trial:trial?sites-1:0;XrXirCompileContext context;context_new(&context);
        XrCompileResources *same=context.resources;XrCompileResourceStats before=stats(&context);first=source_fixture_compile_attempts;
        source_fixture_compile_injected=false;source_fixture_compile_fail_at=first+fault;
        CHECK(binding_job(&context)==XR_XIR_OUT_OF_MEMORY);
        CHECK(source_fixture_compile_injected && source_fixture_compile_attempts==first+fault+1 && stats(&context).held_bytes==before.held_bytes);
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(binding_job(&context)==XR_XIR_OK);

        CHECK(context.resources==same && stats(&context).held_bytes==before.held_bytes);
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
        if (!(trial%256) || trial+1==(complete?sites:2)) fprintf(stderr,"Canonical binding FI actual=%zu/%zu same-owner/live physical0\n",fault+1,sites);
    }
    for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=caps;
        limits.memory_limit=cost.peak_bytes-minus;
        XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits();XrCompileResourceStats before=stats(&context);XrCompileResources *same=context.resources;
        CHECK(binding_job(&context)==(minus?XR_XIR_MEMORY_LIMIT:XR_XIR_OK));
        if (minus) CHECK(binding_job(&context)==XR_XIR_MEMORY_LIMIT);
        CHECK(context.resources==same && stats(&context).held_bytes==before.held_bytes);
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    fprintf(stderr,"Canonical binding %s sites=%zu held exact/minus1 allocated=%llu peak=%llu\n",
        complete?"COMPLETE_ALL_ORDINALS":"FIRST_LAST_ONLY",sites,
        (unsigned long long)cost.allocated_bytes,(unsigned long long)cost.peak_bytes);
}
static void binding_tamper(void) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *checked=NULL,*closed=NULL;
    CHECK(binding_source(&context,&checked,0)==XR_XIR_OK && xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    XrXirComptimeTable *table=&checked->construction->comptime;
    XrXirComptimeSite *site=(XrXirComptimeSite *)table->sites;uint32_t caller=site->caller;site->caller=UINT32_MAX;
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)!=XR_XIR_OK);site->caller=caller;
    XrXirFunction *function=(XrXirFunction *)&checked->module.functions[caller];
    const XrXirInstruction *body=function->instructions;function->instructions=NULL;
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_BAD_STRUCTURE);function->instructions=body;
    XrXirInstruction *call=(XrXirInstruction *)&body[site->instruction];uint32_t offset=call->type_arguments[0];
    call->type_arguments[0]=UINT32_MAX;
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_BAD_STRUCTURE);call->type_arguments[0]=offset;
    XrXirInstruction *ops=(XrXirInstruction *)checked->module.functions[table->bindings[0].provider].instructions;
    XrXirOp saved=ops[0].op;ops[0].op=XR_XIR_ADD_INT;
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)!=XR_XIR_OK);ops[0].op=saved;
    XrXirArtifact *source=(XrXirArtifact *)closed->module.provenance->source;uint32_t count=source->construction->comptime.binding_count;
    source->construction->comptime.binding_count=0;
    CHECK(xr_xir_compile_artifact_verify(closed,NULL)!=XR_XIR_OK);source->construction->comptime.binding_count=count;
    CHECK(xr_xir_compile_artifact_verify(closed,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(checked);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
