/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * ctfe_from_utf8_resources.h - Real compiler-native UTF8 owner and failure cases
 */
static XrXirArtifact *slice_utf8_artifact(XrXirCompileContext *context) {
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=slice_source(context,"from_utf8",&source,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"UTF8 Source %u %d:%d %s\n",(unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK);return slice_lower(context,&source);
}
static void slice_pause_native(XrXirCtfeCallRequest *request,CtfeCallExecution *execution) {
    bool owned=false;XrXirCellProvenance *cells=NULL;void *closure=NULL;
    CHECK(ctfe_call_prepare_received(request,&owned,&cells,&closure)==XR_XIR_CTFE_OK && owned);
    *execution=(CtfeCallExecution){.request=request,.artifact=request->evaluation.artifact,.steps=request->evaluation.limits.steps};
    CHECK(ctfe_owned_new(execution->artifact,owned,&execution->owned)==XR_XIR_CTFE_OK);
    execution->owned.cells=cells;execution->owned.closure=closure;execution->owned.loan_bytes=&execution->loan_bytes;
    execution->owned.reachable=closure?(uint8_t *)((uint32_t *)closure+execution->artifact->module.function_count):NULL;
    CHECK(ctfe_call_frame_new(execution,request->evaluation.function,UINT32_MAX,&execution->top)==XR_XIR_CTFE_OK);
    execution->depth=1;execution->live_bytes=execution->top->bytes;
    for (unsigned step=0;step<1024;++step) {
        slice_activate(execution);
        if (execution->artifact->module.declarations->functions[execution->top->function].native_callable) return;
        XirScalarCoreAction action={0};CHECK(ctfe_call_step(execution,&action)==XR_XIR_CTFE_OK && action.kind==XIR_SCALAR_CONTINUE);
    }
    CHECK(false);
}
static void slice_native_oom(void) {
    XrXirCompileContext context={0};slice_context_new(&context);XrXirArtifact *artifact=slice_utf8_artifact(&context);
    XrXirCtfeCallRequest request=slice_request(artifact,"unicode");
    for (size_t point=0;point<2;++point) {
        CtfeCallExecution execution={0};slice_pause_native(&request,&execution);
        XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
        size_t attempts=source_fixture_compile_attempts;source_fixture_compile_injected=false;source_fixture_compile_fail_at=attempts+point;
        XirScalarCoreAction action={0};XrXirCtfeStatus status=ctfe_native_callable_step(&execution,&action);
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_CTFE_OUT_OF_MEMORY && source_fixture_compile_injected && source_fixture_compile_attempts==attempts+point+1);
        CHECK(!execution.top->returned.type && !execution.top->returned.payload);
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
        slice_execution_free(&execution);
    }
    xr_xir_compile_artifact_free(artifact);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);puts("Compiler native UTF8 physical String OOM points preserve resource status and owners");
}
static void slice_native_control(void) {
    XrXirCompileContext context={0};slice_context_new(&context);XrXirArtifact *artifact=slice_utf8_artifact(&context);
    XrXirCtfeCallRequest request=slice_request(artifact,"unicode");
    for (unsigned mode=0;mode<3;++mode) {
        CtfeCallExecution execution={0};slice_pause_native(&request,&execution);
        SliceArgumentCancel cancel={0,mode?3u:1u};XrCompileRequestControl control={0};
        XrCompileRequestOptions options=mode==2?(XrCompileRequestOptions){.deadline_mode=XR_COMPILE_DEADLINE_AFTER}:
            (XrCompileRequestOptions){.cancel_poll=slice_argument_cancel_poll,.cancel_context=&cancel};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);execution.owned.control=&control;
        XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
        size_t attempts=source_fixture_compile_attempts;XirScalarCoreAction action={0};
        CHECK(ctfe_native_callable_step(&execution,&action)==(mode==2?XR_XIR_CTFE_DEADLINE:XR_XIR_CTFE_CANCELLED));
        CHECK(!execution.top->returned.type && !execution.top->returned.payload);
        if (mode==1) CHECK(source_fixture_compile_attempts==attempts+2 && cancel.polls==3);
        else CHECK(source_fixture_compile_attempts==attempts);
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
        slice_execution_free(&execution);
    }
    xr_xir_compile_artifact_free(artifact);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);puts("Compiler native UTF8 cancellation and absolute deadline preserve empty output");
}
static void slice_native_lifetime(void) {
    XrXirCompileContext context={0};slice_context_new(&context);XrXirArtifact *artifact=slice_utf8_artifact(&context);
    XrXirCtfeCallRequest request=slice_request(artifact,"unicode");CtfeCallExecution execution={0};slice_pause_native(&request,&execution);
    CtfeCallFrame *native=execution.top;CtfeViewBinding *grant=native->views;CHECK(grant);native->views=NULL;
    XirScalarCoreAction action={0};CHECK(ctfe_native_callable_step(&execution,&action)==XR_XIR_CTFE_BAD_ARTIFACT && !native->returned.type);
    native->views=grant;CHECK(ctfe_native_callable_step(&execution,&action)==XR_XIR_CTFE_OK && action.kind==XIR_SCALAR_RETURN);
    XrXirValue answer={0},late={0},input=ctfe_nominal_operand(&native->run,0);
    CHECK(xr_xir_value_copy(&action.value,&answer)==XR_XIR_VALUE_OK && xr_xir_value_copy(&input,&late)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission=ctfe_value_admission(&execution.owned);
    CtfeViewAccessContext access_context={&execution,native,&admission,XR_XIR_CTFE_OK};XrXirViewAccess access={&admission,ctfe_view_authorize,&access_context};
    slice_execution_free(&execution);int64_t length=-1;
    CHECK(xr_xir_view_len(&late,&access,&length)==XR_XIR_VALUE_BAD_ARGUMENT && length==-1);xr_xir_value_drop(&late);
    xr_xir_compile_artifact_free(artifact);xr_compile_resources_release(context.resources);
    const char *bytes=NULL;size_t count=0;CHECK(xr_xir_string_view(&answer,&bytes,&count) && count==4 && !memcmp(bytes,"A\0\xc3\xa9",4));
    xr_xir_value_drop(&answer);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Compiler native owned String outlives producer; retained stale view has no authority");
}
static void slice_native_types(void) {
    XrXirCompileContext context={0};slice_context_new(&context);XrXirArtifact *artifact=slice_utf8_artifact(&context);
    const XrXirTypes *source=artifact->module.types;XrXirTypes types=*source;XrXirNominalTable table=*source->nominals;
    CHECK(table.identities && !table.declarations);XrXirNominalIdentity *identities=malloc((size_t)table.count*sizeof(*identities));CHECK(identities);
    memcpy(identities,table.identities,(size_t)table.count*sizeof(*identities));table.identities=identities;types.nominals=&table;
    XrXirType error=xir_native_callable_error(source,1);CHECK(error!=XR_XIR_NEVER);const XrXirTypeNode *node=xr_xir_type_node(&types,error);CHECK(node);
    uint32_t index=node->nominal.declaration;XrXirNominalIdentity saved=identities[index];CtfeTypeInfo info={0};
    CHECK(ctfe_type_info(&context,&types,error,true,&info)==XR_XIR_CTFE_OK);
    CHECK(ctfe_type_info(&context,&types,error,false,&info)==XR_XIR_CTFE_NOT_SUPPORTED);
    identities[index].native.native_id=UINT32_MAX;CHECK(ctfe_type_info(&context,&types,error,true,&info)==XR_XIR_CTFE_NOT_SUPPORTED);identities[index]=saved;
    identities[index].native.source_fingerprint[0]^=1;CHECK(ctfe_type_info(&context,&types,error,true,&info)==XR_XIR_CTFE_BAD_ARTIFACT);identities[index]=saved;
    XrXirNominalVariant variant=saved.variants[0];variant.name=(XrXirLiteral){"Wrong",5};identities[index].variants=&variant;
    CHECK(ctfe_type_info(&context,&types,error,true,&info)==XR_XIR_CTFE_BAD_ARTIFACT);identities[index]=saved;
    identities[index].field_count=1;CHECK(ctfe_type_info(&context,&types,error,true,&info)==XR_XIR_CTFE_BAD_ARTIFACT);identities[index]=saved;
    CHECK(ctfe_type_info(&context,&types,error,true,&info)==XR_XIR_CTFE_OK);free(identities);
    xr_xir_compile_artifact_free(artifact);xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Compiler native Utf8Error admission verifies fingerprint/variant/payload and external boundary");
}
static void slice_native_memory(void) {
    uint64_t peak=0;CHECK(slice_memory_attempt("from_utf8",0,&peak)==XR_XIR_OK && peak>1);
    CHECK(slice_memory_attempt("from_utf8",peak-1,NULL)==XR_XIR_MEMORY_LIMIT);
    CHECK(slice_memory_attempt("from_utf8",peak,NULL)==XR_XIR_OK && slice_memory_attempt("from_utf8",peak+1,NULL)==XR_XIR_OK);
    fprintf(stderr,"Compiler native UTF8 measured Source held peak %llu +/-1 with physical0\n",(unsigned long long)peak);
}

#include "xir/xxir_output.h"
typedef struct SliceUtf8Output {char bytes[128];size_t length;} SliceUtf8Output;
static XrXirOutputStatus slice_native_output_bytes(void *user,XrXirOutputStream stream,const char *bytes,size_t length) {
    SliceUtf8Output *output=user;CHECK(stream==XR_XIR_STDOUT && length<=sizeof(output->bytes)-output->length);
    memcpy(output->bytes+output->length,bytes,length);output->length+=length;return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus slice_native_output_group(void *user,const XrXirOutputGroup *group) {
    return xr_xir_output_render(user,group);
}
static void slice_native_packet(void) {
    XrXirCompileContext context={0};slice_context_new(&context);
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(slice_source(&context,"from_utf8",&source,&diagnostic)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);
    const XrXirFunction *body=&module->functions[slice_function(module,"run")];bool folded=false;
    for (uint32_t i=0;i<body->instruction_count;++i) {
        XrXirOp op=body->instructions[i].op;
        CHECK(op!=XR_XIR_CALL && op!=XR_XIR_INVOKE && op!=XR_XIR_CALL_INDIRECT && op!=XR_XIR_INVOKE_INDIRECT);
        if (op==XR_XIR_CONST_STRING) folded=true;
    }
    CHECK(folded);XrXirArtifact *lowered=slice_lower(&context,&source);
    uint32_t entry=slice_function(&lowered->module,"run");XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered && program);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    SliceUtf8Output output={0};XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,slice_native_output_bytes,&output,sizeof(output.bytes)};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,slice_native_output_group,&sink};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult completed=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    fprintf(stderr,"Packet owned String VM status=%u value_type=%u payload=%lld\n",(unsigned)completed.outcome.status,completed.outcome.value.type,(long long)completed.outcome.value.payload);
    CHECK(completed.outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_compile_resources_release(context.resources);
    const char *bytes=NULL;size_t count=0;CHECK(xr_xir_string_view(&result,&bytes,&count) && count==4 && !memcmp(bytes,"A\0\xc3\xa9",4));
    xr_xir_value_drop(&result);
    char path[1024],oracle[sizeof(output.bytes)];int n=snprintf(path,sizeof(path),"%s/from_utf8.stdout",XR_CTFE_SLICE_FIXTURES);
    CHECK(n>0 && (size_t)n<sizeof(path));FILE *file=fopen(path,"rb");CHECK(file);
    size_t length=fread(oracle,1,sizeof(oracle),file);CHECK(!ferror(file) && feof(file) && !fclose(file));
    CHECK(length==output.length && !memcmp(output.bytes,oracle,length));
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Actual Source folded String survives packet overwrite/producer retirement and independent VM result ownership");
}
