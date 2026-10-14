/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_library_faults.h - Private Library state transactions keep one finite Domain
 */
#ifndef XIR_MIXED_LIBRARY_FAULTS_H
#define XIR_MIXED_LIBRARY_FAULTS_H
static XrXirCallStatus counter_fault_drive(XrXirInstance *instance, XrXirCallStatus fault) {
    unsigned polls = 0, yields = 0; XrXirInstanceResult event = {0};
    do {
        event = xr_xir_instance_poll_bounded(instance, 1); CHECK(++polls < 4096);
        if (event.outcome.status == XR_XIR_CALL_SUSPENDED) {
            CHECK(event.epoch && event.outcome.wake && !yields); ++yields;
            CHECK(xr_xir_instance_resume(instance, event.epoch, event.outcome.wake) == XR_XIR_CALL_READY);
            event.outcome.status = XR_XIR_CALL_READY;
        }
    } while (event.outcome.status == XR_XIR_CALL_READY);
    CHECK(event.outcome.status == XR_XIR_CALL_RETURNED || event.outcome.status == fault);
    if (event.outcome.status == XR_XIR_CALL_RETURNED) CHECK(yields == 1);
    return event.outcome.status;
}
static bool counter_fault_operation(XrXirProgram *program,
    const XrXirInstanceConfig *limits, XrXirDomainBudgetStats *cost) {
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
        status = xr_xir_instance_start(instance, library_counter_entry, NULL, 0);
        if (status == XR_XIR_CALL_READY) {
            status = counter_fault_drive(instance, fault); driven = status == fault;
            if (status == XR_XIR_CALL_RETURNED) {
                XrXirValue occupied = {XR_XIR_I64, 0, 73}, before = occupied;
                CHECK(xr_xir_instance_take_result(instance, &occupied) == XR_XIR_CALL_BAD_ARGUMENT && !memcmp(&occupied, &before, sizeof(before)));
                CHECK(xr_xir_instance_take_result(instance, &retained[round]) == XR_XIR_CALL_RETURNED);
                CHECK(retained[round].type == XR_XIR_I64 && retained[round].payload == round + 1);
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
    if (freed != expected) fprintf(stderr, "Library retirement site=%zu driven=%u status=%u expected=%u\n",
        runtime_fail_at, (unsigned)driven, freed, expected);
    CHECK(freed == expected);
    for (unsigned round = 0; round < 2; ++round) if (retained[round].type) {
        CHECK(retained[round].type == XR_XIR_I64 && retained[round].payload == round + 1);
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
    CHECK(runtime_live == blocks && runtime_bytes == bytes && !counter_code_releases);
    return complete;
}
static void counter_axes(XrXirProgram *program, unsigned backend) {
    XrXirDomainBudgetStats cost = {0}; CHECK(counter_fault_operation(program, NULL, &cost));
    const uint64_t axes[] = {cost.requested_bytes, cost.requested_call_bytes, cost.work};
    for (unsigned axis = 0; axis < 3; ++axis) {
        CHECK(axes[axis]);
        for (unsigned minus = 0; minus < 2; ++minus) {
            XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
            if (!axis) config.requested_value_limit = axes[axis] - minus;
            else if (axis == 1) config.requested_call_limit = axes[axis] - minus;
            else config.work_limit = axes[axis] - minus;
            CHECK(counter_fault_operation(program, &config, NULL) == !minus);
        }
    }
    printf("Library cumulative axes backend=%s value=%llu call=%llu work=%llu, exact/minus1 and no refund\n",
        counter_backends[backend], (unsigned long long)axes[0], (unsigned long long)axes[1], (unsigned long long)axes[2]);
}
static void counter_faults(XrXirProgram *program, unsigned backend) {
    runtime_attempts = 0; runtime_fail_at = SIZE_MAX;
    CHECK(counter_fault_operation(program, NULL, NULL));
    const size_t sites = runtime_attempts; CHECK(sites && sites < 20000);
    for (size_t site = 0; site < sites; ++site) {
        runtime_attempts = 0; runtime_fail_at = site;
        CHECK(!counter_fault_operation(program, NULL, NULL) && runtime_attempts > site);
    }
    runtime_fail_at = SIZE_MAX;
    printf("Library complete runtime FI backend=%s sites=%zu, every point hit, typed OOM and physical0/0\n", counter_backends[backend], sites);
}
#endif // XIR_MIXED_LIBRARY_FAULTS_H
