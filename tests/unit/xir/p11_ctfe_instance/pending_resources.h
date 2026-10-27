/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * pending_resources.h - Complete Source-to-observation failures retain one ledger
 */
static XrXirStatus pending_job(XrXirCompileContext *context,const char *kind) {
    XrXirArtifact *source=NULL;XirComptimePending *owner=NULL;XirComptimePendingView view={0};
    XrXirStatus status=pending_source(context,kind,0,&source);
    if (status==XR_XIR_OK) status=xr_xir_compile_comptime_pending(source,&owner,NULL);
    xr_xir_compile_artifact_free(source);
    if (status==XR_XIR_OK) { CHECK(owner);status=xr_xir_compile_comptime_pending_view(owner,&view,NULL); }
    if (status==XR_XIR_OK) CHECK(view.definition && view.arguments && view.argument_count);
    else CHECK(pending_empty(&view));
    xr_xir_compile_comptime_pending_free(owner);return status;
}
static void pending_resources(const char *kind,bool complete) {
    XrXirCompileContext census;context_new(&census);XrCompileResourceStats initial=stats(&census);
    size_t first=source_fixture_compile_attempts;CHECK(pending_job(&census,kind)==XR_XIR_OK);
    XrCompileResourceStats cost=stats(&census);size_t sites=source_fixture_compile_attempts-first;
    CHECK(sites && cost.held_bytes==initial.held_bytes);xr_compile_resources_release(census.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    for (size_t trial=0;trial<(complete?sites:2);++trial) {
        size_t fault=complete?trial:trial?sites-1:0;XrXirCompileContext context;context_new(&context);
        XrCompileResources *same=context.resources;XrCompileResourceStats before=stats(&context);first=source_fixture_compile_attempts;
        source_fixture_compile_injected=false;source_fixture_compile_fail_at=first+fault;
        XrXirStatus status=pending_job(&context,kind);
        if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"Pending FI %s actual%zu/%zu status%u\n",kind,fault+1,sites,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected);
        CHECK(source_fixture_compile_attempts==first+fault+1 && stats(&context).held_bytes==before.held_bytes);
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(pending_job(&context,kind)==XR_XIR_OK);

        CHECK(context.resources==same && stats(&context).held_bytes==before.held_bytes);
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
        if (!(trial%256) || trial+1==(complete?sites:2)) fprintf(stderr,"Pending FI %s actual%zu/%zu same-owner/live physical0\n",kind,fault+1,sites);
    }
    for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=caps;
        limits.memory_limit=cost.peak_bytes-minus;
        XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits();XrCompileResourceStats before=stats(&context);XrCompileResources *same=context.resources;
        CHECK(pending_job(&context,kind)==(minus?XR_XIR_MEMORY_LIMIT:XR_XIR_OK));
        if (minus) CHECK(pending_job(&context,kind)==XR_XIR_MEMORY_LIMIT);
        CHECK(context.resources==same && stats(&context).held_bytes==before.held_bytes);
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    fprintf(stderr,"Pending %s %s sites=%zu held exact/minus1 allocated=%llu peak=%llu\n",kind,
        complete?"COMPLETE_ALL_ORDINALS":"FIRST_LAST_ONLY",sites,
        (unsigned long long)cost.allocated_bytes,(unsigned long long)cost.peak_bytes);
}
