/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * literal_controls.h - Current artifact, owner and atomic receiving rejection
 */
static bool literal_empty(const XirLiteralDefinitions *result) {
    return !result->definition && !result->bindings && !result->count;
}
static void literal_controls(void) {
    XrXirCompileContext context;context_new(&context);XirCtfeInstanceBatch *batch=literal_batch(&context,"boxed");
    XirLiteralRequest request={&context,batch,2,NULL};XirLiteralDefinitions output={0};XrXirDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_instance_literals(NULL,&output,&diagnostic)==XR_XIR_BAD_STRUCTURE && literal_empty(&output));
    CHECK(diagnostic.status==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_xir_compile_instance_literals(&request,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    XirLiteralRequest bad=request;bad.count=0;
    CHECK(xr_xir_compile_instance_literals(&bad,&output,NULL)==XR_XIR_BAD_STRUCTURE && literal_empty(&output));
    bad.count=1;CHECK(xr_xir_compile_instance_literals(&bad,&output,NULL)==XR_XIR_BAD_STRUCTURE && literal_empty(&output));
    bad.count=context.limits.functions+1;
    CHECK(xr_xir_compile_instance_literals(&bad,&output,NULL)==XR_XIR_BAD_STRUCTURE && literal_empty(&output));
    output.count=1;CHECK(xr_xir_compile_instance_literals(&request,&output,NULL)==XR_XIR_BAD_STRUCTURE && output.count==1);
    output=(XirLiteralDefinitions){0};XrXirCompileContext foreign;context_new(&foreign);bad=request;bad.context=&foreign;
    XrCompileResourceStats before=stats(&foreign);
    CHECK(xr_xir_compile_instance_literals(&bad,&output,NULL)==XR_XIR_BAD_STRUCTURE && literal_empty(&output));
    CHECK(stats(&foreign).held_bytes==before.held_bytes);xr_compile_resources_release(foreign.resources);
    XirCtfeInstanceView views[2]={0};CHECK(xr_xir_compile_ctfe_instance_batch_views(batch,views,2)==XR_XIR_CTFE_OK);
    XrXirOrigin *origin=(XrXirOrigin *)&views[0].checked->module.provenance->origins[views[0].instance_function];
    XrXirType *argument=(XrXirType *)origin->arguments;XrXirType saved=*argument;*argument=XR_XIR_BOOL;
    before=stats(&context);
    CHECK(xr_xir_compile_instance_literals(&request,&output,NULL)==XR_XIR_BAD_STRUCTURE && literal_empty(&output));
    CHECK(stats(&context).held_bytes==before.held_bytes);*argument=saved;
    CHECK(xr_xir_compile_instance_literals(&request,&output,NULL)==XR_XIR_OK);xr_xir_compile_instance_literals_free(&output);
    bad=request;XrXirCompileContext small=context;small.limits.functions=views[0].definition->module.function_count;bad.context=&small;
    CHECK(xr_xir_compile_instance_literals(&bad,&output,NULL)==XR_XIR_BUDGET && literal_empty(&output));
    CHECK(xr_xir_compile_instance_literals(&request,&output,NULL)==XR_XIR_OK);xr_xir_compile_instance_literals_free(&output);
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;
    xr_xir_compile_instance_literals_free(&output);xr_xir_compile_ctfe_instance_batch_free(batch);
    xr_compile_resources_release(context.resources);CHECK(source_fixture_compile_attempts==attempts);
    source_fixture_compile_fail_at=SIZE_MAX;CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Typed literal controls occupied/count/foreign-owner/current-tamper/functions atomic output no-allocation destruction physical0\n");
}
