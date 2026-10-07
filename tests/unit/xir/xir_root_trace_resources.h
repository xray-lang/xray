/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_trace_resources.h - Exhaustive allocation failures and exact three-axis limits
 */
#ifndef XIR_ROOT_TRACE_RESOURCES_H
#define XIR_ROOT_TRACE_RESOURCES_H
static void trace_oom_function(uint32_t function) {
    TraceMark mark=trace_mark(); XrXirCompileContext context=trace_owner(trace_caps());
    uint64_t baseline=trace_stats(&context).live_bytes; XrXirEffects *effects=trace_summary(&context);
    XrXirRootCauseTrace *output=NULL; trace_attempts=0;
    CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,function,&output)==XR_XIR_OK && output);
    size_t sites=trace_attempts; CHECK(sites==1); trace_oracle(output,function);
    xr_xir_compile_root_cause_trace_free(output); xr_xir_compile_effects_free(effects);
    trace_owner_free(&context,baseline); trace_balanced(mark);
    for(size_t point=0;point<sites;++point) {
        context=trace_owner(trace_caps()); baseline=trace_stats(&context).live_bytes; effects=trace_summary(&context);
        XrCompileResourceStats before=trace_stats(&context); TraceMark retained=trace_mark();
        output=NULL; trace_attempts=0; trace_fail_at=point; trace_injected=false;
        CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,function,&output)==XR_XIR_OUT_OF_MEMORY);
        CHECK(!output && trace_injected && trace_attempts==point+1); trace_balanced(retained);
        XrCompileResourceStats paid=trace_stats(&context);
        uint64_t steps=(uint64_t)trace_expected[function].root_count+trace_expected[function].unknown_count;
        CHECK(paid.work==before.work+steps+2 && paid.allocated_bytes==before.allocated_bytes &&
            paid.live_bytes==before.live_bytes && paid.allocation_count==before.allocation_count);
        trace_fail_at=SIZE_MAX;
        CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,function,&output)==XR_XIR_OK && output);
        trace_oracle(output,function); XrCompileResourceStats retry=trace_stats(&context);
        uint64_t bytes=sizeof(XrXirRootCauseTrace)+steps*sizeof(XrXirRootCauseStep);
        CHECK(retry.work==paid.work+bytes+2*steps+3 && retry.allocated_bytes>paid.allocated_bytes);
        xr_xir_compile_root_cause_trace_free(output); xr_xir_compile_effects_free(effects);
        trace_owner_free(&context,baseline); trace_balanced(mark);
    }
}
static void trace_oom_sites(void) {
    trace_oom_function(3); trace_oom_function(8); trace_oom_function(11);
}
/* Retained real storage makes the trace establish the peak itself, so the
 * live-minus-one cut reaches this constructor rather than an earlier producer. */
