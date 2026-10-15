/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_retained_faults.h - Pending child owners preserve finite execution ledgers
 */
#ifndef XIR_MIXED_RETAINED_FAULTS_H
#define XIR_MIXED_RETAINED_FAULTS_H
typedef struct RetainedFaultRun {
    const WorkerControlCase *oracle;
    XrXirInstance *instance;
    XrXirDomain *domain;
    XrXirValue tasks[2];
    XrXirDomainBudgetStats previous;
    XrXirCallStatus fault;
    unsigned mode, round;
    bool complete, driven;
} RetainedFaultRun;
static XrXirCallStatus retained_fault_poll(RetainedFaultRun *run, bool capture) {
    unsigned polls = 0; XrXirCallStatus status;
    do {
        status = xr_xir_instance_poll_bounded(run->instance, 1).outcome.status;
        CHECK(++polls < 32768);
    } while (status == XR_XIR_CALL_READY && (!capture || !run->tasks[run->round].type));
    return status;
}
static void retained_fault_result(RetainedFaultRun *run, XrXirCallStatus expected) {
    XrXirCallStatus status = retained_fault_poll(run, false);
    if (status == run->fault) { run->complete = false; run->driven = true; return; }
    CHECK(status == expected);
    if (status == XR_XIR_CALL_RETURNED) {
        XrXirValue value = {0}; CHECK(xr_xir_instance_take_result(run->instance, &value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && value.payload == 7); xr_xir_value_drop(&value);
    }
}
static void retained_fault_round(RetainedFaultRun *run) {
    XrXirCallStatus status = xr_xir_instance_start(run->instance, *run->oracle->entry, NULL, 0);
    if (status != XR_XIR_CALL_READY) { CHECK(status == run->fault); run->complete = false; return; }
    worker_retained_instance = run->instance; worker_retained_target = &run->tasks[run->round];
    status = retained_fault_poll(run, true);
    worker_retained_instance = NULL; worker_retained_target = NULL;
    if (status == run->fault) { run->complete = false; run->driven = true; return; }
    CHECK(status == XR_XIR_CALL_READY && run->tasks[run->round].type);
    worker_retained_expect_pending(&run->tasks[run->round]);
    unsigned mode = run->round ? run->mode : 0;
    if (mode == 1) CHECK(xr_xir_instance_cancel_current(run->instance) == XR_XIR_CALL_CANCEL_REQUESTED);
    if (mode == 2 || mode == 3) {
        status = mode == 2 ? xr_xir_instance_stop(run->instance) : xr_xir_instance_free(run->instance);
        if (mode == 3) run->instance = NULL;
        CHECK(status == XR_XIR_CALL_READY || status == run->fault);
        if (status == run->fault) { run->complete = false; run->driven = true; return; }
    }
    if (mode != 3) retained_fault_result(run, mode ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED);
}
static void retained_fault_cost(RetainedFaultRun *run) {
    XrXirDomainBudgetStats current = xr_xir_domain_budget_stats(run->domain);
    CHECK(current.work >= run->previous.work && current.requested_bytes >= run->previous.requested_bytes &&
        current.requested_call_bytes >= run->previous.requested_call_bytes);
    if (run->complete && run->round) CHECK(current.work > run->previous.work &&
        current.requested_call_bytes > run->previous.requested_call_bytes);
    run->previous = current;
}
static void retained_fault_copy(RetainedFaultRun *run, unsigned round) {
    XrXirValue *value = &run->tasks[round]; if (!value->type) return;
    CHECK(xr_xir_value_valid(value)); const XirTask *task = (const XirTask *)(uintptr_t)value->payload;
    CHECK(!task->executor && !task->call && atomic_load_explicit(&task->state, memory_order_acquire) == XIR_TASK_TERMINAL);
    XrXirCallStatus expected = round && run->mode >= 2 ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED;
    if (run->complete) CHECK(task->outcome.status == expected);
    else CHECK(task->outcome.status == XR_XIR_CALL_RETURNED || task->outcome.status == XR_XIR_CALL_CANCELLED || task->outcome.status == run->fault);
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        XrXirCallResult copied = {0}; XrXirCallStatus status = xr_xir_task_copy_outcome(value, &copied);
        if (xr_xir_call_result_empty(&copied)) {
            CHECK(status == run->fault && xr_xir_value_valid(value)); run->complete = false;
        } else {
            CHECK(status == task->outcome.status && xr_xir_panic_empty(&copied.panic));
            if (status == XR_XIR_CALL_RETURNED) CHECK(copied.value.type == XR_XIR_I64 && copied.value.payload == 7);
            else CHECK(!copied.value.type && !copied.value.payload);
        }
        xr_xir_call_result_drop(&copied);
    }
    xr_xir_value_drop(value);
}
static bool retained_fault_operation(XrXirProgram *program, const WorkerControlCase *oracle,
    unsigned mode, const XrXirInstanceConfig *limits, XrXirDomainBudgetStats *cost) {
    CHECK(mode < 4 && !worker_retained_instance && !worker_retained_target);
    const size_t blocks = runtime_live, bytes = runtime_bytes;
    RetainedFaultRun run = {.oracle=oracle, .fault=limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM, .mode=mode};
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    if (limits) config = *limits;
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &run.instance);
    run.complete = status == XR_XIR_CALL_READY;
    CHECK(run.complete || (status == run.fault && !run.instance));
    if (run.instance) { run.domain = run.instance->domain; CHECK(xr_xir_domain_retain(run.domain)); }
    for (run.round = 0; run.complete && run.round < 2; ++run.round) {
        CHECK(run.instance->domain == run.domain); retained_fault_round(&run); retained_fault_cost(&run);
    }
    if (run.instance) {
        XrXirCallStatus expected = run.driven || run.instance->budget.exhausted ? run.fault : XR_XIR_CALL_READY;
        status = xr_xir_instance_free(run.instance);
        if (status != expected) fprintf(stderr, "Retained retirement site=%zu mode=%u driven=%u actual=%u expected=%u\n",
            runtime_fail_at, mode, (unsigned)run.driven, status, expected);
        CHECK(status == expected);
    }
    for (unsigned round = 0; round < 2; ++round) retained_fault_copy(&run, round);
    if (run.domain) {
        XrXirDomainBudgetStats final = xr_xir_domain_budget_stats(run.domain);
        CHECK(final.work >= run.previous.work && final.requested_bytes >= run.previous.requested_bytes &&
            final.requested_call_bytes >= run.previous.requested_call_bytes);
        CHECK(!final.call_live && !final.metadata_live && final.call_allocations == final.call_frees &&
            final.metadata_allocations == final.metadata_frees && xr_xir_domain_stats(run.domain).live_bytes == sizeof(*run.domain));
        if (cost) *cost = final;
        xr_xir_domain_drop(run.domain);
    }
    CHECK(runtime_live == blocks && runtime_bytes == bytes && !worker_code_releases);
    return run.complete;
}
static void retained_fault_axes(XrXirProgram *program, const WorkerControlCase *oracle, unsigned backend, bool fi) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        XrXirDomainBudgetStats cost = {0}; runtime_attempts = 0; runtime_fail_at = SIZE_MAX;
        CHECK(retained_fault_operation(program, oracle, mode, NULL, &cost));
        const size_t sites = runtime_attempts; CHECK(sites && sites < 20000);
        const uint64_t axes[] = {cost.requested_bytes, cost.requested_call_bytes, cost.work};
        for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
            XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY && axes[axis]);
            if (!axis) config.requested_value_limit = axes[axis] - minus;
            else if (axis == 1) config.requested_call_limit = axes[axis] - minus;
            else config.work_limit = axes[axis] - minus;
            CHECK(retained_fault_operation(program, oracle, mode, &config, NULL) == !minus);
        }
        if (fi) for (size_t site = 0; site < sites; ++site) {
            runtime_attempts = 0; runtime_fail_at = site;
            CHECK(!retained_fault_operation(program, oracle, mode, NULL, NULL) && runtime_attempts > site);
        }
        runtime_fail_at = SIZE_MAX;
        printf("Retained resources case=%s backend=%s mode=%u gate=%s sites=%zu value=%llu call=%llu work=%llu, twoRounds sameDomain exact/minus1 latecopy physical0/0\n",
            oracle->name, worker_backends[backend], mode, fi ? "fi" : "axes", sites,
            (unsigned long long)axes[0], (unsigned long long)axes[1], (unsigned long long)axes[2]);
    }
}
#endif // XIR_MIXED_RETAINED_FAULTS_H
