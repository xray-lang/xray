/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_leaf_resources.h - Complete same-ledger scalar compiler resource gates
 *
 * KEY CONCEPT:
 *   Independent producer censuses bound fresh exact/minus-one owners; every
 *   actual allocation failure preserves outputs, fees and physical ownership.
 */
#ifndef XIR_CTFE_LEAF_RESOURCES_H
#define XIR_CTFE_LEAF_RESOURCES_H
typedef struct CtfeWholeStatus {
    XrXirStatus producer, normalized;
    XrXirCtfeStatus evaluator;
    bool evaluator_entered;
} CtfeWholeStatus;
typedef struct CtfeWholeReport {
    CtfeWholeStatus status;
    CtfeChainTrace trace;
    XrCompileResourceStats baseline, spent;
    XrXirValue value;
    size_t attempts;
    bool published_owner;
} CtfeWholeReport;
static XrXirStatus ctfe_resource_status(XrCompileResourceStatus status) {
    if (status == XR_COMPILE_RESOURCE_OK) return XR_XIR_OK;
    if (status == XR_COMPILE_RESOURCE_BUDGET) return XR_XIR_BUDGET;
    if (status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY) return XR_XIR_OUT_OF_MEMORY;
    return XR_XIR_BAD_STRUCTURE;
}
static void ctfe_stats_monotonic(XrCompileResourceStats before, XrCompileResourceStats after) {
    CHECK(after.allocation_count >= before.allocation_count);
    CHECK(after.allocated_bytes >= before.allocated_bytes && after.work >= before.work);
    CHECK(after.peak_bytes >= before.peak_bytes);
}
static void ctfe_value41(const XrXirValue *value) {
    CHECK(value->type == XR_XIR_I64 && !value->reserved && value->payload == 41);
}
static CtfeWholeStatus ctfe_whole(const XrXirCompileContext *context,
    XrXirValue *output, CtfeChainTrace *trace) {
    CtfeWholeStatus result = {XR_XIR_BAD_STRUCTURE, XR_XIR_BAD_STRUCTURE, XR_XIR_CTFE_BAD_ARGUMENT, false};
    if (!context || !context->resources || !output || output->type || output->reserved || output->payload)
        return result;
    CtfeFixture fixture = {0};
    result.producer = ctfe_fixture_build(context, &fixture, trace);
    result.normalized = result.producer;
    if (result.producer != XR_XIR_OK) { CHECK(!fixture.context.resources && !fixture.lowered); return result; }
    CHECK(fixture.context.resources == context->resources);
    CHECK(xr_xir_compile_artifact_context(fixture.lowered)->resources == context->resources);
    XrXirCtfeRequest request = ctfe_request(&fixture, "answer", NULL, 0);
    ctfe_chain_mark(trace, CTFE_CHAIN_EVALUATE); result.evaluator_entered = true;
    result.evaluator = xr_xir_compile_ctfe_leaf(&request, output);
    if (result.evaluator == XR_XIR_CTFE_OK) result.normalized = XR_XIR_OK;
    else if (result.evaluator == XR_XIR_CTFE_BUDGET) result.normalized = XR_XIR_BUDGET;
    else if (result.evaluator == XR_XIR_CTFE_OUT_OF_MEMORY) result.normalized = XR_XIR_OUT_OF_MEMORY;
    else result.normalized = XR_XIR_BAD_STRUCTURE;
    ctfe_chain_mark(trace, CTFE_CHAIN_CLEANUP); ctfe_fixture_free(&fixture);
    return result;
}
static void ctfe_whole_run(const XrCompileResourceLimits *limits, bool retry,
    CtfeWholeReport *output) {
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    CtfeWholeReport report = {0}; XrXirCompileContext context = {0};
    size_t start = source_fixture_compile_attempts;
    ctfe_chain_mark(&report.trace, CTFE_CHAIN_LEDGER);
    report.status.producer = ctfe_resource_status(xr_compile_resources_new(limits, &context.resources));
    report.status.normalized = report.status.producer; report.status.evaluator = XR_XIR_CTFE_BAD_ARGUMENT;
    if (report.status.producer != XR_XIR_OK) { CHECK(!context.resources); goto done; }
    report.published_owner = true; context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_stats(context.resources, &report.baseline) == XR_COMPILE_RESOURCE_OK);
    size_t live = source_fixture_compile_live, bytes = source_fixture_compile_bytes;
    report.status = ctfe_whole(&context, &report.value, &report.trace);
    CHECK(source_fixture_compile_live == live && source_fixture_compile_bytes == bytes);
    CHECK(xr_compile_resources_stats(context.resources, &report.spent) == XR_COMPILE_RESOURCE_OK);
    CHECK(report.spent.live_bytes == report.baseline.live_bytes);
    ctfe_stats_monotonic(report.baseline, report.spent);
    CHECK(report.spent.allocated_bytes <= limits->allocated_bytes && report.spent.peak_bytes <= limits->live_bytes &&
        report.spent.work <= limits->work);
    ctfe_chain_trace_end(&report.trace); report.attempts = source_fixture_compile_attempts - start;
    if (report.status.normalized != XR_XIR_OK && report.status.normalized != XR_XIR_BUDGET &&
        report.status.normalized != XR_XIR_OUT_OF_MEMORY)
        fprintf(stderr, "CTFE whole unexpected producer%u entered%u evaluator%u phases%u\n",
            (unsigned)report.status.producer, report.status.evaluator_entered ? 1u : 0u,
            (unsigned)report.status.evaluator, report.trace.visited);
    if (report.status.normalized != XR_XIR_OK) {
        XrXirValue empty = {0}; CHECK(!memcmp(&report.value, &empty, sizeof(empty)));
        CHECK(report.spent.work > report.baseline.work);
    }
    if (retry) {
        source_fixture_compile_fail_at = SIZE_MAX;
        XrXirValue again = {0}; XrCompileResources *same_owner = context.resources;
        CtfeWholeStatus repeated = ctfe_whole(&context, &again, NULL);
        CHECK(context.resources == same_owner);
        XrCompileResourceStats after = {0};
        CHECK(xr_compile_resources_stats(context.resources, &after) == XR_COMPILE_RESOURCE_OK);
        CHECK(after.live_bytes == report.baseline.live_bytes);
        ctfe_stats_monotonic(report.spent, after);
        CHECK(source_fixture_compile_live == live && source_fixture_compile_bytes == bytes);
        if (report.status.normalized == XR_XIR_OUT_OF_MEMORY) {
            CHECK(repeated.normalized == XR_XIR_OK && repeated.evaluator_entered && repeated.evaluator == XR_XIR_CTFE_OK);
            ctfe_value41(&again);
        } else {
            CHECK(report.status.normalized == XR_XIR_BUDGET && repeated.normalized == XR_XIR_BUDGET);
            XrXirValue empty = {0}; CHECK(!memcmp(&again, &empty, sizeof(empty)));
        }
    }
    xr_compile_resources_release(context.resources);
