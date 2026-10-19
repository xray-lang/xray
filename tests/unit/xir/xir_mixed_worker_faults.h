/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_worker_faults.h - Real worker transactions retain cumulative parent budgets
 */
#ifndef XIR_MIXED_WORKER_FAULTS_H
#define XIR_MIXED_WORKER_FAULTS_H
static XrXirCallStatus worker_fault_drive(XrXirInstance *instance, XrXirCallStatus fault) {
    unsigned polls = 0; XrXirInstanceResult event = {0};
    do { event = xr_xir_instance_poll_bounded(instance, 1); CHECK(++polls < 32768); }
    while (event.outcome.status == XR_XIR_CALL_READY);
    CHECK(event.outcome.status == XR_XIR_CALL_RETURNED || event.outcome.status == fault);
    return event.outcome.status;
}
static bool worker_fault_operation(XrXirProgram *program,
    const WorkerControlCase *oracle, const XrXirInstanceConfig *limits, XrXirDomainBudgetStats *cost) {
    const XrXirCallStatus fault = limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM;
    const size_t blocks = runtime_live, bytes = runtime_bytes;
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    if (limits) config = *limits;
    XrXirInstance *instance = NULL; XrXirDomain *domain = NULL;
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
    bool complete = status == XR_XIR_CALL_READY, driven = false;
    CHECK(complete || (status == fault && !instance));
    XrXirValue retained[2] = {{0}}; XrXirDomainBudgetStats previous = {0};
    if (instance) { domain = instance->domain; CHECK(xr_xir_domain_retain(domain)); }
    for (unsigned round = 0; complete && round < 2; ++round) {
        status = xr_xir_instance_start(instance, *oracle->entry, NULL, 0);
        if (status == XR_XIR_CALL_READY) {
            status = worker_fault_drive(instance, fault); driven = status == fault;
            if (status == XR_XIR_CALL_RETURNED) {
                XrXirValue occupied = {XR_XIR_I64, 0, 73}, before = occupied;
                CHECK(xr_xir_instance_take_result(instance, &occupied) == XR_XIR_CALL_BAD_ARGUMENT && !memcmp(&occupied, &before, sizeof(before)));
                CHECK(xr_xir_instance_take_result(instance, &retained[round]) == XR_XIR_CALL_RETURNED);
                CHECK(retained[round].type == XR_XIR_I64 && retained[round].payload == 7);
            }
        }
        complete = status == XR_XIR_CALL_RETURNED; CHECK(complete || status == fault);
        CHECK(instance->domain == domain);
        XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(domain);
        CHECK(spent.work >= previous.work && spent.requested_bytes >= previous.requested_bytes &&
            spent.requested_call_bytes >= previous.requested_call_bytes);
        if (complete && round) CHECK(spent.work > previous.work && spent.requested_call_bytes > previous.requested_call_bytes);
        previous = spent;
    }
    XrXirCallStatus expected = driven || (instance && instance->budget.exhausted) ? fault : XR_XIR_CALL_READY;
    XrXirCallStatus freed = xr_xir_instance_free(instance);
    if (freed != expected) fprintf(stderr, "Worker retirement site=%zu driven=%u status=%u expected=%u\n",
        runtime_fail_at, (unsigned)driven, freed, expected);
    CHECK(freed == expected);
    for (unsigned round = 0; round < 2; ++round) if (retained[round].type) {
        CHECK(retained[round].type == XR_XIR_I64 && retained[round].payload == 7);
        xr_xir_value_drop(&retained[round]);
    }
    if (domain) {
        XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(domain);
        CHECK(spent.work >= previous.work && spent.requested_bytes >= previous.requested_bytes &&
            spent.requested_call_bytes >= previous.requested_call_bytes);
        CHECK(!spent.call_live && !spent.metadata_live && spent.call_allocations == spent.call_frees &&
            spent.metadata_allocations == spent.metadata_frees && xr_xir_domain_stats(domain).live_bytes == sizeof(*domain));
        if (cost) *cost = spent;
        xr_xir_domain_drop(domain);
    }
    CHECK(runtime_live == blocks && runtime_bytes == bytes && !worker_code_releases);
    return complete;
}
static void worker_axes(XrXirProgram *program, const WorkerControlCase *oracle, unsigned backend) {
    XrXirDomainBudgetStats cost = {0}; CHECK(worker_fault_operation(program, oracle, NULL, &cost));
    const uint64_t axes[] = {cost.requested_bytes, cost.requested_call_bytes, cost.work};
    for (unsigned axis = 0; axis < 3; ++axis) {
        CHECK(axes[axis]);
        for (unsigned minus = 0; minus < 2; ++minus) {
            XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
            if (!axis) config.requested_value_limit = axes[axis] - minus;
            else if (axis == 1) config.requested_call_limit = axes[axis] - minus;
            else config.work_limit = axes[axis] - minus;
            CHECK(worker_fault_operation(program, oracle, &config, NULL) == !minus);
        }
    }
    printf("Worker cumulative axes case=%s backend=%s value=%llu call=%llu work=%llu, exact/minus1 and no refund\n",
        oracle->name, worker_backends[backend], (unsigned long long)axes[0], (unsigned long long)axes[1], (unsigned long long)axes[2]);
}
static void worker_faults(XrXirProgram *program, const WorkerControlCase *oracle, unsigned backend) {
    runtime_attempts = 0; runtime_fail_at = SIZE_MAX;
    CHECK(worker_fault_operation(program, oracle, NULL, NULL));
    const size_t sites = runtime_attempts; CHECK(sites && sites < 20000);
    for (size_t site = 0; site < sites; ++site) {
        runtime_attempts = 0; runtime_fail_at = site;
        CHECK(!worker_fault_operation(program, oracle, NULL, NULL) && runtime_attempts > site);
    }
    runtime_fail_at = SIZE_MAX;
    printf("Worker complete runtime FI case=%s backend=%s sites=%zu, every point hit, typed OOM and physical0/0\n", oracle->name, worker_backends[backend], sites);
}
#endif // XIR_MIXED_WORKER_FAULTS_H
