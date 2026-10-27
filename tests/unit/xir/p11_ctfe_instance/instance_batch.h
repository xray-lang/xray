/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * instance_batch.h - Independent roots and current immutable input admission
 */
#include "xir/xxir_ctfe_batch_internal.h"
static XirCtfeInstanceBatchRequest instance_batch_request(const XrXirArtifact *closed,const uint32_t *functions,uint32_t count) {
    XirCtfeInstanceRequest single=instance_request(closed,0);
    return (XirCtfeInstanceBatchRequest){closed,functions,count,single.target,single.limits,single.call_depth};
}
static void instance_batch_normal(const char *kind) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *closed=instance_source(&context,kind);
    uint32_t functions[2]={instance_named(closed,kind,0),instance_named(closed,kind,1)};
    XirCtfeInstanceBatchRequest request=instance_batch_request(closed,functions,2);XirCtfeInstanceBatch *owner=NULL;
    CHECK(xr_xir_compile_ctfe_instance_batch(&request,NULL,&owner)==XR_XIR_CTFE_OK);
    XirCtfeInstanceView views[2]={0};CHECK(xr_xir_compile_ctfe_instance_batch_views(owner,views,2)==XR_XIR_CTFE_OK);
    CHECK(views[0].checked==views[1].checked && views[0].lowered==views[1].lowered && views[0].checked!=closed);
    CHECK(views[0].definition==views[1].definition && views[0].definition_function==views[1].definition_function);
    CHECK(views[0].checked->module.provenance->origins[functions[0]].arguments[0]!=
        views[1].checked->module.provenance->origins[functions[1]].arguments[0]);
    if (!strcmp(kind,"foreign")) instance_foreign_edges(views[0].checked);
    if (!strcmp(kind,"scalar") || !strcmp(kind,"foreign")) CHECK(views[0].value->payload==42 && views[1].value->payload==42);
    if (!strcmp(kind,"array") || !strcmp(kind,"tuple") || !strcmp(kind,"optional") || !strcmp(kind,"empty") ||
        !strcmp(kind,"boxed") || !strcmp(kind,"nested")) CHECK(views[0].value->type!=views[1].value->type);
    if (!strcmp(kind,"text")) CHECK(views[0].value->payload!=views[1].value->payload);
    XrXirValue values[2]={0};for (unsigned i=0;i<2;++i) CHECK(xr_xir_value_copy(views[i].value,&values[i])==XR_XIR_VALUE_OK);
    xr_xir_compile_artifact_free(closed);xr_compile_resources_release(context.resources);
    memset(views,0,sizeof(views));CHECK(xr_xir_compile_ctfe_instance_batch_views(owner,views,2)==XR_XIR_CTFE_OK);
    xr_xir_compile_ctfe_instance_batch_free(owner);
    for (unsigned i=0;i<2;++i) CHECK(xr_xir_value_valid(&values[i]));
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;
    xr_xir_value_drop(&values[0]);xr_xir_value_drop(&values[1]);CHECK(source_fixture_compile_attempts==attempts);
    source_fixture_compile_fail_at=SIZE_MAX;CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"instance batch %s two real substitutions shared received graph independent values last-owner physical0\n",kind);
}
static bool instance_batch_empty(const XrXirValue *values,uint32_t count) {
    for (uint32_t i=0;i<count;++i) if (values[i].type || values[i].reserved || values[i].payload) return false;
    return true;
}
static void instance_batch_limits(void) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *closed=instance_source(&context,"text"),*lowered=NULL;
    uint32_t functions[2]={instance_named(closed,"text",0),instance_named(closed,"text",0)};
    XirCtfeInstanceBatchRequest request=instance_batch_request(closed,functions,2);XirCtfeInstanceBatch *owner=NULL;
    CHECK(xr_xir_compile_ctfe_instance_batch(NULL,NULL,&owner)==XR_XIR_CTFE_BAD_ARGUMENT && !owner);
    XirCtfeInstanceBatchRequest malformed=request;malformed.count=0;
    CHECK(xr_xir_compile_ctfe_instance_batch(&malformed,NULL,&owner)==XR_XIR_CTFE_BAD_ARGUMENT && !owner);
    malformed=request;malformed.count=context.limits.functions+1;
    CHECK(xr_xir_compile_ctfe_instance_batch(&malformed,NULL,&owner)==XR_XIR_CTFE_BAD_ARGUMENT && !owner);
    uint32_t nongeneric=named(&closed->module,"run");malformed=request;malformed.functions=&nongeneric;malformed.count=1;
    CHECK(xr_xir_compile_ctfe_instance_batch(&malformed,NULL,&owner)==XR_XIR_CTFE_NOT_SUPPORTED && !owner);
    CHECK(xr_xir_compile_ctfe_instance_batch(&request,NULL,&owner)==XR_XIR_CTFE_OK);
    XirCtfeInstanceView views[2]={0};
    CHECK(xr_xir_compile_ctfe_instance_batch_views(owner,views,1)==XR_XIR_CTFE_BAD_ARGUMENT && !views[0].checked);
    views[1].instance_function=1;
    CHECK(xr_xir_compile_ctfe_instance_batch_views(owner,views,2)==XR_XIR_CTFE_BAD_ARGUMENT && !views[0].checked && views[1].instance_function==1);
    memset(views,0,sizeof(views));CHECK(xr_xir_compile_ctfe_instance_batch_views(owner,views,2)==XR_XIR_CTFE_OK);
    CHECK(views[0].value->payload!=views[1].value->payload);
    CHECK(xr_xir_compile_lower(closed,&request.target,&lowered,NULL)==XR_XIR_OK);
    XirCtfeBatchRequest call={lowered,functions,2,request.target,{0},request.limits,request.call_depth};
    memcpy(call.checked_identity,lowered->checked_identity,32);XrXirValue values[2]={0};
    CHECK(xr_xir_compile_ctfe_batch(&call,NULL,values)==XR_XIR_CTFE_OK);
    CHECK(values[0].payload!=values[1].payload);xr_xir_value_drop(&values[0]);xr_xir_value_drop(&values[1]);
    XrCompileResourceStats before=stats(&context);
    functions[1]=instance_named(closed,"identity",0);
    CHECK(xr_xir_compile_ctfe_batch(&call,NULL,values)==XR_XIR_CTFE_BAD_ARGUMENT && instance_batch_empty(values,2));
    CHECK(stats(&context).held_bytes==before.held_bytes);
    functions[1]=closed->module.function_count;
    CHECK(xr_xir_compile_ctfe_batch(&call,NULL,values)==XR_XIR_CTFE_BAD_ARTIFACT && instance_batch_empty(values,2));
    CHECK(stats(&context).held_bytes==before.held_bytes);functions[1]=functions[0];
    values[1]=(XrXirValue){XR_XIR_I64,0,17};
    CHECK(xr_xir_compile_ctfe_batch(&call,NULL,values)==XR_XIR_CTFE_BAD_ARGUMENT && !values[0].type && values[1].payload==17);
    xr_xir_value_drop(&values[1]);
    for (unsigned axis=0;axis<6;++axis) {
        XirCtfeBatchRequest bad=call;
        if (!axis) bad.limits.steps=0;else if (axis==1) bad.limits.frame_bytes=0;
        else if (axis==2) bad.limits.output_bytes=0;else if (axis==3) bad.call_depth=0;
        else if (axis==4) ++bad.target.abi_version;else bad.checked_identity[0]^=1;
        XrXirCtfeStatus expected=!axis?XR_XIR_CTFE_STEP_LIMIT:axis==1?XR_XIR_CTFE_FRAME_LIMIT:
            axis==2?XR_XIR_CTFE_OUTPUT_LIMIT:axis==3?XR_XIR_CTFE_DEPTH_LIMIT:axis==4?XR_XIR_CTFE_PROFILE:XR_XIR_CTFE_IDENTITY;
        CHECK(xr_xir_compile_ctfe_batch(&bad,NULL,values)==expected && instance_batch_empty(values,2));
        CHECK(stats(&context).held_bytes==before.held_bytes);
    }
    CHECK(xr_xir_compile_ctfe_batch(&call,NULL,values)==XR_XIR_CTFE_OK);
    xr_xir_value_drop(&values[0]);xr_xir_value_drop(&values[1]);
    XrXirInstruction *instruction=&((XrXirInstruction *)lowered->module.functions[functions[0]].instructions)[0];
    XrXirInstruction saved=*instruction;instruction->op=XR_XIR_OP_COUNT;
    CHECK(xr_xir_compile_ctfe_batch(&call,NULL,values)==XR_XIR_CTFE_BAD_ARTIFACT && instance_batch_empty(values,2));
    *instruction=saved;CHECK(xr_xir_compile_ctfe_batch(&call,NULL,values)==XR_XIR_CTFE_OK);
    xr_xir_value_drop(&values[0]);xr_xir_value_drop(&values[1]);
    XrXirType *argument=(XrXirType *)views[0].checked->module.provenance->origins[functions[0]].arguments;
    XrXirType original=*argument;*argument=original==XR_XIR_I8?XR_XIR_STRING:XR_XIR_I8;
    memset(views,0,sizeof(views));
    CHECK(xr_xir_compile_ctfe_instance_batch_views(owner,views,2)==XR_XIR_CTFE_BAD_ARTIFACT && !views[0].checked && !views[1].checked);
    *argument=original;CHECK(xr_xir_compile_ctfe_instance_batch_views(owner,views,2)==XR_XIR_CTFE_OK);
    size_t attempts=source_fixture_compile_attempts;xr_xir_compile_ctfe_instance_batch_free(owner);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(closed);xr_compile_resources_release(context.resources);
    CHECK(source_fixture_compile_attempts==attempts && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"instance batch limits duplicate roots atomic failure fresh current admission original memory owner physical0\n");
}
