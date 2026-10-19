/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_deep_faults.h - Recursive retirement preserves cumulative finite owners
 */
#ifndef XIR_MIXED_DEEP_FAULTS_H
#define XIR_MIXED_DEEP_FAULTS_H
typedef struct DeepResourceRun {
    XrXirInstance *instance;
    XrXirDomain *domain;
    XrXirValue tasks[2];
    XrXirDomainBudgetStats previous;
    XrXirCallStatus fault, driven_fault;
    uintptr_t warm_stack;
    unsigned mode, round;
    bool failed, reported_fault;
} DeepResourceRun;
static bool deep_resource_status(DeepResourceRun *run, XrXirCallStatus actual, XrXirCallStatus expected) {
    if (actual == run->fault) run->reported_fault = true;
    if (actual == expected) return true;
    CHECK(actual == run->fault); run->failed = true; return false;
}
static bool deep_resource_poll(DeepResourceRun *run, bool yield, XrXirCallStatus expected) {
    XrXirCallStatus status = deep_poll(run->instance, yield);
    if (status == XR_XIR_CALL_LIMIT || status == XR_XIR_CALL_OOM) run->driven_fault = status;
    return deep_resource_status(run, status, expected);
}
static void deep_resource_cost(DeepResourceRun *run, bool grew) {
    XrXirDomainBudgetStats cost = xr_xir_domain_budget_stats(run->domain);
    CHECK(cost.work >= run->previous.work && cost.requested_bytes >= run->previous.requested_bytes &&
        cost.requested_call_bytes >= run->previous.requested_call_bytes);
    if (grew) CHECK(cost.work > run->previous.work && cost.requested_call_bytes > run->previous.requested_call_bytes);
    run->previous = cost;
}
static void deep_resource_round(DeepResourceRun *run) {
    DeepRecord *record = &deep_code->record; memset(record, 0, sizeof(*record));
    record->instance = run->instance; record->mode = run->round ? run->mode : 0;
    record->depth = record->mode ? DEEP_RECURSION : 16; record->fault = run->fault;
    record->observer = (XrXirCallObserver){record, deep_event};
    XrXirValue input = {XR_XIR_I64, 0, record->depth};
    XrXirCallStatus status = xr_xir_instance_start(run->instance, worker_deep_entry, &input, 1);
    if (!deep_resource_status(run, status, XR_XIR_CALL_READY)) return;
    XrXirCallStatus expected = record->mode < 2 ? XR_XIR_CALL_RETURNED :
        record->mode == 5 ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_CANCELLED;
    if (!deep_resource_poll(run, record->mode != 5, record->mode == 5 ? expected : XR_XIR_CALL_READY)) return;
    if (record->mode != 5) {
        CHECK(record->yields == 1);
        if (record->mode == 2) CHECK(xr_xir_instance_cancel_current(run->instance) == XR_XIR_CALL_CANCEL_REQUESTED);
        if (record->mode == 3) {
            status = xr_xir_instance_stop(run->instance);
            if (!deep_resource_status(run, status, XR_XIR_CALL_READY)) { run->driven_fault = status; return; }
        }
        if (record->mode == 4) {
            status = xr_xir_instance_free(run->instance); run->instance = NULL;
            if (!deep_resource_status(run, status, XR_XIR_CALL_READY)) return;
        } else if (!deep_resource_poll(run, false, expected)) return;
    }
    if (run->instance) {
        if (run->instance->budget.exhausted) { CHECK(run->fault == XR_XIR_CALL_LIMIT); run->failed = true; return; }
        XrXirValue value = {0};
        status = xr_xir_instance_take_result(run->instance, &value);
        XrXirCallStatus taken = record->mode < 2 ? XR_XIR_CALL_RETURNED :
            record->mode == 5 ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_BAD_STATE;
        if (!deep_resource_status(run, status, taken)) { CHECK(!value.type && !value.payload); return; }
        if (record->mode < 2) CHECK(value.type == XR_XIR_I64 && value.payload == (int64_t)record->depth+7);
        else CHECK(!value.type && !value.payload);
        xr_xir_value_drop(&value);
    }
    CHECK(!record->live && record->entered == record->left && record->walks == record->walk_leaves);
    uint32_t walks = record->mode == 5 ? DEEP_LIMIT-1 : record->depth+1;
    if (record->walks != walks) { CHECK(record->mode == 5 && run->fault == XR_XIR_CALL_LIMIT && run->reported_fault); run->failed = true; return; }
    CHECK(record->peak_depth == (record->mode == 5 ? DEEP_LIMIT : record->depth+2));
    CHECK(record->yields == (record->mode == 5 ? 0u : 1u));
    if (!run->round) run->warm_stack = record->stack_high-record->stack_low;
    else CHECK(record->stack_high-record->stack_low <= run->warm_stack+4096);
}
static void deep_resource_copy(DeepResourceRun *run, unsigned round) {
    XrXirValue *value = &run->tasks[round]; if (!value->type) return;
    CHECK(xr_xir_value_valid(value)); XirTask *task = (XirTask *)(uintptr_t)value->payload;
    CHECK(!task->executor && !task->call && atomic_load_explicit(&task->state, memory_order_acquire) == XIR_TASK_TERMINAL);
    XrXirCallStatus expected = !round || run->mode < 3 ? XR_XIR_CALL_RETURNED :
        run->mode == 5 ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_CANCELLED;
    if (!run->failed) CHECK(task->outcome.status == expected);
    else CHECK(task->outcome.status == XR_XIR_CALL_RETURNED || task->outcome.status == XR_XIR_CALL_CANCELLED ||
        task->outcome.status == run->fault || (round && run->mode == 5 && task->outcome.status == XR_XIR_CALL_LIMIT));
    for (unsigned copy = 0; copy < 2; ++copy) {
        XrXirCallResult out = {0}; XrXirCallStatus status = xr_xir_task_copy_outcome(value, &out);
        if (xr_xir_call_result_empty(&out)) {
            CHECK(status == run->fault && xr_xir_value_valid(value)); run->failed = true; run->reported_fault = true;
        } else {
            CHECK(status == task->outcome.status && out.status == status && xr_xir_panic_empty(&out.panic));
            if (status == XR_XIR_CALL_RETURNED)
                CHECK(out.value.type == XR_XIR_I64 && out.value.payload == (int64_t)(round && run->mode ? DEEP_RECURSION : 16u)+7);
            else CHECK(!out.value.type && !out.value.payload);
        }
        xr_xir_call_result_drop(&out);
    }
    xr_xir_value_drop(value);
}
static bool deep_resource_operation(XrXirProgram *program, unsigned mode,
    const XrXirInstanceConfig *limits, XrXirDomainBudgetStats *cost) {
    CHECK(mode < 6); const size_t blocks = runtime_live, bytes = runtime_bytes;
    DeepResourceRun run = {.fault=limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM, .mode=mode};
    DeepRecord *record = &deep_code->record; memset(record, 0, sizeof(*record));
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    if (limits) config = *limits;
    if (mode == 5) config.depth_limit = DEEP_LIMIT;
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &run.instance);
    if (deep_resource_status(&run, status, XR_XIR_CALL_READY)) {
        run.domain = run.instance->domain; CHECK(xr_xir_domain_retain(run.domain));
        for (run.round = 0; run.round < 2 && !run.failed; ++run.round) {
            CHECK(run.instance->domain == run.domain); deep_resource_round(&run);
            run.tasks[run.round] = record->task; record->task = (XrXirValue){0};
            deep_resource_cost(&run, !run.failed && run.round);
        }
    } else CHECK(!run.instance);
    if (run.instance) {
        if (run.instance->budget.exhausted) { CHECK(run.fault == XR_XIR_CALL_LIMIT); run.failed = true; }
        record->retiring_fault = run.failed;
        XrXirCallStatus expected = run.driven_fault != XR_XIR_CALL_READY ? run.driven_fault :
            run.instance->budget.exhausted ? run.fault : XR_XIR_CALL_READY;
        status = xr_xir_instance_free(run.instance);
        if (status != expected) fprintf(stderr, "Deep resource retirement site=%zu mode=%u round=%u actual=%u expected=%u\n",
            runtime_fail_at, mode, run.round, status, expected);
        CHECK(status == expected);
        if (status == run.fault) run.reported_fault = true;
    }
    record->instance = NULL;
    CHECK(!record->live && record->entered == record->left && record->walks == record->walk_leaves);
    for (unsigned round = 0; round < 2; ++round) deep_resource_copy(&run, round);
    if (run.domain) {
        deep_resource_cost(&run, false); XrXirDomainBudgetStats final = run.previous;
        CHECK(!final.call_live && !final.metadata_live && final.call_allocations == final.call_frees && final.metadata_allocations == final.metadata_frees);
        CHECK(xr_xir_domain_stats(run.domain).live_bytes == sizeof(*run.domain));
        if (cost) *cost = final;
        xr_xir_domain_drop(run.domain);
    }
    CHECK(!run.failed || run.reported_fault);
    CHECK(runtime_live == blocks && runtime_bytes == bytes && !worker_code_releases); return !run.failed;
}
static size_t deep_resource_axes(XrXirProgram *program, unsigned backend, unsigned mode) {
    runtime_attempts = 0; runtime_fail_at = SIZE_MAX; XrXirDomainBudgetStats cost = {0};
    CHECK(deep_resource_operation(program, mode, NULL, &cost));
    const size_t sites = runtime_attempts; CHECK(sites && sites < 20000);
    const uint64_t axes[] = {cost.requested_bytes, cost.requested_call_bytes, cost.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrXirInstanceConfig limits; CHECK(xr_xir_instance_config_init(&limits, sizeof(limits)) == XR_XIR_CALL_READY && axes[axis]);
        if (!axis) limits.requested_value_limit = axes[axis] - minus;
        else if (axis == 1) limits.requested_call_limit = axes[axis] - minus;
        else limits.work_limit = axes[axis] - minus;
        CHECK(deep_resource_operation(program, mode, &limits, NULL) == !minus);
    }
    fprintf(stderr, "Deep resources backend=%s mode=%u sites=%zu value=%llu call=%llu work=%llu, warm16 then originalDepth sameDomain exact/minus1 physical0/0\n",
        worker_backends[backend], mode, sites, (unsigned long long)axes[0], (unsigned long long)axes[1], (unsigned long long)axes[2]);
    return sites;
}
#endif // XIR_MIXED_DEEP_FAULTS_H
