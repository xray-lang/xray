/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * evaluation_cases.h - Actual missing helpers execute with compiler-only purpose
 */
static XrXirStatus evaluation_source(XrXirCompileContext *context,const char *kind,XrXirArtifact **output) {
    XrXirArtifact *source=NULL;XirComptimePending *pending=NULL;
    XrXirStatus status=pending_source(context,kind,0,&source);
    if (status==XR_XIR_OK) status=xr_xir_compile_comptime_pending(source,&pending,NULL);
    xr_xir_compile_artifact_free(source);
    if (status==XR_XIR_OK) {
        CHECK(pending);status=xr_xir_compile_comptime_pending_specialize(pending,output,NULL);
    }
    xr_xir_compile_comptime_pending_free(pending);return status;
}
static void evaluation_normal(const char *kind) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *checked=NULL,*received=NULL;
    XrXirStatus status=evaluation_source(&context,kind,&checked);
    if (status!=XR_XIR_OK) fprintf(stderr,"Evaluation %s Source status%u\n",kind,status);
    CHECK(status==XR_XIR_OK && checked && checked->evaluation_only==1);
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
    const XrXirProvenance *proof=checked->module.provenance;CHECK(proof->kind==XR_XIR_EVIDENCE_EVALUATION);
    uint32_t root=proof->evaluation.callee;
    CHECK(proof->source->module.provenance->kind==XR_XIR_EVIDENCE_TEMPLATE);
    CHECK(checked->module.declarations->module_count==proof->source->module.declarations->module_count);
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_BAD_STAGE && !packet.bytes);
    CHECK(xir_evaluation_write(checked,&packet,NULL)==XR_XIR_OK && packet.length>76 && !memcmp(packet.bytes,"XRCEV\0\0\0",8));
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_BAD_STRUCTURE && !received);
    CHECK(xir_evaluation_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK && received->evaluation_only==1);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(checked);checked=NULL;
    XirCtfeInstanceBatchRequest request=instance_batch_request(received,&root,1);XirCtfeInstanceBatch *owner=NULL;
    XrXirCtfeStatus evaluated=xr_xir_compile_ctfe_instance_batch(&request,&owner);
    if (evaluated!=XR_XIR_CTFE_OK) fprintf(stderr,"Evaluation %s CTFE status%u\n",kind,evaluated);
    CHECK(evaluated==XR_XIR_CTFE_OK && owner);xr_xir_compile_artifact_free(received);
    XirCtfeInstanceView view={0};CHECK(xr_xir_compile_ctfe_instance_batch_views(owner,&view,1)==XR_XIR_CTFE_OK);
    CHECK(view.checked->evaluation_only && view.lowered->evaluation_only && view.instance_function==root);
    CHECK(view.definition==view.checked->module.provenance->source && view.definition_function==view.checked->module.provenance->origins[root].function);
    CHECK(xr_xir_value_valid(view.value));
    if (!strcmp(kind,"scalar") || !strcmp(kind,"builtin") || !strcmp(kind,"call") || !strcmp(kind,"nested-unbound"))
        CHECK(view.value->type==XR_XIR_I64 && view.value->payload==42);
    if (!strcmp(kind,"unit")) CHECK(!view.value->type && !view.value->reserved && !view.value->payload);
    XirLiteralDefinitions literals={0};XirLiteralRequest literal={&context,owner,1};
    CHECK(xr_xir_compile_instance_literals(&literal,&literals,NULL)==XR_XIR_OK && literals.count==1);
    CHECK(xr_xir_compile_artifact_verify(literals.definition,NULL)==XR_XIR_OK);
    CHECK(literals.definition->module.provenance->kind==XR_XIR_EVIDENCE_TEMPLATE && !literals.definition->evaluation_only);
    CHECK(literals.bindings[0].definition==view.definition_function);
    CHECK(xr_xir_compile_checked_write(literals.definition,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_instance_literals_free(&literals);
    xr_compile_resources_release(context.resources);view=(XirCtfeInstanceView){0};
    CHECK(xr_xir_compile_ctfe_instance_batch_views(owner,&view,1)==XR_XIR_CTFE_OK);
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;
    xr_xir_compile_ctfe_instance_batch_free(owner);CHECK(source_fixture_compile_attempts==attempts);
    source_fixture_compile_fail_at=SIZE_MAX;CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Evaluation %s actual missing root, full original Source/initializers/TEMPLATE, sole CTFE and ordinary literal receiver producer-dead physical0\n",kind);
}
