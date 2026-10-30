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
    XrXirStatus status=source_comptime_test_build(context,source_comptime_resource_text,&output,&diagnostic);
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
    CHECK(report.spent.held_bytes==report.baseline.held_bytes);
    CHECK(source_fixture_compile_live==held_blocks && source_fixture_compile_bytes==held_bytes);
    source_comptime_test_monotonic(report.baseline,report.spent);
    CHECK(report.spent.peak_bytes<=limits->memory_limit);
    source_fixture_compile_fail_at=SIZE_MAX;
    if (retry) {
        CHECK(report.status==XR_XIR_MEMORY_LIMIT || report.status==XR_XIR_OUT_OF_MEMORY);
        XrCompileResources *same_owner=context.resources;
        XrXirStatus again=source_comptime_test_whole(&context);
        if (report.status==XR_XIR_OUT_OF_MEMORY) CHECK(again==XR_XIR_OK && source_fixture_compile_injected);
        else CHECK(again==XR_XIR_MEMORY_LIMIT);
        CHECK(context.resources==same_owner);
        XrCompileResourceStats after=source_comptime_test_stats(&context);
        CHECK(after.held_bytes==report.baseline.held_bytes);source_comptime_test_monotonic(report.spent,after);
        CHECK(source_fixture_compile_live==held_blocks && source_fixture_compile_bytes==held_bytes);
        CHECK(after.peak_bytes<=limits->memory_limit);
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
    for (unsigned less=0;less<2;++less) {
        XrCompileResourceLimits limits=source_comptime_test_limits;
        CHECK(first.spent.peak_bytes>1);limits.memory_limit=first.spent.peak_bytes-less;
        SourceComptimeTestReport report={0};source_comptime_test_trial(&limits,SIZE_MAX,less!=0,&report);
        CHECK(report.status==(less?XR_XIR_MEMORY_LIMIT:XR_XIR_OK));
    }
    size_t selected[3]={0,first.sites/2,first.sites-1};
    for (size_t trial=0;trial<3;++trial) {
        size_t site=selected[trial];SourceComptimeTestReport report={0};
        source_comptime_test_trial(&source_comptime_test_limits,site,true,&report);
        CHECK(report.status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && report.sites==site+1);
    }
    fprintf(stderr,"Source comptime representative physicalOOM, same-owner recovery and held exact/minus-one PASS\n");
}
#endif // XIR_SOURCE_COMPTIME_RESOURCES_H
