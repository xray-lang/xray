/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_text_cases.h - Independent scalar text and owning failure controls
 */
#ifndef XIR_CTFE_TEXT_CASES_H
#define XIR_CTFE_TEXT_CASES_H
typedef struct ReferenceTextRow { XrXirType type;uint64_t bits;const char *text; } ReferenceTextRow;
static XrXirCtfeStatus reference_text_evaluate(XrXirCtfeRequest *request,bool calls,XrXirValue *output) {
    XrXirCtfeCallRequest call={*request,XR_XIR_CTFE_MAX_CALL_DEPTH};
    return calls?xr_xir_compile_ctfe_calls(&call,output):xr_xir_compile_ctfe_leaf(request,output);
}
static void reference_text_value(const XrXirValue *value,const char *expected) {
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(value,&bytes,&length) && length==strlen(expected) && !memcmp(bytes,expected,length));
}
static void reference_text_direct(const XrXirCompileContext *context,XrXirSourceResult *source) {
    static const ReferenceTextRow rows[]={
        {XR_XIR_F64,UINT64_C(0x0000000000000000),"0.0"},
        {XR_XIR_F64,UINT64_C(0x8000000000000000),"-0.0"},
        {XR_XIR_F64,UINT64_C(0x0000000000000001),"5e-324"},
        {XR_XIR_F64,UINT64_C(0x7fefffffffffffff),"1.7976931348623157e+308"},
        {XR_XIR_F64,UINT64_C(0x7ff0000000000000),"inf"},
        {XR_XIR_F64,UINT64_C(0x7ff8000000000000),"nan"},
        {XR_XIR_F32,UINT64_C(0x0000000000000001),"1e-45"},
        {XR_XIR_F32,UINT64_C(0x000000007f7fffff),"3.4028235e+38"},
        {XR_XIR_F32,UINT64_C(0x00000000ff800000),"-inf"},
        {XR_XIR_F32,UINT64_C(0x000000007fc00000),"nan"},
        {XR_XIR_F32,UINT64_C(0x000000003fc00000),"1.5"},
        {XR_XIR_F64,UINT64_C(0x400c000000000000),"3.5"}};
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    XrXirDomain *reader=NULL;CHECK(xr_xir_compile_domain_new(context->resources,UINT64_MAX,&reader)==XR_XIR_VALUE_OK);
    XrXirValue args[2]={{0}},output={0};
    XrXirCtfeRequest request={.artifact=lowered,.target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .arguments=args,.argument_count=1,.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}};
    memcpy(request.checked_identity,identity,sizeof(identity));
    for (size_t i=0;i<sizeof(rows)/sizeof(rows[0]);++i) {
        const ReferenceTextRow *row=&rows[i];args[0]=(XrXirValue){row->type,0,(int64_t)row->bits};
        request.function=reference_named(module,row->type==XR_XIR_F32?"format32":"format");request.argument_count=1;
        for (unsigned mode=0;mode<2;++mode) {
            XrXirCtfeStatus status=reference_text_evaluate(&request,mode!=0,&output);
            if (strlen(row->text)>XR_XIR_CTFE_MAX_OUTPUT_BYTES) CHECK(status==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
            else { CHECK(status==XR_XIR_CTFE_OK);reference_text_value(&output,row->text);xr_xir_value_drop(&output); }
        }
        CHECK(xr_xir_string_new(reader,row->text,strlen(row->text),&args[1])==XR_XIR_VALUE_OK);
        request.function=reference_named(module,row->type==XR_XIR_F32?"check32":"check");request.argument_count=2;
        for (unsigned mode=0;mode<2;++mode) {
            CHECK(reference_text_evaluate(&request,mode!=0,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_BOOL && output.payload==1);
            output=(XrXirValue){0};reference_text_value(&args[1],row->text);
        }
        xr_xir_value_drop(&args[1]);
    }
    args[0]=(XrXirValue){XR_XIR_F64,0,INT64_C(0x400c000000000000)};
    request.function=reference_named(module,"format");request.argument_count=1;request.limits.output_bytes=3;
    XrCompileResourceStats baseline=reference_stats(context);size_t counts[2]={0};
    for (unsigned mode=0;mode<2;++mode) {
        size_t first=source_fixture_compile_attempts;
        CHECK(reference_text_evaluate(&request,mode!=0,&output)==XR_XIR_CTFE_OK);reference_text_value(&output,"3.5");
        counts[mode]=source_fixture_compile_attempts-first;CHECK(counts[mode]>0);xr_xir_value_drop(&output);
        for (size_t fault=0;fault<counts[mode];++fault) {
            source_fixture_compile_injected=false;source_fixture_compile_fail_at=source_fixture_compile_attempts+fault;
            CHECK(reference_text_evaluate(&request,mode!=0,&output)==XR_XIR_CTFE_OUT_OF_MEMORY);
            CHECK(source_fixture_compile_injected && !output.type && !output.reserved && !output.payload);
            CHECK(reference_stats(context).live_bytes==baseline.live_bytes);source_fixture_compile_fail_at=SIZE_MAX;
            CHECK(reference_text_evaluate(&request,mode!=0,&output)==XR_XIR_CTFE_OK);reference_text_value(&output,"3.5");xr_xir_value_drop(&output);
        }
    }
    request.limits.output_bytes=2;
    CHECK(reference_text_evaluate(&request,false,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
    const XrXirFunctionLayout *layout=xr_xir_compile_artifact_layout(lowered,request.function);
    CHECK(layout && (uint64_t)layout->frame_bytes+(uint64_t)layout->outgoing_count*sizeof(XrXirValue)+1024<4096);
    request.limits.output_bytes=3;request.limits.frame_bytes=4096;
    CHECK(reference_text_evaluate(&request,false,&output)==XR_XIR_CTFE_FRAME_LIMIT && !output.type && !output.payload);
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_FRAME_LIMIT && !output.type && !output.payload);
    request.limits.frame_bytes=XR_XIR_CTFE_MAX_FRAME_BYTES;request.limits.steps=1;
    CHECK(reference_text_evaluate(&request,false,&output)==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    xr_xir_domain_drop(reader);xr_xir_compile_artifact_free(lowered);
    fprintf(stderr,"CTFE scalar text: independent12 IEEE rows both entries; long internal text with output8 denial; leaf FI=%zu calls FI=%zu complete same-owner/live baseline; output3/minus2, formatter frame and step physical0\n",counts[0],counts[1]);
}
static void reference_text_last_owner(void) {
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&reference_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(reference_source(&context,"text_direct",&source,&diagnostic)==XR_XIR_OK);uint8_t identity[32]={0};
    XrXirArtifact *lowered=reference_lower(&context,&source,identity);xr_xir_compile_source_result_free(&source);
    XrXirValue input={XR_XIR_F64,0,INT64_C(0x400c000000000000)},value={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_named(xr_xir_compile_artifact_module(lowered),"format"),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.arguments=&input,.argument_count=1,
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,3}};
    memcpy(request.checked_identity,identity,sizeof(identity));CHECK(reference_text_evaluate(&request,true,&value)==XR_XIR_CTFE_OK);
    xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);reference_text_value(&value,"3.5");
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;xr_xir_value_drop(&value);source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(source_fixture_compile_attempts==attempts && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"CTFE actual formatted String survives Source/Artifact/ledger caller death; final drop allocates nothing physical0\n");
}
#endif // XIR_CTFE_TEXT_CASES_H
