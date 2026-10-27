/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * program_controls.h - Current keys and exact batch failures remain fail-closed
 */
static void program_binding_controls(void) {
    XrXirCompileContext context;context_new(&context);XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(program_source(&context,program_text("array"),&source,&diagnostic)==XR_XIR_OK);
    XrXirComptimeTable *table=&source.checked->construction->comptime;CHECK(table->binding_count==2);
    XrXirArtifact *closed=NULL;table->binding_count=1;
    XrXirStatus partial_status=xr_xir_compile_specialize(source.checked,&closed,NULL);
    if(partial_status!=XR_XIR_BAD_STRUCTURE || closed)fprintf(stderr,"Binding truncation actual status=%u published=%u\n",partial_status,closed!=NULL);
    CHECK(partial_status==XR_XIR_BAD_STRUCTURE && !closed);table->binding_count=2;
    CHECK(xr_xir_compile_construction_comptime(xr_xir_compile_source_snapshot_construction(source.snapshot))->binding_count==2);
    XrXirInstruction *op=(XrXirInstruction *)source.checked->module.functions[table->bindings[0].provider].instructions;
    XrXirOp saved=op->op;op->op=XR_XIR_OP_COUNT;
    CHECK(xr_xir_compile_artifact_verify(source.checked,NULL)!=XR_XIR_OK);op->op=saved;
    CHECK(xr_xir_compile_specialize(source.checked,&closed,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_source_result_free(&source);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static void program_batch_controls(void) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *closed=instance_source(&context,"scalar");
    uint32_t functions[2]={instance_named(closed,"scalar",0),instance_named(closed,"identity",0)};
    XirCtfeInstanceBatchRequest request=instance_batch_request(closed,functions,2);XirCtfeInstanceBatch *owner=NULL;
    uint32_t failed=UINT32_MAX;
    CHECK(xr_xir_compile_ctfe_instance_batch_at(&request,NULL,&owner,&failed)==XR_XIR_CTFE_NOT_SUPPORTED && failed==1 && !owner);
    failed=0;CHECK(xr_xir_compile_ctfe_instance_batch_at(&request,NULL,&owner,&failed)==XR_XIR_CTFE_BAD_ARGUMENT && failed==0 && !owner);
    failed=UINT32_MAX;functions[1]=instance_named(closed,"scalar",1);
    CHECK(xr_xir_compile_ctfe_instance_batch_at(&request,NULL,&owner,&failed)==XR_XIR_CTFE_OK && failed==UINT32_MAX && owner);
    xr_xir_compile_ctfe_instance_batch_free(owner);owner=NULL;XrXirArtifact *lowered=NULL;
    CHECK(xr_xir_compile_lower(closed,&request.target,&lowered,NULL)==XR_XIR_OK);
    functions[1]=instance_named(closed,"identity",0);XrXirValue values[2]={0};
    XirCtfeBatchRequest call={lowered,functions,2,request.target,{0},request.limits,request.call_depth};
    memcpy(call.checked_identity,lowered->checked_identity,32);
    CHECK(xr_xir_compile_ctfe_batch_at(&call,NULL,values,&failed)==XR_XIR_CTFE_BAD_ARGUMENT && failed==1 && instance_batch_empty(values,2));
    failed=0;CHECK(xr_xir_compile_ctfe_batch_at(&call,NULL,values,&failed)==XR_XIR_CTFE_BAD_ARGUMENT && failed==0 && instance_batch_empty(values,2));
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(closed);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Public comptime controls missing current key rejects no runtime fallback; malformed provider rejects; actual second batch index atomic output no execution retry physical0\n");
}
