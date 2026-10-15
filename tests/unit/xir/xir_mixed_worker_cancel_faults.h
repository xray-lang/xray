/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_worker_cancel_faults.h - Finite retirement at each real worker prefix
 */
#ifndef XIR_MIXED_WORKER_CANCEL_FAULTS_H
#define XIR_MIXED_WORKER_CANCEL_FAULTS_H
typedef struct WorkerCancelFaultRun {
    const WorkerControlCase *oracle;
    XrXirInstance *instance;
    XrXirDomain *domain;
    XrXirDomainBudgetStats previous;
    XrXirValue results[2];
    XrXirCallStatus fault;
    bool failed, driven;
} WorkerCancelFaultRun;
static bool worker_cancel_fault_status(WorkerCancelFaultRun *run, XrXirCallStatus status, XrXirCallStatus expected) {
    if (status == run->fault) { run->failed = true; return false; }
    CHECK(status == expected); return true;
}
static void worker_cancel_fault_cost(WorkerCancelFaultRun *run, bool grew) {
    XrXirDomainBudgetStats current = xr_xir_domain_budget_stats(run->domain);
    CHECK(current.work >= run->previous.work && current.requested_bytes >= run->previous.requested_bytes &&
        current.requested_call_bytes >= run->previous.requested_call_bytes);
    if (grew) CHECK(current.work > run->previous.work && current.requested_call_bytes > run->previous.requested_call_bytes);
    run->previous = current;
}
static bool worker_cancel_fault_finish(WorkerCancelFaultRun *run, XrXirCallStatus expected, XrXirValue *output) {
    unsigned polls = 0; XrXirCallStatus status; XrXirInstanceResult event = {0};
    do { event = xr_xir_instance_poll_bounded(run->instance, 1); status = event.outcome.status; CHECK(++polls < 32768); }
    while (status == XR_XIR_CALL_READY);
    if (!worker_cancel_fault_status(run, status, expected)) { run->driven = true; return false; }
    if (expected == XR_XIR_CALL_RETURNED) {
        CHECK(output && xr_xir_instance_take_result(run->instance, output) == XR_XIR_CALL_RETURNED);
        CHECK(output->type == XR_XIR_I64 && output->payload == 7);
    } else {
        XrXirValue empty = {0}; CHECK(expected == XR_XIR_CALL_CANCELLED);
        CHECK(!event.outcome.value.type && !event.outcome.value.payload && xr_xir_panic_empty(&event.outcome.panic));
        CHECK(xr_xir_instance_take_result(run->instance, &empty) == XR_XIR_CALL_BAD_STATE && !empty.type && !empty.payload);
    }
    return true;
}
static bool worker_cancel_fault_start(WorkerCancelFaultRun *run) {
    return worker_cancel_fault_status(run,
        xr_xir_instance_start(run->instance, *run->oracle->entry, NULL, 0), XR_XIR_CALL_READY);
}
static void worker_cancel_fault_retire(WorkerCancelFaultRun *run, unsigned mode, uint64_t epoch) {
    XrXirCallStatus status;
    if (mode == 2) {
        status = xr_xir_instance_free(run->instance); run->instance = NULL;
        (void)worker_cancel_fault_status(run, status, XR_XIR_CALL_READY); return;
    }
    if (mode == 1) {
        status = xr_xir_instance_stop(run->instance);
        if (!worker_cancel_fault_status(run, status, XR_XIR_CALL_READY)) { run->driven = true; return; }
    } else {
        CHECK(!mode);
        CHECK(xr_xir_instance_cancel_current(run->instance) == XR_XIR_CALL_CANCEL_REQUESTED);
    }
    if (!worker_cancel_fault_finish(run, XR_XIR_CALL_CANCELLED, NULL)) return;
    CHECK(xr_xir_instance_resume(run->instance, epoch, 1) == XR_XIR_CALL_BAD_STATE);
    worker_cancel_fault_cost(run, true);
    if (mode == 1) {
        CHECK(xr_xir_instance_start(run->instance, *run->oracle->entry, NULL, 0) == XR_XIR_CALL_BAD_STATE); return;
    }
    if (!worker_cancel_fault_start(run)) return;
    CHECK(run->instance->epoch > epoch && xr_xir_instance_resume(run->instance, epoch, 1) == XR_XIR_CALL_BAD_STATE);
    if (worker_cancel_fault_finish(run, XR_XIR_CALL_RETURNED, &run->results[1])) worker_cancel_fault_cost(run, true);
}
static bool worker_cancel_fault_operation(XrXirProgram *program, const WorkerControlCase *oracle,
    unsigned prefix, unsigned mode, const XrXirInstanceConfig *limits, XrXirDomainBudgetStats *cost) {
    CHECK(oracle && mode < 3);
    const size_t blocks = runtime_live, bytes = runtime_bytes;
    WorkerCancelFaultRun run = {.oracle=oracle, .fault=limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM};
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    if (limits) config = *limits;
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &run.instance);
    if (worker_cancel_fault_status(&run, status, XR_XIR_CALL_READY)) {
        run.domain = run.instance->domain; CHECK(xr_xir_domain_retain(run.domain));
        if (worker_cancel_fault_start(&run) && worker_cancel_fault_finish(&run, XR_XIR_CALL_RETURNED, &run.results[0])) {
            worker_cancel_fault_cost(&run, true);
            if (worker_cancel_fault_start(&run)) {
                for (unsigned tick = 0; tick < prefix; ++tick) {
                    status = xr_xir_instance_poll_bounded(run.instance, 1).outcome.status;
                    if (!worker_cancel_fault_status(&run, status, XR_XIR_CALL_READY)) { run.driven = true; break; }
                }
                if (!run.failed) worker_cancel_fault_retire(&run, mode, run.instance->epoch);
            }
        }
    } else CHECK(!run.instance);
    if (run.instance) {
        CHECK(run.instance->domain == run.domain);
        XrXirCallStatus expected = run.driven || run.instance->budget.exhausted ? run.fault : XR_XIR_CALL_READY;
        status = xr_xir_instance_free(run.instance);
        if (status != expected) fprintf(stderr, "Worker cancel retirement site=%zu prefix=%u mode=%u actual=%u expected=%u\n",
            runtime_fail_at, prefix, mode, status, expected);
        CHECK(status == expected); if (status == run.fault) run.failed = true;
    }
    for (unsigned i = 0; i < 2; ++i) if (run.results[i].type) {
        CHECK(run.results[i].type == XR_XIR_I64 && run.results[i].payload == 7); xr_xir_value_drop(&run.results[i]);
    }
    if (run.domain) {
        worker_cancel_fault_cost(&run, false); XrXirDomainBudgetStats final = run.previous;
        CHECK(!final.call_live && !final.metadata_live && final.call_allocations == final.call_frees && final.metadata_allocations == final.metadata_frees);
        CHECK(xr_xir_domain_stats(run.domain).live_bytes == sizeof(*run.domain));
        if (cost) *cost = final;
        xr_xir_domain_drop(run.domain);
    }
    CHECK(runtime_live == blocks && runtime_bytes == bytes && !worker_code_releases); return !run.failed;
}
static void worker_cancel_fault_axes(XrXirProgram *program, const WorkerControlCase *oracle,
    unsigned backend, unsigned prefixes, bool fi) {
    for (unsigned prefix = 0; prefix < prefixes; ++prefix) for (unsigned mode = 0; mode < 3; ++mode) {
        XrXirDomainBudgetStats cost = {0}; runtime_attempts = 0; runtime_fail_at = SIZE_MAX;
        CHECK(worker_cancel_fault_operation(program, oracle, prefix, mode, NULL, &cost));
        const size_t sites = runtime_attempts; CHECK(sites && sites < 20000);
        const uint64_t axes[] = {cost.requested_bytes, cost.requested_call_bytes, cost.work};
        for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
            XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY && axes[axis]);
            if (!axis) config.requested_value_limit = axes[axis] - minus;
            else if (axis == 1) config.requested_call_limit = axes[axis] - minus;
            else config.work_limit = axes[axis] - minus;
            CHECK(worker_cancel_fault_operation(program, oracle, prefix, mode, &config, NULL) == !minus);
        }
        if (fi) for (size_t site = 0; site < sites; ++site) {
            runtime_attempts = 0; runtime_fail_at = site;
            CHECK(!worker_cancel_fault_operation(program, oracle, prefix, mode, NULL, NULL) && runtime_attempts > site);
        }
        runtime_fail_at = SIZE_MAX;
        fprintf(stderr, "Worker cancel resources case=%s backend=%s prefix=%u mode=%u gate=%s sites=%zu value=%llu call=%llu work=%llu, originalDomain warm then cancel/stop/free restartOnlyCancel physical0/0\n",
            oracle->name, worker_backends[backend], prefix, mode, fi ? "fi" : "axes", sites,
            (unsigned long long)axes[0], (unsigned long long)axes[1], (unsigned long long)axes[2]);
    }
}
#endif // XIR_MIXED_WORKER_CANCEL_FAULTS_H
