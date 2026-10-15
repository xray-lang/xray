/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_projection_cases.h - Real projected argument and preparation gates
 */
#ifndef XIR_CTFE_PROJECTION_CASES_H
#define XIR_CTFE_PROJECTION_CASES_H
static void reference_projection_direct(const XrXirCompileContext *context,XrXirSourceResult *source) {
    XrXirCheckedPacket wire={0};CHECK(xr_xir_compile_checked_write(source->checked,&wire,NULL)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);XrXirValue output={0};
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t run=reference_run_function(module),projects=0;
    for (uint32_t i=0;i<module->functions[run].instruction_count;++i)
        if (module->functions[run].instructions[i].op==XR_XIR_CELL_PROJECT) ++projects;
    CHECK(projects==2);XrXirCtfeRequest request={.artifact=lowered,.function=run,.target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,8}};
    memcpy(request.checked_identity,identity,32);XrCompileResourceStats baseline=reference_stats(context);size_t start=source_fixture_compile_attempts;
    CHECK(reference_text_evaluate(&request,false,&output)==XR_XIR_CTFE_NOT_LEAF && !output.type && !output.payload);
    start=source_fixture_compile_attempts;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_I64 && output.payload==42);
    size_t sites=source_fixture_compile_attempts-start;CHECK(sites>0);xr_xir_value_drop(&output);
    for (size_t fault=0;fault<sites;++fault) reference_calls_fault(&wire,identity,fault,sites,NULL);
    uint64_t steps=0;
    for (;;++steps) {
        CHECK(steps<XR_XIR_CTFE_MAX_STEPS);request.limits.steps=steps;
        XrXirCtfeStatus status=reference_text_evaluate(&request,true,&output);
        if (status==XR_XIR_CTFE_OK) { CHECK(output.type==XR_XIR_I64 && output.payload==42);xr_xir_value_drop(&output);break; }
        CHECK(status==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    }
    request.limits.steps=XR_XIR_CTFE_MAX_STEPS;uint64_t low=0,high=XR_XIR_CTFE_MAX_FRAME_BYTES;
    while (low<high) {
        uint64_t middle=low+(high-low)/2;request.limits.frame_bytes=middle;
        XrXirCtfeStatus status=reference_text_evaluate(&request,true,&output);
        if (status==XR_XIR_CTFE_OK) { CHECK(output.type==XR_XIR_I64 && output.payload==42);xr_xir_value_drop(&output);high=middle; }
        else {
            CHECK((status==XR_XIR_CTFE_FRAME_LIMIT || status==XR_XIR_CTFE_BUDGET) && !output.type && !output.payload);
            /* Common Value admission reports insufficient scratch as LIMIT.
             * The original ledger must still have ample work and allocation. */
            CHECK(reference_stats(context).work<reference_limits.work-8000000 &&
                reference_stats(context).allocated_bytes<reference_limits.allocated_bytes/2);low=middle+1;
        }
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    }
    CHECK(low);request.limits.frame_bytes=low;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_I64 && output.payload==42);xr_xir_value_drop(&output);
    request.limits.frame_bytes=low-1;
    XrXirCtfeStatus short_frame=reference_text_evaluate(&request,true,&output);
    CHECK((short_frame==XR_XIR_CTFE_FRAME_LIMIT || short_frame==XR_XIR_CTFE_BUDGET) && !output.type && !output.payload);
    request.limits.frame_bytes=XR_XIR_CTFE_MAX_FRAME_BYTES;request.limits.output_bytes=7;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
    request.limits.output_bytes=8;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_I64 && output.payload==42);xr_xir_value_drop(&output);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);uint32_t completed=0;
    for (;;) {
        XrXirCtfeStatus status=reference_text_evaluate(&request,true,&output);
        if (status!=XR_XIR_CTFE_OK) { CHECK(status==XR_XIR_CTFE_BUDGET && !output.type && !output.payload);break; }
        CHECK(output.type==XR_XIR_I64 && output.payload==42);xr_xir_value_drop(&output);++completed;
    }
    uint64_t spent=reference_stats(context).work;CHECK(completed && spent>reference_limits.work-8000000 && spent<=reference_limits.work);
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_BUDGET && !output.type && !output.payload);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);xr_xir_compile_artifact_free(lowered);xr_xir_compile_checked_packet_free(&wire);
    fprintf(stderr,"CTFE actual two projected Tuple roots: complete FI=%zu per-job same-owner/live, all step prefixes0..%llu, simultaneous child/argument scratch frame exact%llu/minus-one status%u, output8/minus7, canonical roots restored physical0; same-owner exhaustion work%llu/128000000 after%u successes, retry BUDGET\n",
        sites,(unsigned long long)steps,(unsigned long long)low,(unsigned)short_frame,(unsigned long long)spent,completed);
}
static void reference_projection_string(const XrXirValue *value) {
    const char *text=NULL;size_t length=0;
    CHECK(xr_xir_string_view(value,&text,&length) && length==2 && !memcmp(text,"ab",2));
}
static void reference_projection_owning(const XrXirCompileContext *context,XrXirSourceResult *source) {
    XrXirCheckedPacket wire={0};CHECK(xr_xir_compile_checked_write(source->checked,&wire,NULL)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);XrXirValue output={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,2}};
    memcpy(request.checked_identity,identity,32);XrCompileResourceStats baseline=reference_stats(context);
    size_t start=source_fixture_compile_attempts;CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);
    reference_projection_string(&output);size_t sites=source_fixture_compile_attempts-start;CHECK(sites);xr_xir_value_drop(&output);
    for (size_t fault=0;fault<sites;++fault) reference_calls_fault(&wire,identity,fault,sites,"ab");
    uint64_t steps=0;
    for (;;++steps) {
        CHECK(steps<XR_XIR_CTFE_MAX_STEPS);request.limits.steps=steps;
        XrXirCtfeStatus status=reference_text_evaluate(&request,true,&output);
        if (status==XR_XIR_CTFE_OK) { reference_projection_string(&output);xr_xir_value_drop(&output);break; }
        CHECK(status==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    }
    request.limits.steps=XR_XIR_CTFE_MAX_STEPS;request.limits.output_bytes=1;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
    request.limits.output_bytes=2;CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);
    reference_projection_string(&output);xr_xir_value_drop(&output);CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_checked_packet_free(&wire);
    fprintf(stderr,"CTFE projected owning String: FI=%zu complete per-job same-owner/live; all step prefixes0..%llu; output2/minus1 physical0\n",
        sites,(unsigned long long)steps);
}
static void reference_projection_last_owner(void) {
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&reference_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(reference_source(&context,"project_owning_direct",&source,&diagnostic)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(&context,&source,identity);
    xr_xir_compile_source_result_free(&source);XrXirValue output={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,2}};
    memcpy(request.checked_identity,identity,32);CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);
    xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);reference_projection_string(&output);
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;xr_xir_value_drop(&output);
    source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(source_fixture_compile_attempts==attempts && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"CTFE projected String survives Source/Artifact/ledger caller death; final drop allocates nothing physical0\n");
}
#endif // XIR_CTFE_PROJECTION_CASES_H
