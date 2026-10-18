/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_cleanup_cases.h - Actual frontier, reborrow and terminal cleanup gates
 */
#ifndef XIR_CTFE_CLEANUP_CASES_H
#define XIR_CTFE_CLEANUP_CASES_H
static void reference_cleanup_foreign(const XrXirCompileContext *context,XrXirCtfeRequest *request) {
    const XrXirModule *module=xr_xir_compile_artifact_module(request->artifact);
    uint32_t function=reference_named(module,"inc");XrXirType type=module->functions[function].parameters[0];
    XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(context,module->types,&arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_domain_new(context->resources,UINT64_MAX,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={.arena=arena,.domain=domain,.work=UINT64_MAX,.scratch_bytes=65536};
    XrXirValue initial={XR_XIR_I64,0,40},cell={0},output={0},read={0};
    CHECK(xr_xir_cell_new(domain,arena,type,&initial,&admission,&cell)==XR_XIR_VALUE_OK);
    XrXirValue unchanged=cell;XrCompileResourceStats baseline=reference_stats(context);
    XrXirCtfeRequest external=*request;external.function=function;external.arguments=&cell;external.argument_count=1;
    CHECK(reference_text_evaluate(&external,false,&output)==XR_XIR_CTFE_NOT_LEAF && !output.type && !output.payload);
    CHECK(reference_text_evaluate(&external,true,&output)==XR_XIR_CTFE_NOT_SUPPORTED && !output.type && !output.payload);
    CHECK(!memcmp(&unchanged,&cell,sizeof(cell)) && reference_stats(context).live_bytes==baseline.live_bytes);
    CHECK(xr_xir_cell_read(&cell,&read)==XR_XIR_VALUE_OK && read.type==XR_XIR_I64 && read.payload==40);xr_xir_value_drop(&read);
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;xr_xir_value_drop(&cell);
    source_fixture_compile_fail_at=SIZE_MAX;CHECK(source_fixture_compile_attempts==attempts);
}
static void reference_cleanup_direct(const XrXirCompileContext *context,XrXirSourceResult *source) {
    XrXirCheckedPacket wire={0};CHECK(xr_xir_compile_checked_write(source->checked,&wire,NULL)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);XrXirValue output={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,8}};
    memcpy(request.checked_identity,identity,32);reference_cleanup_foreign(context,&request);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);bool protected=false;
    for (uint32_t f=0;f<module->function_count;++f) {
        if (!module->declarations->functions[f].cleanup_owner) continue;
        XrXirCtfeRequest external=request;external.function=f;
        CHECK(reference_text_evaluate(&external,true,&output)==XR_XIR_CTFE_NOT_SUPPORTED && !output.type && !output.payload);protected=true;
    }
    CHECK(protected);
    XrCompileResourceStats baseline=reference_stats(context);size_t start=source_fixture_compile_attempts;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_I64 && output.payload==42);
    size_t sites=source_fixture_compile_attempts-start;CHECK(sites>1);xr_xir_value_drop(&output);
    for (size_t fault=0;fault<sites;++fault) reference_calls_fault(&wire,identity,fault,sites,NULL);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    uint64_t steps=0;
    for (;;++steps) {
        CHECK(steps<XR_XIR_CTFE_MAX_STEPS);request.limits.steps=steps;
        XrXirCtfeStatus status=reference_text_evaluate(&request,true,&output);
        if (status==XR_XIR_CTFE_OK) { CHECK(output.type==XR_XIR_I64 && output.payload==42);xr_xir_value_drop(&output);break; }
        CHECK(status==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    }
    request.limits.steps=XR_XIR_CTFE_MAX_STEPS;uint32_t depth=1;
    for (;;++depth) {
        CHECK(depth<=XR_XIR_CTFE_MAX_CALL_DEPTH);XrXirCtfeCallRequest call={request,depth};
        XrXirCtfeStatus status=xr_xir_compile_ctfe_calls(&call,&output);
        if (status==XR_XIR_CTFE_OK) { CHECK(output.type==XR_XIR_I64 && output.payload==42);xr_xir_value_drop(&output);break; }
        CHECK(status==XR_XIR_CTFE_DEPTH_LIMIT && !output.type && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    }
    request.limits.frame_bytes=1;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_FRAME_LIMIT && !output.type && !output.payload);
    request.limits.frame_bytes=XR_XIR_CTFE_MAX_FRAME_BYTES;request.limits.output_bytes=7;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
    request.limits.output_bytes=8;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_I64 && output.payload==42);
    xr_xir_value_drop(&output);CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_checked_packet_free(&wire);
    fprintf(stderr,"CTFE lexical cleanup/ref actual reborrow: complete FI=%zu per-job same-owner successful retry/live; all step prefixes0..%llu and depth1..%u; output8/minus7/frame and foreign Cell denial; exact parent restored physical0\n",
        sites,(unsigned long long)steps,depth);
}
static void reference_cleanup_string(const XrXirValue *value) {
    const char *bytes=NULL;size_t length=0;
    CHECK(value->type==XR_XIR_STRING && xr_xir_string_view(value,&bytes,&length) && length==1 && bytes[0]=='a');
}
static void reference_cleanup_owning(const XrXirCompileContext *context,XrXirSourceResult *source) {
    XrXirCheckedPacket wire={0};CHECK(xr_xir_compile_checked_write(source->checked,&wire,NULL)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);XrXirValue output={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,1}};
    memcpy(request.checked_identity,identity,32);XrCompileResourceStats baseline=reference_stats(context);
    size_t start=source_fixture_compile_attempts;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);reference_cleanup_string(&output);
    size_t sites=source_fixture_compile_attempts-start;CHECK(sites>0);xr_xir_value_drop(&output);
    for (size_t fault=0;fault<sites;++fault) reference_calls_fault(&wire,identity,fault,sites,"a");
    uint64_t steps=0;
    for (;;++steps) {
        CHECK(steps<XR_XIR_CTFE_MAX_STEPS);request.limits.steps=steps;
        XrXirCtfeStatus status=reference_text_evaluate(&request,true,&output);
        if (status==XR_XIR_CTFE_OK) { reference_cleanup_string(&output);xr_xir_value_drop(&output);break; }
        CHECK(status==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    }
    request.limits.steps=XR_XIR_CTFE_MAX_STEPS;request.limits.output_bytes=0;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
    request.limits.output_bytes=1;CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);
    reference_cleanup_string(&output);xr_xir_value_drop(&output);CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);
    xr_xir_compile_artifact_free(lowered);reference_cleanup_string(&output);xr_xir_value_drop(&output);
    xr_xir_compile_checked_packet_free(&wire);
    fprintf(stderr,"CTFE cleanup owning return snapshot=a after private Cell becomes ab; FI=%zu complete per-job same-owner/live; all step prefixes0..%llu, output1/minus0, Artifact death physical0\n",sites,(unsigned long long)steps);
}
static void reference_cleanup_last_owner(void) {
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&reference_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(reference_source(&context,"cleanup_owning_direct",&source,&diagnostic)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(&context,&source,identity);
    xr_xir_compile_source_result_free(&source);XrXirValue output={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,1}};
    memcpy(request.checked_identity,identity,32);CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);
    xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);reference_cleanup_string(&output);
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;xr_xir_value_drop(&output);
    source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(source_fixture_compile_attempts==attempts && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"CTFE actual cleanup snapshot survives Source/Artifact/ledger caller death; final drop allocates nothing physical0\n");
}
#endif // XIR_CTFE_CLEANUP_CASES_H
