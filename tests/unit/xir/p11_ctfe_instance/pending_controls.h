/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * pending_controls.h - Every observation repeats current full Source verification
 */
static void pending_rejected(XirComptimePending *owner,XrXirStatus expected) {
    XirComptimePendingView output={0};XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_comptime_pending_view(owner,&output,&diagnostic);
    CHECK(status==expected && diagnostic.status==status && pending_empty(&output));
}
static void pending_controls(void) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *source=NULL,*closed=NULL;
    XirComptimePending *owner=NULL;XirComptimePendingView view={0};
    CHECK(xr_xir_compile_comptime_pending(NULL,&owner,NULL)==XR_XIR_BAD_STRUCTURE && !owner);
    CHECK(xr_xir_compile_comptime_pending_view(NULL,&view,NULL)==XR_XIR_BAD_STRUCTURE && pending_empty(&view));
    CHECK(pending_source(&context,"array",1,&source)==XR_XIR_OK);
    CHECK(xr_xir_compile_comptime_pending(source,&owner,NULL)==XR_XIR_OK && !owner);
    CHECK(xr_xir_compile_specialize(source,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_comptime_pending(closed,&owner,NULL)==XR_XIR_BAD_STAGE && !owner);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(source);source=NULL;
    CHECK(pending_source(&context,"array",0,&source)==XR_XIR_OK);
    CHECK(xr_xir_compile_comptime_pending(source,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_xir_compile_comptime_pending(source,&owner,NULL)==XR_XIR_OK && owner);
    XirComptimePending *occupied=owner;CHECK(xr_xir_compile_comptime_pending(source,&occupied,NULL)==XR_XIR_BAD_STRUCTURE && occupied==owner);
    CHECK(xr_xir_compile_comptime_pending_view(owner,&view,NULL)==XR_XIR_OK);
    XirComptimePendingView occupied_view=view;
    CHECK(xr_xir_compile_comptime_pending_view(owner,&occupied_view,NULL)==XR_XIR_BAD_STRUCTURE && occupied_view.definition==view.definition);
    XrXirType *argument=(XrXirType *)view.arguments;XrXirType saved=*argument;
    *argument=saved==XR_XIR_I8?XR_XIR_STRING:XR_XIR_I8;pending_rejected(owner,XR_XIR_BAD_TYPE);*argument=saved;
    XrXirInstruction *op=(XrXirInstruction *)view.definition->module.functions[view.site.helper].instructions;
    XrXirOp saved_op=op->op;op->op=XR_XIR_OP_COUNT;
    XirComptimePendingView rejected={0};CHECK(xr_xir_compile_comptime_pending_view(owner,&rejected,NULL)!=XR_XIR_OK && pending_empty(&rejected));op->op=saved_op;
    uint32_t provider=view.definition->construction->comptime.bindings[0].provider;
    char *name=(char *)view.definition->module.functions[provider].name;char saved_name=*name;*name='Z';
    rejected=(XirComptimePendingView){0};CHECK(xr_xir_compile_comptime_pending_view(owner,&rejected,NULL)!=XR_XIR_OK && pending_empty(&rejected));*name=saved_name;
    CHECK(xr_xir_compile_comptime_pending_view(owner,&(XirComptimePendingView){0},NULL)==XR_XIR_OK);
    XrCompileResourceStats used=stats(&context);CHECK(xr_compile_resources_work(context.resources,caps.work-used.work)==XR_COMPILE_RESOURCE_OK);
    XirComptimePending *none=NULL;
    CHECK(xr_xir_compile_comptime_pending(source,&none,NULL)==XR_XIR_BUDGET && !none);
    CHECK(xr_xir_compile_comptime_pending(source,&none,NULL)==XR_XIR_BUDGET && !none);
    pending_rejected(owner,XR_XIR_BUDGET);pending_rejected(owner,XR_XIR_BUDGET);
    size_t attempts=source_fixture_compile_attempts;xr_xir_compile_comptime_pending_free(owner);
    xr_xir_compile_artifact_free(source);xr_compile_resources_release(context.resources);
    CHECK(source_fixture_compile_attempts==attempts && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Pending current Source/ordered argument tamper, occupied slots, INSTANCE and exhausted owner refuse atomically physical0\n");
}
