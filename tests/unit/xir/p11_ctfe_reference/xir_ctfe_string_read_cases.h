/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_string_read_cases.h - Independent UTF-8 search and prepaid-work controls
 */
#ifndef XIR_CTFE_STRING_READ_CASES_H
#define XIR_CTFE_STRING_READ_CASES_H
static void reference_string_read_direct(const XrXirCompileContext *context,XrXirSourceResult *source) {
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t search=reference_named(module,"search"),last=reference_named(module,"last"),order=reference_named(module,"order");
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);
    XrXirDomainStats domain_baseline=xr_xir_domain_stats(domain);
    XrXirValue args[3]={{0},{0},{XR_XIR_I64,0,1}},output={0};
    CHECK(xr_xir_string_new(domain,"\xc3\xa9" "a" "\xc3\xa9",5,&args[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"\xc3\xa9",2,&args[1])==XR_XIR_VALUE_OK);
    XrXirCtfeRequest request={.artifact=lowered,.function=search,
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.arguments=args,.argument_count=3,
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,8}};
    memcpy(request.checked_identity,identity,sizeof(identity));
    XrCompileResourceStats baseline=reference_stats(context);
    for (int64_t start=-1;start<=4;++start) {
        args[2].payload=start;XrXirCtfeStatus expected=start<0 || start>3?XR_XIR_CTFE_NUMERIC_RANGE:XR_XIR_CTFE_OK;
        XrXirCtfeCallRequest calls={request,1};
        for (unsigned c=0;c<2;++c) {
            CHECK((c?xr_xir_compile_ctfe_calls(&calls,&output):xr_xir_compile_ctfe_leaf(&request,&output))==expected);
            if (expected==XR_XIR_CTFE_OK) CHECK(output.type==XR_XIR_I64 && !output.reserved && output.payload==(start==0?0:start==3?-1:2));
            else CHECK(!output.type && !output.reserved && !output.payload);
            output=(XrXirValue){0};CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
        }
    }
    request.function=last;request.argument_count=2;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);
    CHECK(output.type==XR_XIR_I64 && output.payload==2);output=(XrXirValue){0};
    request.function=order;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);
    CHECK(output.type==XR_XIR_BOOL && !output.payload);output=(XrXirValue){0};
    request.function=search;request.argument_count=3;args[2].payload=1;
    size_t first=source_fixture_compile_attempts;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);output=(XrXirValue){0};
    size_t sites=source_fixture_compile_attempts-first;CHECK(sites>0);
    for (size_t fault=0;fault<sites;++fault) {
        source_fixture_compile_injected=false;source_fixture_compile_fail_at=source_fixture_compile_attempts+fault;
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OUT_OF_MEMORY);
        CHECK(source_fixture_compile_injected && !output.type && !output.reserved && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);
        CHECK(output.type==XR_XIR_I64 && output.payload==2);output=(XrXirValue){0};
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    }
    request.limits.steps=1;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_STEP_LIMIT);
    CHECK(!output.type && !output.reserved && !output.payload && reference_stats(context).live_bytes==baseline.live_bytes);
    request.limits.steps=XR_XIR_CTFE_MAX_STEPS;
    xr_xir_value_drop(&args[0]);xr_xir_value_drop(&args[1]);
    char text[60000],pattern[30000];memset(text,'a',sizeof(text));memset(pattern,'a',sizeof(pattern));pattern[sizeof(pattern)-1]='b';
    CHECK(xr_xir_string_new(domain,text,sizeof(text),&args[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,pattern,sizeof(pattern),&args[1])==XR_XIR_VALUE_OK);args[2].payload=0;
    XrXirDomainStats original=xr_xir_domain_stats(domain);XrCompileResourceStats before=reference_stats(context);
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_BUDGET);
    CHECK(!output.type && !output.reserved && !output.payload && reference_stats(context).live_bytes==baseline.live_bytes);
    CHECK(xr_xir_domain_stats(domain).live_bytes==original.live_bytes);
    const char *bytes=NULL;size_t count=0;
    CHECK(xr_xir_string_view(&args[0],&bytes,&count) && count==sizeof(text) && !memcmp(bytes,text,count));
    CHECK(xr_xir_string_view(&args[1],&bytes,&count) && count==sizeof(pattern) && !memcmp(bytes,pattern,count));
    XrCompileResourceStats after=reference_stats(context);
    CHECK(after.work>=before.work && after.allocated_bytes>=before.allocated_bytes && after.work<=reference_limits.work);
    xr_xir_value_drop(&args[0]);xr_xir_value_drop(&args[1]);CHECK(xr_xir_domain_stats(domain).live_bytes==domain_baseline.live_bytes);xr_xir_domain_drop(domain);
    xr_xir_compile_artifact_free(lowered);
    fprintf(stderr,"CTFE String read UTF-8 indices/bounds leaf and calls; evaluator FI=%zu same-owner retry; step cleanup; adversarial search prepaid budget refusal preserves external arguments and live baseline; physical0\n",sites);
}
#endif // XIR_CTFE_STRING_READ_CASES_H