static void *trace_ballast(const XrXirCompileContext *context) {
    XrCompileResourceStats before=trace_stats(context); void *memory=NULL;
    XrCompileResourceLimits bound=trace_caps(); CHECK(before.peak_bytes<=bound.live_bytes);
    CHECK(xr_compile_resources_alloc(context->resources,(size_t)before.peak_bytes,&memory)==XR_COMPILE_RESOURCE_OK);
    CHECK(memory && trace_stats(context).live_bytes>before.peak_bytes); return memory;
}
static XrCompileResourceStats trace_budget_probe(uint32_t function, XrCompileResourceLimits caps, bool exact) {
    TraceMark mark=trace_mark(); XrXirCompileContext context=trace_owner(caps);
    uint64_t baseline=trace_stats(&context).live_bytes; XrXirEffects *effects=trace_summary(&context);
    void *ballast=trace_ballast(&context); XrXirRootCauseTrace *output=NULL;
    XrCompileResourceStats before=trace_stats(&context); TraceMark retained=trace_mark(); trace_attempts=0;
    XrXirStatus status=xr_xir_compile_root_cause_trace_copy(&context,effects,function,&output);
    CHECK(status==(exact?XR_XIR_OK:XR_XIR_BUDGET)); CHECK(exact?output!=NULL:output==NULL);
    XrCompileResourceStats measured=trace_stats(&context);
    if(exact) {
        trace_oracle(output,function); CHECK(measured.peak_bytes==measured.live_bytes && measured.peak_bytes>before.peak_bytes);
        xr_xir_compile_root_cause_trace_free(output);
    } else {
        CHECK(!trace_attempts && measured.allocated_bytes==before.allocated_bytes);
        trace_balanced(retained); output=(XrXirRootCauseTrace *)(uintptr_t)1;
        CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,function,&output)==XR_XIR_BAD_STRUCTURE);
        CHECK(output==(XrXirRootCauseTrace *)(uintptr_t)1);
        trace_stats_equal(trace_stats(&context),measured); output=NULL;
        CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,function,&output)==XR_XIR_BUDGET && !output);
        CHECK(trace_stats(&context).work>=measured.work && trace_stats(&context).allocated_bytes==measured.allocated_bytes);
        trace_balanced(retained);
    }
    xr_compile_resources_free(ballast); xr_xir_compile_effects_free(effects);
    trace_owner_free(&context,baseline); trace_balanced(mark); return measured;
}
static void trace_budget_boundaries(void) {
    const uint32_t functions[]={3,8,11};
    for(unsigned f=0;f<3;++f) {
        XrCompileResourceStats measured=trace_budget_probe(functions[f],trace_caps(),true);
        fprintf(stderr,"trace f%u exact: allocated=%llu peak=%llu work=%llu\n",functions[f],
            (unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,
            (unsigned long long)measured.work);
        for(unsigned axis=0;axis<3;++axis) {
            XrCompileResourceLimits caps=trace_caps();
            uint64_t *limit=axis==0?&caps.allocated_bytes:axis==1?&caps.live_bytes:&caps.work;
            *limit=axis==0?measured.allocated_bytes:axis==1?measured.peak_bytes:measured.work;
            trace_budget_probe(functions[f],caps,true); CHECK(*limit>1); --*limit;
            trace_budget_probe(functions[f],caps,false);
        }
    }
}
static void trace_post_admission_cuts(void) {
    /* One entry charge plus four validations precede four record writes and
     * the final header. Every partially initialized allocation is released. */
    for(size_t point=5;point<10;++point) {
        TraceMark mark=trace_mark(); XrXirCompileContext context=trace_owner(trace_caps());
        uint64_t baseline=trace_stats(&context).live_bytes; XrXirEffects *effects=trace_summary(&context);
        XrCompileResourceStats before=trace_stats(&context); TraceMark retained=trace_mark();
        XrXirRootCauseTrace *output=NULL; trace_work_position=0; trace_work_cut_at=point;
        trace_competing_charge=true; trace_attempts=0;
        CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,8,&output)==XR_XIR_BUDGET);
        CHECK(!output && !trace_competing_charge && trace_attempts==1); trace_balanced(retained);
        XrCompileResourceStats after=trace_stats(&context); XrCompileResourceLimits caps=trace_caps();
        CHECK(after.work==caps.work && after.allocated_bytes>before.allocated_bytes && after.live_bytes==before.live_bytes);
        output=(XrXirRootCauseTrace *)(uintptr_t)1;
        CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,8,&output)==XR_XIR_BAD_STRUCTURE);
        CHECK(output==(XrXirRootCauseTrace *)(uintptr_t)1); trace_stats_equal(trace_stats(&context),after);
        output=NULL;
        CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,8,&output)==XR_XIR_BUDGET && !output);
        trace_stats_equal(trace_stats(&context),after); trace_balanced(retained);
        xr_xir_compile_effects_free(effects); trace_owner_free(&context,baseline); trace_balanced(mark);
    }
}
#endif // XIR_ROOT_TRACE_RESOURCES_H
