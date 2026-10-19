/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_diamond_faults.h - Diamond initialization retains one cumulative Domain
 */
#ifndef XIR_MIXED_DIAMOND_FAULTS_H
#define XIR_MIXED_DIAMOND_FAULTS_H
static XrXirCallStatus diamond_fault_drive(XrXirInstance *instance) {
    XrXirCallStatus status; unsigned polls = 0;
    do { status = xr_xir_instance_poll_bounded(instance, 1).outcome.status; CHECK(++polls < 4096); }
    while (status == XR_XIR_CALL_READY);
    CHECK(status == XR_XIR_CALL_RETURNED || status == diamond_expected_fault); return status;
}
static bool diamond_fault_operation(XrXirProgram *program, unsigned backend,
    const XrXirInstanceConfig *limits, XrXirDomainBudgetStats *cost) {
    const size_t blocks = runtime_live, bytes = runtime_bytes;
    diamond_expected_fault = limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM;
    DiamondRecord *record = &diamond_code->records[0];
    memset(record, 0, sizeof(*record)); record->code = diamond_code;
    record->observer = (XrXirCallObserver){record, diamond_event};
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    if (limits) config = *limits;
    XrXirInstance *instance = NULL; XrXirDomain *domain = NULL;
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
    bool complete = status == XR_XIR_CALL_READY, driven = false;
    CHECK(complete || (status == diamond_expected_fault && !instance)); record->instance = instance;
    if (instance) { domain = instance->domain; CHECK(xr_xir_domain_retain(domain)); }
    XrXirValue retained[2] = {{0}}; XrXirDomainBudgetStats previous = {0};
    for (unsigned round = 0; complete && round < 2; ++round) {
        status = xr_xir_instance_start(instance, diamond_entry, NULL, 0);
        if (status == XR_XIR_CALL_READY) {
            status = diamond_fault_drive(instance); driven = status == diamond_expected_fault;
            if (status == XR_XIR_CALL_RETURNED) {
                CHECK(xr_xir_instance_take_result(instance, &retained[round]) == XR_XIR_CALL_RETURNED);
                diamond_value(&retained[round]);
            }
        }
        complete = status == XR_XIR_CALL_RETURNED; CHECK(complete || status == diamond_expected_fault);
        CHECK(instance->domain == domain); XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(domain);
        CHECK(spent.work >= previous.work && spent.requested_bytes >= previous.requested_bytes &&
            spent.requested_call_bytes >= previous.requested_call_bytes);
        if (complete && round) CHECK(spent.work > previous.work && spent.requested_call_bytes > previous.requested_call_bytes);
        previous = spent;
    }
    if (instance) {
        XrXirCallStatus expected = driven || instance->budget.exhausted ? diamond_expected_fault : XR_XIR_CALL_READY;
        status = xr_xir_instance_free(instance);
        if (status != expected) fprintf(stderr, "Diamond retirement site=%zu driven=%u actual=%u expected=%u\n",
            runtime_fail_at, (unsigned)driven, status, expected);
        CHECK(status == expected); record->instance = NULL;
    }
    if (complete) diamond_verify(record, backend);
    for (unsigned round = 0; round < 2; ++round) if (retained[round].type) {
        diamond_value(&retained[round]); xr_xir_value_drop(&retained[round]);
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
    CHECK(runtime_live == blocks && runtime_bytes == bytes && !diamond_code_releases);
    diamond_expected_fault = XR_XIR_CALL_READY; return complete;
}
static void diamond_fault_axes(XrXirProgram *program, unsigned backend, bool fi) {
    XrXirDomainBudgetStats cost = {0}; runtime_attempts = 0; runtime_fail_at = SIZE_MAX;
    CHECK(diamond_fault_operation(program, backend, NULL, &cost));
    const size_t sites = runtime_attempts; CHECK(sites && sites < 20000);
    const uint64_t axes[] = {cost.requested_bytes, cost.requested_call_bytes, cost.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY && axes[axis]);
        if (!axis) config.requested_value_limit = axes[axis] - minus;
        else if (axis == 1) config.requested_call_limit = axes[axis] - minus;
        else config.work_limit = axes[axis] - minus;
        CHECK(diamond_fault_operation(program, backend, &config, NULL) == !minus);
    }
    if (fi) for (size_t site = 0; site < sites; ++site) {
        runtime_attempts = 0; runtime_fail_at = site;
        CHECK(!diamond_fault_operation(program, backend, NULL, NULL) && runtime_attempts > site);
    }
    runtime_fail_at = SIZE_MAX;
    printf("Diamond resources backend=%s gate=%s sites=%zu value=%llu call=%llu work=%llu, sameDomain twoRounds four initializer frames once, independentSLRT, exact/minus1 physical0/0\n",
        diamond_backends[backend], fi ? "fi" : "axes", sites, (unsigned long long)axes[0],
        (unsigned long long)axes[1], (unsigned long long)axes[2]);
}
#endif // XIR_MIXED_DIAMOND_FAULTS_H
