/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_equal_cases.h - Independent shared equality and failed owner cleanup
 */
#ifndef XIR_CTFE_EQUAL_CASES_H
#define XIR_CTFE_EQUAL_CASES_H
static void reference_owned_equal(const XrXirCompileContext *context,XrXirSourceResult *source) {
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);XrXirValue output={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_named(module,"compare"),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,1}};
    memcpy(request.checked_identity,identity,sizeof(identity));XrCompileResourceStats baseline=reference_stats(context);
    size_t first=source_fixture_compile_attempts;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_BOOL && output.payload==1);
    size_t sites=source_fixture_compile_attempts-first;CHECK(sites>0);output=(XrXirValue){0};
    for (size_t fault=0;fault<sites;++fault) {
        source_fixture_compile_injected=false;source_fixture_compile_fail_at=source_fixture_compile_attempts+fault;
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OUT_OF_MEMORY);
        CHECK(source_fixture_compile_injected && !output.type && !output.reserved && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_BOOL && output.payload==1);output=(XrXirValue){0};
    }
    XrXirCtfeCallRequest calls={request,XR_XIR_CTFE_MAX_CALL_DEPTH};
    CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_BOOL && output.payload==1);output=(XrXirValue){0};
    request.function=reference_named(module,"nan");calls.evaluation=request;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_BOOL && !output.payload);output=(XrXirValue){0};
    CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_BOOL && !output.payload);output=(XrXirValue){0};
    request.function=reference_named(module,"compare");
    uint32_t equality=UINT32_MAX;const XrXirFunction *body=&module->functions[request.function];
    for (uint32_t i=0;i<body->instruction_count;++i) if (body->instructions[i].op==XR_XIR_EQUAL) equality=i;
    CHECK(equality!=UINT32_MAX);request.limits.steps=(uint64_t)equality+1;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    request.limits.steps=XR_XIR_CTFE_MAX_STEPS;request.limits.output_bytes=0;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
    request.limits.output_bytes=1;request.limits.frame_bytes=1;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_FRAME_LIMIT && !output.type && !output.payload);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);xr_xir_compile_artifact_free(lowered);
    fprintf(stderr,"CTFE owned equality Array/Nullable/String: evaluator FI=%zu exact same-owner retry/live baseline; leaf/calls independent truth and self-NaN false; step/frame/output failure physical0\n",sites);
}
#endif // XIR_CTFE_EQUAL_CASES_H
