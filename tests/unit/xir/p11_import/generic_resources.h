/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * generic_resources.h - Whole Source failures retain their original receiver
 */
static void generic_resources(const char *text,bool library,bool prefix) {
    XrXirCompileContext census;context_new(&census);XrCompileResourceStats baseline=stats(&census);
    XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};size_t first=source_fixture_compile_attempts;
    CHECK(generic_source(&census,text,library,&output,&diagnostic)==XR_XIR_OK);
    size_t sites=source_fixture_compile_attempts-first;CHECK(sites);XrCompileResourceStats used=stats(&census);
    xr_xir_compile_source_result_free(&output);CHECK(stats(&census).live_bytes==baseline.live_bytes);
    xr_compile_resources_release(census.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    uint64_t work=used.work-baseline.work,allocated=used.allocated_bytes-baseline.allocated_bytes;
    fprintf(stderr,"Generic Source library=%u actualFI=%zu allocated=%llu peak=%llu work=%llu\n",library,sites,
        (unsigned long long)used.allocated_bytes,(unsigned long long)used.peak_bytes,(unsigned long long)used.work);
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=caps;
        if (!axis) limits.work=used.work-minus;
        else if (axis==1) limits.allocated_bytes=used.allocated_bytes-minus;
        else limits.live_bytes=used.peak_bytes-minus;
        XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits();XrCompileResourceStats before=stats(&context);
        XrXirStatus status=generic_source(&context,text,library,&output,&diagnostic);
        if (status!=(minus?XR_XIR_BUDGET:XR_XIR_OK)) fprintf(stderr,"Generic bound library=%u axis=%u minus=%u status=%u %s\n",library,axis,minus,status,diagnostic.message);
        CHECK(status==(minus?XR_XIR_BUDGET:XR_XIR_OK));xr_xir_compile_source_result_free(&output);
        CHECK(stats(&context).live_bytes==before.live_bytes);
        if (minus) {
            CHECK(generic_source(&context,text,library,&output,&diagnostic)==XR_XIR_BUDGET && !output.checked && !output.snapshot);
            CHECK(stats(&context).live_bytes==before.live_bytes);
        }
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    size_t trials=prefix?(sites==1?1:2):sites,retry_ok=0,retry_budget=0;
    for (size_t trial=0;trial<trials;++trial) {
        size_t fault=prefix?(trial?sites-1:0):trial;
        XrXirCompileContext context;context_new(&context);XrCompileResources *same=context.resources;
        XrCompileResourceStats before=stats(&context);first=source_fixture_compile_attempts;
        source_fixture_compile_fail_at=first+fault;source_fixture_compile_injected=false;
        XrXirStatus status=generic_source(&context,text,library,&output,&diagnostic);
        if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"Generic FI library=%u fault=%zu/%zu status=%u %s\n",library,fault,sites,status,diagnostic.message);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && !output.checked && !output.snapshot);
        CHECK(source_fixture_compile_attempts==first+fault+1 && stats(&context).live_bytes==before.live_bytes);
        source_fixture_compile_fail_at=SIZE_MAX;XrCompileResourceStats failed=stats(&context);
        bool exhausted=work>caps.work-failed.work || allocated>caps.allocated_bytes-failed.allocated_bytes;
        status=generic_source(&context,text,library,&output,&diagnostic);
        CHECK(status==(exhausted?XR_XIR_BUDGET:XR_XIR_OK));
        if (exhausted) {
            CHECK(!output.checked && !output.snapshot);
            CHECK(generic_source(&context,text,library,&output,&diagnostic)==XR_XIR_BUDGET && !output.checked && !output.snapshot);
            ++retry_budget;
        } else {
            CHECK(output.checked && output.snapshot);xr_xir_compile_source_result_free(&output);
            CHECK(stats(&context).work-failed.work==work && stats(&context).allocated_bytes-failed.allocated_bytes==allocated);
            ++retry_ok;
        }
        CHECK(context.resources==same && stats(&context).live_bytes==before.live_bytes);
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
        if (!(trial%256) || trial+1==trials) fprintf(stderr,"Generic FI library=%u point=%zu/%zu same-owner retry/live physical0\n",library,fault+1,sites);
    }
    fprintf(stderr,"Generic Source library=%u %s actualFI=%zu selected=%zu retryOK=%zu retryBudget=%zu threeaxes exact/minus1 physical0\n",
        library,prefix?"FIRST_LAST_ONLY":"COMPLETE_ALL_ORDINALS",sites,trials,retry_ok,retry_budget);
}
