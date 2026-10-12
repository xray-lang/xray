/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_comptime_resources.h - All real two-construction compiler refusal sites
 */
#ifndef XIR_SOURCE_COMPTIME_RESOURCES_H
#define XIR_SOURCE_COMPTIME_RESOURCES_H
typedef struct SourceComptimeTestReport {
    XrXirStatus status;
    XrCompileResourceStats baseline,spent;
    size_t sites;
    bool owner;
} SourceComptimeTestReport;
static XrXirStatus source_comptime_test_whole(const XrXirCompileContext *context) {
    XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=source_comptime_test_build(context,source_comptime_test_text,&output,&diagnostic);
    if (status==XR_XIR_OK) {
        CHECK(output.checked && output.snapshot);
        const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(output.snapshot);
        CHECK(view && view->complete && view->declaration_count && view->expression_count);
        source_comptime_test_fold(xr_xir_compile_artifact_module(output.checked),42,XR_XIR_I64);
        status=xr_xir_compile_artifact_verify(output.checked,NULL);
    } else CHECK(!output.checked && !output.snapshot);
    xr_xir_compile_source_result_free(&output);return status;
}
static void source_comptime_test_trial(const XrCompileResourceLimits *limits,size_t fault,bool retry,
    SourceComptimeTestReport *output) {
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    SourceComptimeTestReport report={0};XrXirCompileContext context={0};
    size_t start=source_fixture_compile_attempts;
    source_fixture_compile_injected=false;
    source_fixture_compile_fail_at=fault==SIZE_MAX?SIZE_MAX:start+fault;
    report.status=source_comptime_test_resource_status(xr_compile_resources_new(limits,&context.resources));
    if (report.status!=XR_XIR_OK) { CHECK(!context.resources);goto done; }
    report.owner=true;context.limits=xr_xir_compile_default_limits();report.baseline=source_comptime_test_stats(&context);
    size_t held_blocks=source_fixture_compile_live,held_bytes=source_fixture_compile_bytes;
    report.status=source_comptime_test_whole(&context);report.sites=source_fixture_compile_attempts-start;
    report.spent=source_comptime_test_stats(&context);
    CHECK(report.spent.live_bytes==report.baseline.live_bytes);
    CHECK(source_fixture_compile_live==held_blocks && source_fixture_compile_bytes==held_bytes);
    source_comptime_test_monotonic(report.baseline,report.spent);
    CHECK(report.spent.allocated_bytes<=limits->allocated_bytes && report.spent.peak_bytes<=limits->live_bytes && report.spent.work<=limits->work);
    source_fixture_compile_fail_at=SIZE_MAX;
    if (retry) {
        CHECK(report.status==XR_XIR_BUDGET || report.status==XR_XIR_OUT_OF_MEMORY);
        XrCompileResources *same_owner=context.resources;
        XrXirStatus again=source_comptime_test_whole(&context);
        if (report.status==XR_XIR_OUT_OF_MEMORY) CHECK(again==XR_XIR_OK && source_fixture_compile_injected);
        else CHECK(again==XR_XIR_BUDGET);
        CHECK(context.resources==same_owner);
        XrCompileResourceStats after=source_comptime_test_stats(&context);
        CHECK(after.live_bytes==report.baseline.live_bytes);source_comptime_test_monotonic(report.spent,after);
        CHECK(source_fixture_compile_live==held_blocks && source_fixture_compile_bytes==held_bytes);
        CHECK(after.allocated_bytes<=limits->allocated_bytes && after.peak_bytes<=limits->live_bytes && after.work<=limits->work);
    }
    xr_compile_resources_release(context.resources);
done:
    if (!report.sites) report.sites=source_fixture_compile_attempts-start;
    source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);*output=report;
}
static void source_comptime_test_resources(void) {
    SourceComptimeTestReport first={0},second={0};
    source_comptime_test_trial(&source_comptime_test_limits,SIZE_MAX,false,&first);
    source_comptime_test_trial(&source_comptime_test_limits,SIZE_MAX,false,&second);
    CHECK(first.status==XR_XIR_OK && first.owner && first.sites>1);
    CHECK(second.status==first.status && second.sites==first.sites && !memcmp(&second.spent,&first.spent,sizeof(first.spent)));
    fprintf(stderr,"Source comptime census N=%zu allocated=%llu live=%llu peak=%llu work=%llu\n",first.sites,
        (unsigned long long)first.spent.allocated_bytes,(unsigned long long)first.spent.live_bytes,
        (unsigned long long)first.spent.peak_bytes,(unsigned long long)first.spent.work);
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t less=0;less<2;++less) {
        XrCompileResourceLimits limits=source_comptime_test_limits;
        uint64_t exact=axis==0?first.spent.allocated_bytes:axis==1?first.spent.peak_bytes:first.spent.work;
        CHECK(exact>1);
        if (axis==0) limits.allocated_bytes=exact-less;
        else if (axis==1) limits.live_bytes=exact-less;
        else limits.work=exact-less;
        SourceComptimeTestReport report={0};source_comptime_test_trial(&limits,SIZE_MAX,less!=0,&report);
        if (report.status!=(less?XR_XIR_BUDGET:XR_XIR_OK)) fprintf(stderr,"Source comptime axis%u less%u cap%llu got%u\n",
            axis,less,(unsigned long long)(exact-less),(unsigned)report.status);
        CHECK(report.status==(less?XR_XIR_BUDGET:XR_XIR_OK));
        if (!less) CHECK(report.sites==first.sites && !memcmp(&report.spent,&first.spent,sizeof(first.spent)));
    }
    for (size_t site=0;site<first.sites;++site) {
        SourceComptimeTestReport report={0};source_comptime_test_trial(&source_comptime_test_limits,site,true,&report);
        if (report.status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"Source comptime FI ordinal%zu got%u sites%zu\n",site,(unsigned)report.status,report.sites);
        CHECK(report.status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && report.sites==site+1);
    }
    fprintf(stderr,"Source comptime complete FI=%zu, all physical sites including bootstrap; remaining-owner retry; six axes; physical0\n",first.sites);
}
#endif // XIR_SOURCE_COMPTIME_RESOURCES_H