done:
    ctfe_chain_trace_end(&report.trace);
    if (!report.attempts) report.attempts = source_fixture_compile_attempts - start;
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    *output = report;
}
static void ctfe_census(const CtfeWholeReport *report) {
    CHECK(report->published_owner && report->status.producer == XR_XIR_OK);
    CHECK(report->status.normalized == XR_XIR_OK && report->status.evaluator_entered && report->status.evaluator == XR_XIR_CTFE_OK);
    ctfe_value41(&report->value);
    CHECK(report->trace.visited == (UINT32_C(1) << CTFE_CHAIN_PHASE_COUNT)-1);
    size_t sites = 0;
    for (uint32_t phase = 0; phase < CTFE_CHAIN_PHASE_COUNT; ++phase) sites += report->trace.allocations[phase];
    CHECK(sites == report->attempts && report->trace.allocations[CTFE_CHAIN_LEDGER] == 1);
    CHECK(report->trace.allocations[CTFE_CHAIN_SOURCE] && report->trace.allocations[CTFE_CHAIN_READ1] &&
        report->trace.allocations[CTFE_CHAIN_READ2] && report->trace.allocations[CTFE_CHAIN_EVALUATE]);
}
static void ctfe_whole_occupied(void) {
    XrXirCompileContext context = {0};
    CHECK(xr_compile_resources_new(&ctfe_owner_limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits();
    XrCompileResourceStats baseline = {0};
    CHECK(xr_compile_resources_stats(context.resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    CtfeFixture fixture = {0}; CHECK(ctfe_fixture_build(&context, &fixture, NULL) == XR_XIR_OK);
    unsigned char original[sizeof(fixture)]; memcpy(original, &fixture, sizeof(original));
    XrCompileResourceStats before = ctfe_stats(&fixture);
    size_t attempts = source_fixture_compile_attempts;
    CHECK(ctfe_fixture_build(&context, &fixture, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(!memcmp(&fixture, original, sizeof(original)));
    XrXirValue occupied = {XR_XIR_I64,77,99}, value_copy = occupied;
    CtfeWholeStatus denied = ctfe_whole(&context, &occupied, NULL);
    CHECK(denied.normalized == XR_XIR_BAD_STRUCTURE && denied.evaluator == XR_XIR_CTFE_BAD_ARGUMENT && !denied.evaluator_entered);
    CHECK(!memcmp(&occupied, &value_copy, sizeof(value_copy)));
    XrCompileResourceStats after = ctfe_stats(&fixture);
    CHECK(!memcmp(&before, &after, sizeof(before)) && attempts == source_fixture_compile_attempts);
    XrXirValue answer = {0}; XrXirCtfeRequest request = ctfe_request(&fixture, "answer", NULL, 0);
    CHECK(xr_xir_compile_ctfe_leaf(&request, &answer) == XR_XIR_CTFE_OK); ctfe_value41(&answer);
    ctfe_fixture_free(&fixture);
    CHECK(xr_compile_resources_stats(context.resources, &after) == XR_COMPILE_RESOURCE_OK && after.live_bytes == baseline.live_bytes);
    ctfe_stats_monotonic(before, after); xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes); ctfe_value41(&answer);
}
static void ctfe_whole_resources(void) {
    CtfeWholeReport first = {0}, independent = {0};
    source_fixture_compile_fail_at = SIZE_MAX;
    ctfe_whole_run(&ctfe_owner_limits, false, &first); ctfe_census(&first);
    ctfe_whole_run(&ctfe_owner_limits, false, &independent); ctfe_census(&independent);
    CHECK(!memcmp(&first.spent, &independent.spent, sizeof(first.spent)));
    CHECK(first.attempts == independent.attempts && !memcmp(first.trace.allocations, independent.trace.allocations, sizeof(first.trace.allocations)));
    for (uint32_t axis = 0; axis < 3; ++axis) for (uint32_t less = 0; less < 2; ++less) {
        XrCompileResourceLimits limits = ctfe_owner_limits;
        uint64_t exact = axis == 0 ? first.spent.allocated_bytes : axis == 1 ? first.spent.peak_bytes : first.spent.work;
        CHECK(exact > 1);
        if (axis == 0) limits.allocated_bytes = exact-less;
        else if (axis == 1) limits.live_bytes = exact-less;
        else limits.work = exact-less;
        CtfeWholeReport report = {0}; ctfe_whole_run(&limits, less != 0, &report);
        if (report.status.normalized != (less ? XR_XIR_BUDGET : XR_XIR_OK))
            fprintf(stderr, "CTFE whole axis%u less%u exact%llu producer%u evaluator%u\n", axis, less,
                (unsigned long long)exact, (unsigned)report.status.producer, (unsigned)report.status.evaluator);
        CHECK(report.status.normalized == (less ? XR_XIR_BUDGET : XR_XIR_OK));
        if (!less) { ctfe_census(&report); CHECK(!memcmp(&report.spent, &first.spent, sizeof(first.spent))); }
        else if (axis != 1) CHECK(report.status.producer == XR_XIR_OK && report.status.evaluator_entered && report.status.evaluator == XR_XIR_CTFE_BUDGET);
        fprintf(stderr, "CTFE whole axis=%u less=%u cap=%llu producer=%u entered=%u evaluator=%u allocated=%llu peak=%llu work=%llu; same-owner fees retained; finalphysical=0/0\n",
            axis, less, (unsigned long long)(exact-less), (unsigned)report.status.producer,
            report.status.evaluator_entered ? 1u : 0u, (unsigned)report.status.evaluator,
            (unsigned long long)report.spent.allocated_bytes, (unsigned long long)report.spent.peak_bytes,
            (unsigned long long)report.spent.work);
    }
    size_t injected = 0;
    for (size_t site = 0; site < first.attempts; ++site) {
        source_fixture_compile_injected = false;
        source_fixture_compile_fail_at = source_fixture_compile_attempts + site;
        CtfeWholeReport report = {0}; ctfe_whole_run(&ctfe_owner_limits, true, &report);
        if (report.status.normalized != XR_XIR_OUT_OF_MEMORY)
            fprintf(stderr, "CTFE whole FI%zu producer%u evaluator%u owner%u\n", site,
                (unsigned)report.status.producer, (unsigned)report.status.evaluator, report.published_owner ? 1u : 0u);
        CHECK(source_fixture_compile_injected && report.status.normalized == XR_XIR_OUT_OF_MEMORY);
        CHECK(site ? report.published_owner : !report.published_owner);
        size_t prefix = 0; uint32_t phase = 0;
        while (phase < CTFE_CHAIN_PHASE_COUNT && site >= prefix + first.trace.allocations[phase])
            prefix += first.trace.allocations[phase++];
        CHECK(phase < CTFE_CHAIN_PHASE_COUNT && report.trace.allocations[phase] >= site-prefix+1);
        for (uint32_t prior = 0; prior < phase; ++prior)
            CHECK(report.trace.allocations[prior] == first.trace.allocations[prior]);
        fprintf(stderr, "CTFE whole FI site=%zu phase=%s producer=%u entered=%u evaluator=%u owner=%u allocated=%llu live=%llu work=%llu recovery=%s; output preserved; finalphysical=0/0\n",
            site, ctfe_chain_names[phase], (unsigned)report.status.producer,
            report.status.evaluator_entered ? 1u : 0u, (unsigned)report.status.evaluator,
            report.published_owner ? 1u : 0u, (unsigned long long)report.spent.allocated_bytes,
            (unsigned long long)report.spent.live_bytes, (unsigned long long)report.spent.work,
            report.published_owner ? "same-owner41" : "unpublished-ledger");
        source_fixture_compile_fail_at = SIZE_MAX; ++injected;
    }
    CHECK(injected == first.attempts);
    ctfe_whole_occupied();
    fprintf(stderr, "CTFE complete same-owner census allocations=%zu allocated=%llu peak=%llu work=%llu; all FI=%zu; three exact/minus-one axes; occupied unchanged; physical=0/0\n",
        first.attempts, (unsigned long long)first.spent.allocated_bytes, (unsigned long long)first.spent.peak_bytes,
        (unsigned long long)first.spent.work, injected);
    for (uint32_t phase = 0; phase < CTFE_CHAIN_PHASE_COUNT; ++phase)
        fprintf(stderr, "CTFE complete census phase=%s allocations=%zu\n", ctfe_chain_names[phase], first.trace.allocations[phase]);
}
#endif // XIR_CTFE_LEAF_RESOURCES_H
