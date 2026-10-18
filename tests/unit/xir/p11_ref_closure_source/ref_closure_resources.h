/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * ref_closure_resources.h - Complete producer and receiving failure prefixes
 */
#ifndef REF_CLOSURE_RESOURCES_H
#define REF_CLOSURE_RESOURCES_H
typedef struct RefClosureReport { XrXirStatus status;XrCompileResourceStats spent;size_t sites;bool owner; } RefClosureReport;
static XrXirStatus resource_status(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static void monotonic(XrCompileResourceStats before,XrCompileResourceStats after) {
    CHECK(after.work>=before.work && after.allocated_bytes>=before.allocated_bytes && after.allocation_count>=before.allocation_count);
    CHECK(after.peak_bytes>=before.peak_bytes);
}
static RefClosureReport trial(const XrCompileResourceLimits *limits,size_t fault,bool retry) {
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);RefClosureReport report={0};XrXirCompileContext context={0};
    size_t start=source_fixture_compile_attempts;source_fixture_compile_injected=false;
    source_fixture_compile_fail_at=fault==SIZE_MAX?SIZE_MAX:start+fault;
    report.status=resource_status(xr_compile_resources_new(limits,&context.resources));
    if (report.status!=XR_XIR_OK) { CHECK(!context.resources);goto done; }
    context.limits=xr_xir_compile_default_limits();report.owner=true;
    XrCompileResourceStats baseline=stats(&context);size_t held=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    report.status=operation(&context,combined,2,1,false);report.spent=stats(&context);
    CHECK(report.spent.live_bytes==baseline.live_bytes && source_fixture_compile_live==held && source_fixture_compile_bytes==bytes);
    monotonic(baseline,report.spent);report.sites=source_fixture_compile_attempts-start;
    CHECK(report.spent.work<=limits->work && report.spent.allocated_bytes<=limits->allocated_bytes && report.spent.peak_bytes<=limits->live_bytes);
    source_fixture_compile_fail_at=SIZE_MAX;
    if (retry) {
        CHECK(report.status==XR_XIR_OUT_OF_MEMORY || report.status==XR_XIR_BUDGET);
        XrCompileResources *same=context.resources;XrXirStatus again=operation(&context,combined,2,1,false);
        CHECK(again==(report.status==XR_XIR_OUT_OF_MEMORY?XR_XIR_OK:XR_XIR_BUDGET) && context.resources==same);
        XrCompileResourceStats after=stats(&context);monotonic(report.spent,after);
        CHECK(after.live_bytes==baseline.live_bytes && source_fixture_compile_live==held && source_fixture_compile_bytes==bytes);
        CHECK(after.work<=limits->work && after.allocated_bytes<=limits->allocated_bytes && after.peak_bytes<=limits->live_bytes);
    }
    xr_compile_resources_release(context.resources);
done:
    if (!report.sites) report.sites=source_fixture_compile_attempts-start;
    source_fixture_compile_fail_at=SIZE_MAX;CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);return report;
}
static void resources(bool inject) {
    RefClosureReport first=trial(&caps,SIZE_MAX,false),second=trial(&caps,SIZE_MAX,false);
    CHECK(first.status==XR_XIR_OK && first.owner && first.sites>1);
    CHECK(second.status==XR_XIR_OK && second.sites==first.sites && !memcmp(&first.spent,&second.spent,sizeof(first.spent)));
    fprintf(stderr,"Source ref closure whole pipeline census=%zu allocated=%llu peak=%llu work=%llu\n",first.sites,
        (unsigned long long)first.spent.allocated_bytes,(unsigned long long)first.spent.peak_bytes,(unsigned long long)first.spent.work);
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t less=0;less<2;++less) {
        XrCompileResourceLimits limits=caps;
        uint64_t exact=axis==0?first.spent.allocated_bytes:axis==1?first.spent.peak_bytes:first.spent.work;CHECK(exact>1);
        if (axis==0) limits.allocated_bytes=exact-less;
        else if (axis==1) limits.live_bytes=exact-less;
        else limits.work=exact-less;
        RefClosureReport result=trial(&limits,SIZE_MAX,less!=0);CHECK(result.status==(less?XR_XIR_BUDGET:XR_XIR_OK));
        if (!less) CHECK(result.sites==first.sites && !memcmp(&result.spent,&first.spent,sizeof(first.spent)));
    }
    if (!inject) { fprintf(stderr,"Source ref closure allocated/live/work exact-minus-one and same-owner budget refusals physical0; full FI separately registered\n");return; }
    for (size_t fault=0;fault<first.sites;++fault) {
        RefClosureReport result=trial(&caps,fault,true);
        if (result.status!=XR_XIR_OUT_OF_MEMORY || result.sites!=fault+1)
            fprintf(stderr,"Source ref closure FI ordinal%zu status%u actualprefix%zu\n",fault,(unsigned)result.status,result.sites);
        CHECK(result.status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && result.sites==fault+1);
        if (!(fault%512) || fault+1==first.sites) fprintf(stderr,"Source ref closure full FI=%zu/%zu same-owner physical0\n",fault+1,first.sites);
    }
    fprintf(stderr,"Source ref closure full physical FI=%zu,strict failure prefix,original-owner retry,allocated/live/work exact-minus-one,producer and packet death physical0\n",first.sites);
}
#endif // REF_CLOSURE_RESOURCES_H
