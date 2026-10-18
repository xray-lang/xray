/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_panic_cases.h - Authentic panic handlers and cleanup controls
 */
#ifndef XIR_CTFE_PANIC_CASES_H
#define XIR_CTFE_PANIC_CASES_H
#include "xir/xxir_panic.h"
static void reference_panic_foreign(const XrXirCompileContext *context,const XrXirCtfeRequest *request) {
    const XrXirModule *module=xr_xir_compile_artifact_module(request->artifact);XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_domain_new(context->resources,UINT64_MAX,&domain)==XR_XIR_VALUE_OK);
    XrXirPanicPayload payload={{XR_XIR_PANIC_DIVIDE,0,0,0},{0}};XrXirValue info={0},output={0};
    CHECK(xr_xir_panic_info_new(domain,&payload,&info)==XR_XIR_VALUE_OK);XrXirValue saved=info;
    XrXirCtfeRequest external=*request;external.function=reference_named(module,"accept_info");external.arguments=&info;external.argument_count=1;
    XrCompileResourceStats baseline=reference_stats(context);
    CHECK(reference_text_evaluate(&external,false,&output)==XR_XIR_CTFE_NOT_SUPPORTED && !output.type && !output.payload);
    CHECK(reference_text_evaluate(&external,true,&output)==XR_XIR_CTFE_NOT_SUPPORTED && !output.type && !output.payload);
    CHECK(!memcmp(&saved,&info,sizeof(info)) && reference_stats(context).live_bytes==baseline.live_bytes);
    xr_xir_value_drop(&info);xr_xir_domain_drop(domain);external=*request;external.function=reference_named(module,"make_info");baseline=reference_stats(context);
    CHECK(reference_text_evaluate(&external,false,&output)==XR_XIR_CTFE_NOT_LEAF && !output.type && !output.payload);
    CHECK(reference_text_evaluate(&external,true,&output)==XR_XIR_CTFE_NOT_SUPPORTED && !output.type && !output.payload);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
}
static uint64_t reference_panic_frame(const XrXirCompileContext *context,XrXirCtfeRequest *request) {
    XrCompileResourceStats baseline=reference_stats(context);XrXirValue output={0};uint64_t low=0,high=XR_XIR_CTFE_MAX_FRAME_BYTES;
    while (low<high) {
        uint64_t middle=low+(high-low)/2;request->limits.frame_bytes=middle;
        XrXirCtfeStatus status=reference_text_evaluate(request,true,&output);
        if (status==XR_XIR_CTFE_OK) { CHECK(output.type==XR_XIR_I64 && output.payload==42);xr_xir_value_drop(&output);high=middle; }
        else {
            CHECK((status==XR_XIR_CTFE_FRAME_LIMIT || status==XR_XIR_CTFE_BUDGET) && !output.type && !output.payload);
            CHECK(reference_stats(context).work<reference_limits.work-8000000 && reference_stats(context).allocated_bytes<reference_limits.allocated_bytes/2);
            low=middle+1;
        }
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    }
    CHECK(low);request->limits.frame_bytes=low;
    CHECK(reference_text_evaluate(request,true,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_I64 && output.payload==42);xr_xir_value_drop(&output);
    request->limits.frame_bytes=low-1;XrXirCtfeStatus short_frame=reference_text_evaluate(request,true,&output);
    CHECK((short_frame==XR_XIR_CTFE_FRAME_LIMIT || short_frame==XR_XIR_CTFE_BUDGET) && !output.type && !output.payload);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);request->limits.frame_bytes=XR_XIR_CTFE_MAX_FRAME_BYTES;
    fprintf(stderr,"CTFE pending panic payload/child/Info scratch frame exact%llu/minus-one status%u original-ledger headroom verified physical0\n",(unsigned long long)low,(unsigned)short_frame);
    return low;
}
static void reference_panic_direct(const XrXirCompileContext *context,XrXirSourceResult *source) {
    XrXirCheckedPacket wire={0};CHECK(xr_xir_compile_checked_write(source->checked,&wire,NULL)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);XrXirValue output={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,8}};
    memcpy(request.checked_identity,identity,32);reference_panic_foreign(context,&request);XrCompileResourceStats baseline=reference_stats(context);
    CHECK(reference_text_evaluate(&request,false,&output)==XR_XIR_CTFE_NOT_LEAF && !output.type && !output.payload);
    size_t start=source_fixture_compile_attempts;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_I64 && output.payload==42);
    size_t sites=source_fixture_compile_attempts-start;CHECK(sites);xr_xir_value_drop(&output);
    for (size_t fault=0;fault<sites;++fault) reference_calls_fault(&wire,identity,fault,sites,NULL);
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
        XrXirCtfeCallRequest limited={request,depth};CHECK(depth<=XR_XIR_CTFE_MAX_CALL_DEPTH);
        XrXirCtfeStatus status=xr_xir_compile_ctfe_calls(&limited,&output);
        if (status==XR_XIR_CTFE_OK) { CHECK(output.type==XR_XIR_I64 && output.payload==42);xr_xir_value_drop(&output);break; }
        CHECK(status==XR_XIR_CTFE_DEPTH_LIMIT && !output.type && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    }
    request.limits.output_bytes=7;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
    request.limits.output_bytes=8;request.limits.frame_bytes=1;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_FRAME_LIMIT && !output.type && !output.payload);
    request.limits.frame_bytes=XR_XIR_CTFE_MAX_FRAME_BYTES;
    CHECK(reference_panic_frame(context,&request)>1);
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_I64 && output.payload==42);xr_xir_value_drop(&output);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);xr_xir_compile_artifact_free(lowered);xr_xir_compile_checked_packet_free(&wire);
    fprintf(stderr,"CTFE real panic/PanicInfo, protected callsite and projected-ref cleanup: FI=%zu complete per-job same-owner/live; all step prefixes0..%llu and depth1..%u, output8/minus7/frame; foreign authentic PanicInfo rejected both; exact parent restored physical0\n",
        sites,(unsigned long long)steps,depth);
}
static void reference_panic_string(const XrXirValue *value) {
    const char *text=NULL;size_t length=0;
    CHECK(xr_xir_string_view(value,&text,&length) && length==2 && !memcmp(text,"ab",2));
}
static void reference_panic_owning(const XrXirCompileContext *context,XrXirSourceResult *source) {
    XrXirCheckedPacket wire={0};CHECK(xr_xir_compile_checked_write(source->checked,&wire,NULL)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);XrXirValue output={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,2}};
    memcpy(request.checked_identity,identity,32);XrCompileResourceStats baseline=reference_stats(context);
    size_t start=source_fixture_compile_attempts;CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);
    reference_panic_string(&output);size_t sites=source_fixture_compile_attempts-start;CHECK(sites);xr_xir_value_drop(&output);
    for (size_t fault=0;fault<sites;++fault) reference_calls_fault(&wire,identity,fault,sites,"ab");
    uint64_t steps=0;
    for (;;++steps) {
        CHECK(steps<XR_XIR_CTFE_MAX_STEPS);request.limits.steps=steps;
        XrXirCtfeStatus status=reference_text_evaluate(&request,true,&output);
        if (status==XR_XIR_CTFE_OK) { reference_panic_string(&output);xr_xir_value_drop(&output);break; }
        CHECK(status==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    }
    request.limits.steps=XR_XIR_CTFE_MAX_STEPS;request.limits.output_bytes=1;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
    request.limits.output_bytes=2;CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);
    reference_panic_string(&output);xr_xir_value_drop(&output);CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_checked_packet_free(&wire);
    fprintf(stderr,"CTFE caught panic owning String: FI=%zu complete per-job same-owner/live; all step prefixes0..%llu; output2/minus1 physical0\n",
        sites,(unsigned long long)steps);
}
static void reference_panic_last_owner(void) {
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&reference_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(reference_source(&context,"panic_owning_direct",&source,&diagnostic)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(&context,&source,identity);
    xr_xir_compile_source_result_free(&source);XrXirValue output={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,2}};
    memcpy(request.checked_identity,identity,32);CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);
    xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);reference_panic_string(&output);
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;xr_xir_value_drop(&output);
    source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(source_fixture_compile_attempts==attempts && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"CTFE caught panic payload String survives Source/Artifact/ledger caller death; final drop allocates nothing physical0\n");
}
static void reference_panic_work(const XrXirCompileContext *context,XrXirSourceResult *source) {
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);XrXirValue output={0};
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(module),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.limits={10000,XR_XIR_CTFE_MAX_FRAME_BYTES,8}};
    memcpy(request.checked_identity,identity,32);XrCompileResourceStats baseline=reference_stats(context);
    CHECK(baseline.work<reference_limits.work/2);
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_BUDGET && !output.type && !output.payload);
    XrCompileResourceStats spent=reference_stats(context);
    CHECK(spent.work>reference_limits.work-4000000 && spent.work<=reference_limits.work && spent.live_bytes==baseline.live_bytes);
    CHECK(spent.allocated_bytes<=reference_limits.allocated_bytes && spent.peak_bytes<=reference_limits.live_bytes);
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_BUDGET && !output.type && !output.payload);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);xr_xir_compile_artifact_free(lowered);
    fprintf(stderr,"CTFE 128-field PanicInfo metadata/receiving prepaid original owner: work=%llu/128000000 before10000 steps, retry remains BUDGET; no refund, output/live preserved physical0\n",(unsigned long long)spent.work);
}
#endif // XIR_CTFE_PANIC_CASES_H
