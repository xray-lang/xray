/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_calls_resources.h - Complete finite direct-call evaluator resources
 *
 * KEY CONCEPT:
 *   Every real evaluator allocation is injected, including all three frames.
 *   Recovery keeps the same finite owner; fresh owners carry independent axes.
 */
#ifndef XIR_CTFE_CALLS_RESOURCES_H
#define XIR_CTFE_CALLS_RESOURCES_H
typedef struct CtfeCallsResourceReport {
    XrCompileResourceStats spent;
    XrXirCtfeStatus status;
    size_t sites;
} CtfeCallsResourceReport;
static XrXirCtfeStatus ctfe_calls_producer_status(XrXirStatus status) {
    if (status==XR_XIR_OK) return XR_XIR_CTFE_OK;
    if (status==XR_XIR_BUDGET) return XR_XIR_CTFE_BUDGET;
    if (status==XR_XIR_OUT_OF_MEMORY) return XR_XIR_CTFE_OUT_OF_MEMORY;
    return XR_XIR_CTFE_BAD_ARTIFACT;
}
static void ctfe_calls_resource_run(const XrCompileResourceLimits *limits,size_t fault,
    bool retry,CtfeCallsResourceReport *output) {
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    XrXirCompileContext context={0};CtfeFixture fixture={0};XrCompileResourceStats baseline={0};
    CtfeCallsResourceReport report={0};XrXirValue value={0};
    CHECK(xr_compile_resources_new(limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirStatus produced=ctfe_calls_fixture_build(&context,&fixture,NULL);
    report.status=ctfe_calls_producer_status(produced);
    if (produced==XR_XIR_OK) {
        XrXirCtfeCallRequest request=ctfe_calls_request(&fixture,"answer",NULL,0);
        size_t attempts=source_fixture_compile_attempts;
        size_t blocks=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
        XrCompileResourceStats before=ctfe_stats(&fixture);
        if (fault!=SIZE_MAX) source_fixture_compile_fail_at=attempts+fault;
        report.status=xr_xir_compile_ctfe_calls(&request,&value);
        report.sites=source_fixture_compile_attempts-attempts;
        CHECK(ctfe_stats(&fixture).live_bytes==before.live_bytes);
        CHECK(source_fixture_compile_live==blocks && source_fixture_compile_bytes==bytes);
        source_fixture_compile_fail_at=SIZE_MAX;
        if (retry && fault!=SIZE_MAX) {
            XrCompileResources *same=context.resources;XrXirValue recovered={0};
            XrCompileResourceStats rejected=ctfe_stats(&fixture);
            CHECK(report.status==XR_XIR_CTFE_OUT_OF_MEMORY && source_fixture_compile_injected);
            CHECK(xr_xir_compile_ctfe_calls(&request,&recovered)==XR_XIR_CTFE_OK);
            CHECK(context.resources==same && recovered.type==XR_XIR_I64 && !recovered.reserved && recovered.payload==42);
            XrCompileResourceStats after=ctfe_stats(&fixture);
            CHECK(after.live_bytes==before.live_bytes);ctfe_stats_monotonic(rejected,after);
            CHECK(source_fixture_compile_live==blocks && source_fixture_compile_bytes==bytes);
        }
        ctfe_fixture_free(&fixture);
    } else CHECK(!fixture.lowered && !fixture.context.resources);
    if (retry && fault==SIZE_MAX) {
        CHECK(report.status==XR_XIR_CTFE_BUDGET);
        XrCompileResourceStats before={0},after={0};
        CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
        CtfeFixture repeated={0};XrXirValue empty={0};XrCompileResources *same=context.resources;
        XrXirStatus next=ctfe_calls_fixture_build(&context,&repeated,NULL);
        XrXirCtfeStatus status=ctfe_calls_producer_status(next);
        if (next==XR_XIR_OK) {
            XrXirCtfeCallRequest request=ctfe_calls_request(&repeated,"answer",NULL,0);
            status=xr_xir_compile_ctfe_calls(&request,&empty);ctfe_fixture_free(&repeated);
        } else CHECK(!repeated.lowered && !repeated.context.resources);
        CHECK(status==XR_XIR_CTFE_BUDGET && context.resources==same && !empty.type && !empty.reserved && !empty.payload);
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.live_bytes==before.live_bytes);ctfe_stats_monotonic(before,after);
    }
    CHECK(value.type==(uint32_t)(report.status==XR_XIR_CTFE_OK?XR_XIR_I64:XR_XIR_UNIT) && !value.reserved &&
        value.payload==(report.status==XR_XIR_CTFE_OK?42:0));
    CHECK(xr_compile_resources_stats(context.resources,&report.spent)==XR_COMPILE_RESOURCE_OK);
    CHECK(report.spent.live_bytes==baseline.live_bytes);ctfe_stats_monotonic(baseline,report.spent);
    CHECK(report.spent.allocated_bytes<=limits->allocated_bytes && report.spent.peak_bytes<=limits->live_bytes &&
        report.spent.work<=limits->work);
    xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);*output=report;
}
static void ctfe_calls_frame_census(size_t sites) {
    CtfeFixture fixture={0};ctfe_calls_fixture_new(&fixture);
    XrXirCtfeCallRequest request=ctfe_calls_request(&fixture,"answer",NULL,0);
    request.call_depth=0;size_t before=source_fixture_compile_attempts;
    ctfe_calls_expect(&fixture,&request,XR_XIR_CTFE_DEPTH_LIMIT,0);
    size_t preparation=source_fixture_compile_attempts-before;
    /* The straight Source call chain owns exactly three physical frames. No
     * private interpreter layout is copied into this test. */
    CHECK(preparation+3==sites);ctfe_fixture_free(&fixture);
    fprintf(stderr,"CTFE calls evaluator preparation=%zu activation sites=3 total=%zu\n",preparation,sites);
}
static void ctfe_calls_resources(void) {
    CtfeCallsResourceReport first={0},independent={0};
    source_fixture_compile_fail_at=SIZE_MAX;
    ctfe_calls_resource_run(&ctfe_owner_limits,SIZE_MAX,false,&first);
    ctfe_calls_resource_run(&ctfe_owner_limits,SIZE_MAX,false,&independent);
    CHECK(first.status==XR_XIR_CTFE_OK && first.sites>3);
    CHECK(independent.status==first.status && independent.sites==first.sites &&
        !memcmp(&first.spent,&independent.spent,sizeof(first.spent)));
    ctfe_calls_frame_census(first.sites);
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t less=0;less<2;++less) {
        XrCompileResourceLimits limits=ctfe_owner_limits;
        uint64_t exact=axis==0?first.spent.allocated_bytes:axis==1?first.spent.peak_bytes:first.spent.work;
        CHECK(exact>1);
        if (axis==0) limits.allocated_bytes=exact-less;
        else if (axis==1) limits.live_bytes=exact-less;
        else limits.work=exact-less;
        CtfeCallsResourceReport report={0};ctfe_calls_resource_run(&limits,SIZE_MAX,less!=0,&report);
        CHECK(report.status==(less?XR_XIR_CTFE_BUDGET:XR_XIR_CTFE_OK));
        if (!less) CHECK(report.sites==first.sites && !memcmp(&report.spent,&first.spent,sizeof(first.spent)));
        fprintf(stderr,"CTFE calls axis%u less%u cap%llu status%u physical0\n",axis,less,
            (unsigned long long)(exact-less),(unsigned)report.status);
    }
    for (size_t site=0;site<first.sites;++site) {
        source_fixture_compile_injected=false;CtfeCallsResourceReport failed={0};
        ctfe_calls_resource_run(&ctfe_owner_limits,site,true,&failed);
        CHECK(source_fixture_compile_injected && failed.status==XR_XIR_CTFE_OUT_OF_MEMORY);
        CHECK(failed.sites==site+1);
    }
    fprintf(stderr,"CTFE calls complete evaluator FI=%zu, same-owner successful retry each; whole producer/evaluator six axes; physical0\n",first.sites);
}
#endif // XIR_CTFE_CALLS_RESOURCES_H
