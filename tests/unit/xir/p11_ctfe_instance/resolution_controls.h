/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * resolution_controls.h - Existing keys never authorize reevaluation or fallback
 */
static bool resolution_failure_empty(const XirComptimeResolveFailure *failure) {
    return !failure->status && !failure->root_local && !failure->site.caller && !failure->site.instruction && !failure->site.helper;
}
static void resolution_controls(void) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *source=NULL,*out=NULL,*closed=NULL;
    XirComptimeResolveFailure failure={0};
    CHECK(xr_xir_compile_comptime_resolve(NULL,&out,&failure,NULL)==XR_XIR_BAD_STRUCTURE && !out && resolution_failure_empty(&failure));
    CHECK(resolution_input(&context,"known-trap",&source)==XR_XIR_OK);
    XirComptimeResolveRequest request=resolution_request(source);request.limits.steps=0;request.call_depth=0;
    CHECK(xr_xir_compile_comptime_resolve(&request,&out,&failure,NULL)==XR_XIR_OK && out && resolution_failure_empty(&failure));
    XrXirArtifact *occupied=out;
    CHECK(xr_xir_compile_comptime_resolve(&request,&occupied,&failure,NULL)==XR_XIR_BAD_STRUCTURE && occupied==out);
    CHECK(xr_xir_compile_specialize(out,&closed,NULL)==XR_XIR_OK);
    request=resolution_request(closed);XrXirArtifact *none=NULL;
    CHECK(xr_xir_compile_comptime_resolve(&request,&none,&failure,NULL)==XR_XIR_BAD_STAGE && !none);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(out);xr_xir_compile_artifact_free(source);
    source=NULL;CHECK(resolution_input(&context,"bad-trap",&source)==XR_XIR_OK);request=resolution_request(source);out=NULL;
    const XrXirComptimeSite expected=source->construction->comptime.sites[0];
    for (unsigned i=0;i<2;++i) {
        failure=(XirComptimeResolveFailure){0};XrXirDiagnostic diagnostic={0};XrCompileResourceStats before=stats(&context);
        CHECK(xr_xir_compile_comptime_resolve(&request,&out,&failure,&diagnostic)==XR_XIR_BAD_TYPE && !out);
        CHECK(failure.status==XR_XIR_CTFE_DIVIDE_BY_ZERO && failure.root_local && failure.site.caller==expected.caller &&
            failure.site.instruction==expected.instruction && failure.site.helper==expected.helper);
        CHECK(diagnostic.function==expected.caller && diagnostic.instruction==expected.instruction);
        CHECK(stats(&context).held_bytes==before.held_bytes && source->construction->comptime.binding_count==1);
    }
    failure=(XirComptimeResolveFailure){.status=XR_XIR_CTFE_PROFILE};
    CHECK(xr_xir_compile_comptime_resolve(&request,&out,&failure,NULL)==XR_XIR_BAD_STRUCTURE && failure.status==XR_XIR_CTFE_PROFILE && !out);
    failure=(XirComptimeResolveFailure){0};request.limits.steps=XR_XIR_CTFE_MAX_STEPS+1;
    CHECK(xr_xir_compile_comptime_resolve(&request,&out,&failure,NULL)==XR_XIR_BAD_STRUCTURE && !out);
    request=resolution_request(source);request.target.abi_version++;
    CHECK(xr_xir_compile_comptime_resolve(&request,&out,&failure,NULL)==XR_XIR_BAD_LAYOUT && !out);
    xr_xir_compile_artifact_free(source);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    for (unsigned axis=0;axis<4;++axis) {
        context_new(&context);source=NULL;CHECK(pending_source(&context,"scalar",0,&source)==XR_XIR_OK);request=resolution_request(source);
        if (!axis) request.limits.steps=0;else if (axis==1) request.limits.frame_bytes=0;else if (axis==2) request.limits.output_bytes=0;else request.call_depth=0;
        XrXirCtfeStatus wanted=!axis?XR_XIR_CTFE_STEP_LIMIT:axis==1?XR_XIR_CTFE_FRAME_LIMIT:axis==2?XR_XIR_CTFE_OUTPUT_LIMIT:XR_XIR_CTFE_DEPTH_LIMIT;
        failure=(XirComptimeResolveFailure){0};XrCompileResourceStats before=stats(&context);
        CHECK(xr_xir_compile_comptime_resolve(&request,&out,&failure,NULL)==XR_XIR_BUDGET && !out && failure.status==wanted && failure.root_local);
        CHECK(stats(&context).held_bytes==before.held_bytes);xr_xir_compile_artifact_free(source);xr_compile_resources_release(context.resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    context_new(&context);source=NULL;CHECK(pending_source(&context,"scalar",0,&source)==XR_XIR_OK);request=resolution_request(source);
    size_t attempts=source_fixture_compile_attempts;xr_xir_compile_artifact_free(source);xr_compile_resources_release(context.resources);
    CHECK(source_fixture_compile_attempts==attempts && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Resolution original providers skip trapping helpers, actual root failure coordinates, original CTFE caps/occupied outputs atomic physical0\n");
}
