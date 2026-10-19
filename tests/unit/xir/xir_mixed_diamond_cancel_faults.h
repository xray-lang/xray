/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_diamond_cancel_faults.h - Finite retirement at every original initialization prefix
 */
#ifndef XIR_MIXED_DIAMOND_CANCEL_FAULTS_H
#define XIR_MIXED_DIAMOND_CANCEL_FAULTS_H
typedef struct DiamondCancelFaultRun {
    XrXirInstance *instance;
    XrXirDomain *domain;
    DiamondCancelTrace trace, before;
    XrXirDomainBudgetStats previous;
    XrXirCallStatus fault;
    bool failed, driven, retiring;
} DiamondCancelFaultRun;
static bool diamond_cancel_fault_status(DiamondCancelFaultRun *run, XrXirCallStatus status, XrXirCallStatus expected) {
    if (status == run->fault) { run->failed = true; return false; }
    CHECK(status == expected); return true;
}
static void diamond_cancel_fault_cost(DiamondCancelFaultRun *run) {
    XrXirDomainBudgetStats current = xr_xir_domain_budget_stats(run->domain);
    CHECK(current.work >= run->previous.work && current.requested_bytes >= run->previous.requested_bytes &&
        current.requested_call_bytes >= run->previous.requested_call_bytes);
    run->previous = current;
}
static void diamond_cancel_fault_cached(DiamondCancelFaultRun *run) {
    XrXirInstance *instance = run->instance; const uint64_t epoch = instance->epoch;
    CHECK(instance->state == XR_XIR_INSTANCE_FAILED && !instance->publication_count);
    XrXirCallStatus cached = xr_xir_instance_poll_bounded(instance, 1).outcome.status;
    CHECK(cached == XR_XIR_CALL_CANCELLED || cached == run->fault);
    if (cached == run->fault) run->failed = true;
    DiamondCancelTrace before = run->trace;
    XrXirCallResult occupied = {.status=XR_XIR_CALL_RETURNED,.value={XR_XIR_I64,0,73}}, original = occupied;
    CHECK(xr_xir_instance_copy_failure(instance, &occupied) == XR_XIR_CALL_BAD_ARGUMENT && !memcmp(&occupied, &original, sizeof(original)));
    for (unsigned repeat = 0; repeat < 24; ++repeat) {
        CHECK(xr_xir_instance_start(instance, diamond_entry, NULL, 0) == cached);
        XrXirInstanceResult event = xr_xir_instance_poll_bounded(instance, 1);
        CHECK(event.outcome.status == cached && event.epoch == epoch);
        XrXirCallResult copied = {0}; XrXirCallStatus status = xr_xir_instance_copy_failure(instance, &copied);
        if (xr_xir_call_result_empty(&copied)) { CHECK(status == run->fault); run->failed = true; }
        else CHECK(status == cached && !copied.value.type && !copied.value.payload && xr_xir_panic_empty(&copied.panic));
        xr_xir_call_result_drop(&copied);
    }
    CHECK(!memcmp(&before, &run->trace, sizeof(before)));
}
static void diamond_cancel_fault_retire(DiamondCancelFaultRun *run, unsigned mode) {
    run->before = run->trace; run->retiring = true; diamond_expected_cancellation = true;
    XrXirCallStatus status;
    if (mode == 2) {
        status = xr_xir_instance_free(run->instance); run->instance = NULL;
        (void)diamond_cancel_fault_status(run, status, XR_XIR_CALL_READY); return;
    }
    if (mode == 1) {
        status = xr_xir_instance_stop(run->instance);
        if (!diamond_cancel_fault_status(run, status, XR_XIR_CALL_READY)) run->driven = true;
    } else {
        CHECK(!mode); status = xr_xir_instance_cancel_current(run->instance);
        (void)diamond_cancel_fault_status(run, status, XR_XIR_CALL_CANCEL_REQUESTED);
    }
    unsigned polls = 0;
    do { status = xr_xir_instance_poll_bounded(run->instance, 1).outcome.status; CHECK(++polls < 4096); }
    while (status == XR_XIR_CALL_READY);
    if (!diamond_cancel_fault_status(run, status, XR_XIR_CALL_CANCELLED)) run->driven = true;
    diamond_cancel_fault_cached(run);
}
static bool diamond_cancel_fault_operation(XrXirProgram *program, unsigned prefix, unsigned mode,
    const XrXirInstanceConfig *limits, XrXirDomainBudgetStats *cost) {
    CHECK(mode < 3 && !diamond_expected_cancellation);
    const size_t blocks = runtime_live, bytes = runtime_bytes;
    DiamondCancelFaultRun run = {.fault=limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM};
    diamond_expected_fault = run.fault; DiamondRecord *record = &diamond_code->records[0];
    memset(record, 0, sizeof(*record)); record->code = diamond_code;
    record->observer = (XrXirCallObserver){record, diamond_event};
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    if (limits) config = *limits;
    config.trace = diamond_cancel_trace; config.trace_context = &run.trace;
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &run.instance); record->instance = run.instance;
    if (diamond_cancel_fault_status(&run, status, XR_XIR_CALL_READY)) {
        run.domain = run.instance->domain; CHECK(xr_xir_domain_retain(run.domain));
        status = xr_xir_instance_start(run.instance, diamond_entry, NULL, 0);
        if (diamond_cancel_fault_status(&run, status, XR_XIR_CALL_READY)) {
            for (unsigned tick = 0; tick < prefix; ++tick) {
                status = xr_xir_instance_poll_bounded(run.instance, 1).outcome.status;
                if (!diamond_cancel_fault_status(&run, status, XR_XIR_CALL_READY)) { run.driven = true; break; }
            }
            if (!run.failed) {
                CHECK(xr_xir_instance_state(run.instance) == XR_XIR_INSTANCE_INITIALIZING && !record->roots);
                diamond_cancel_fault_cost(&run); diamond_cancel_fault_retire(&run, mode);
            }
        }
    } else CHECK(!run.instance);
    diamond_expected_cancellation = true;
    if (run.instance) {
        CHECK(run.instance->domain == run.domain);
        XrXirCallStatus expected = run.driven || run.instance->budget.exhausted ? run.fault : XR_XIR_CALL_READY;
        status = xr_xir_instance_free(run.instance);
        if (status != expected) fprintf(stderr, "Initializer retirement site=%zu prefix=%u mode=%u actual=%u expected=%u\n",
            runtime_fail_at, prefix, mode, status, expected);
        CHECK(status == expected); if (status == run.fault) run.failed = true;
    }
    record->instance = NULL; CHECK(!record->roots && run.trace.releases == run.trace.publications);
    for (unsigned i = 0; i < run.trace.releases; ++i) CHECK(run.trace.released[i] == run.trace.published[run.trace.publications-1-i]);
    for (unsigned i = 0; i < 4; ++i) CHECK(record->initializers[i] == record->initializer_leaves[i]);
    if (run.retiring) diamond_cancel_reverse(&run.trace, &run.before);
    if (run.domain) {
        diamond_cancel_fault_cost(&run); XrXirDomainBudgetStats final = run.previous;
        CHECK(!final.call_live && !final.metadata_live && final.call_allocations == final.call_frees && final.metadata_allocations == final.metadata_frees);
        CHECK(xr_xir_domain_stats(run.domain).live_bytes == sizeof(*run.domain));
        if (cost) *cost = final;
        xr_xir_domain_drop(run.domain);
    }
    CHECK(runtime_live == blocks && runtime_bytes == bytes && !diamond_code_releases);
    diamond_expected_fault = XR_XIR_CALL_READY; diamond_expected_cancellation = false; return !run.failed;
}
static void diamond_cancel_fault_axes(XrXirProgram *program, unsigned backend, unsigned prefixes, bool fi) {
    for (unsigned prefix = 0; prefix < prefixes; ++prefix) for (unsigned mode = 0; mode < 3; ++mode) {
        XrXirDomainBudgetStats cost = {0}; runtime_attempts = 0; runtime_fail_at = SIZE_MAX;
        CHECK(diamond_cancel_fault_operation(program, prefix, mode, NULL, &cost));
        const size_t sites = runtime_attempts; CHECK(sites && sites < 20000);
        const uint64_t axes[] = {cost.requested_bytes, cost.requested_call_bytes, cost.work};
        for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
            XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY && axes[axis]);
            if (!axis) config.requested_value_limit = axes[axis] - minus;
            else if (axis == 1) config.requested_call_limit = axes[axis] - minus;
            else config.work_limit = axes[axis] - minus;
            CHECK(diamond_cancel_fault_operation(program, prefix, mode, &config, NULL) == !minus);
        }
        if (fi) for (size_t site = 0; site < sites; ++site) {
            runtime_attempts = 0; runtime_fail_at = site;
            CHECK(!diamond_cancel_fault_operation(program, prefix, mode, NULL, NULL) && runtime_attempts > site);
        }
        runtime_fail_at = SIZE_MAX;
        fprintf(stderr, "Initializer cancel resources backend=%s prefix=%u mode=%u gate=%s sites=%zu value=%llu call=%llu work=%llu, originalDomain exact/minus1 reverseSlots closedFrames root0 physical0/0\n",
            diamond_backends[backend], prefix, mode, fi ? "fi" : "axes", sites, (unsigned long long)axes[0],
            (unsigned long long)axes[1], (unsigned long long)axes[2]);
    }
}
#endif // XIR_MIXED_DIAMOND_CANCEL_FAULTS_H
