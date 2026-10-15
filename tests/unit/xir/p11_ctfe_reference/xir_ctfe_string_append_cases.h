/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_string_append_cases.h - Complete String mutation ownership controls
 */
#ifndef XIR_CTFE_STRING_APPEND_CASES_H
#define XIR_CTFE_STRING_APPEND_CASES_H
static void reference_append_value(const XrXirValue *value) {
    static const char expected[]="A\0\xc3\xa9\xe4\xb8\xad" "A\0\xc3\xa9\xe4\xb8\xad";
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(value,&bytes,&length) && length==sizeof(expected)-1 &&
        !memcmp(bytes,expected,length));
}
static void reference_append_direct(const XrXirCompileContext *context,XrXirSourceResult *source) {
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t build=reference_named(module,"build"),scalar=reference_named(module,"scalar");
    XrXirValue output={0},input={XR_XIR_F64,0,INT64_C(0x400c000000000000)};
    XrXirCtfeRequest request={.artifact=lowered,.function=build,
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,14}};
    memcpy(request.checked_identity,identity,sizeof(identity));
    XrCompileResourceStats baseline=reference_stats(context);size_t sites[2]={0};
    for (unsigned mode=0;mode<2;++mode) {
        size_t first=source_fixture_compile_attempts;
        CHECK(reference_text_evaluate(&request,mode!=0,&output)==XR_XIR_CTFE_OK);
        reference_append_value(&output);sites[mode]=source_fixture_compile_attempts-first;
        CHECK(sites[mode]>0);xr_xir_value_drop(&output);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
        for (size_t fault=0;fault<sites[mode];++fault) {
            XrCompileResourceStats before=reference_stats(context);
            source_fixture_compile_injected=false;source_fixture_compile_fail_at=source_fixture_compile_attempts+fault;
            CHECK(reference_text_evaluate(&request,mode!=0,&output)==XR_XIR_CTFE_OUT_OF_MEMORY);
            CHECK(source_fixture_compile_injected && !output.type && !output.reserved && !output.payload);
            CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
            source_fixture_compile_fail_at=SIZE_MAX;
            CHECK(reference_text_evaluate(&request,mode!=0,&output)==XR_XIR_CTFE_OK);
            reference_append_value(&output);xr_xir_value_drop(&output);
            XrCompileResourceStats after=reference_stats(context);
            CHECK(after.live_bytes==baseline.live_bytes && after.work>=before.work &&
                after.allocated_bytes>=before.allocated_bytes);
        }
    }
    request.limits.output_bytes=13;
    for (unsigned mode=0;mode<2;++mode) {
        CHECK(reference_text_evaluate(&request,mode!=0,&output)==XR_XIR_CTFE_OUTPUT_LIMIT);
        CHECK(!output.type && !output.reserved && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    }
    request.limits.output_bytes=14;request.limits.steps=1;
    for (unsigned mode=0;mode<2;++mode) {
        CHECK(reference_text_evaluate(&request,mode!=0,&output)==XR_XIR_CTFE_STEP_LIMIT);
        CHECK(!output.type && !output.reserved && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    }
    request.limits.steps=XR_XIR_CTFE_MAX_STEPS;request.function=scalar;
    request.arguments=&input;request.argument_count=1;request.limits.output_bytes=3;
    for (unsigned mode=0;mode<2;++mode) {
        CHECK(reference_text_evaluate(&request,mode!=0,&output)==XR_XIR_CTFE_OK);
        reference_text_value(&output,"3.5");xr_xir_value_drop(&output);
    }
    request.limits.frame_bytes=4096;
    for (unsigned mode=0;mode<2;++mode) {
        CHECK(reference_text_evaluate(&request,mode!=0,&output)==XR_XIR_CTFE_FRAME_LIMIT);
        CHECK(!output.type && !output.reserved && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    }
    xr_xir_compile_artifact_free(lowered);
    fprintf(stderr,"CTFE String append: UTF8/NUL, COW/self append, output14/13, steps, scalar format frame; complete leaf FI=%zu calls FI=%zu same-owner retry physical0\n",sites[0],sites[1]);
}
static void reference_append_last_owner(void) {
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&reference_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(reference_source(&context,"string_append_direct",&source,&diagnostic)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(&context,&source,identity);
    xr_xir_compile_source_result_free(&source);
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_named(xr_xir_compile_artifact_module(lowered),"build"),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,14}};
    memcpy(request.checked_identity,identity,sizeof(identity));XrXirValue output={0};
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);
    xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);
    reference_append_value(&output);size_t attempts=source_fixture_compile_attempts;
    source_fixture_compile_fail_at=attempts;xr_xir_value_drop(&output);source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(source_fixture_compile_attempts==attempts && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"CTFE appended String survives Source/Artifact/ledger caller death; final drop allocates nothing physical0\n");
}
#endif // XIR_CTFE_STRING_APPEND_CASES_H
