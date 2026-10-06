/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_go_source_runtime_cases.h - Exact runtime faults and one surviving budget
 */
#ifndef XIR_GO_SOURCE_RUNTIME_CASES_H
#define XIR_GO_SOURCE_RUNTIME_CASES_H

static XrXirCallStatus source_task_runtime_operation(XrXirProgram *program, uint32_t entry,
    const SourceTaskOracle *oracle, const XrXirInstanceConfig *config, XrXirDomainBudgetStats *measured) {
    XrXirInstance *instance = NULL; XrXirDomain *domain = NULL; XrXirCallResult owned = {0};
    XrXirCallStatus status = xr_xir_instance_new(program, config, &instance);
    if (status != XR_XIR_CALL_READY) { CHECK(!instance); return status; }
    domain = instance->domain;
    CHECK(xr_xir_domain_retain(domain));
    CHECK(xr_xir_domain_budget_stats(domain).bound);
    status = xr_xir_instance_start(instance, entry, NULL, 0);
    if (status == XR_XIR_CALL_READY) {
        XrXirInstanceResult result = {0}; unsigned transitions = 0;
        do { result = xr_xir_instance_poll_bounded(instance, 1); CHECK(++transitions < 32768); }
        while (result.outcome.status == XR_XIR_CALL_READY);
        status = result.outcome.status;
        if (status == oracle->status) {
            source_task_assert(oracle, &result.outcome);
            XrXirValueStatus copied = xr_xir_call_result_copy(&result.outcome, &owned);
            if (copied != XR_XIR_VALUE_OK) {
                CHECK(xr_xir_call_result_empty(&owned));
                source_task_assert(oracle, &result.outcome);
                status = task_core_value_status(copied);
            }
        }
    }
    XrXirCallStatus freed = xr_xir_instance_free(instance);
    CHECK(freed == XR_XIR_CALL_READY || freed == XR_XIR_CALL_LIMIT || freed == XR_XIR_CALL_OOM);
    if (freed != XR_XIR_CALL_READY && status == oracle->status) status = freed;
    if (!xr_xir_call_result_empty(&owned)) source_task_assert(oracle, &owned);
    *measured = xr_xir_domain_budget_stats(domain);
    CHECK(measured->bound && !measured->metadata_live && !measured->call_live);
    CHECK(measured->metadata_allocations == measured->metadata_frees &&
        measured->call_allocations == measured->call_frees);
    xr_xir_call_result_drop(&owned);
    XrXirDomainStats physical = xr_xir_domain_stats(domain);
    CHECK(physical.live_bytes == sizeof(XrXirDomain) && physical.allocations == physical.frees + 1);
    xr_xir_domain_drop(domain);
    return status;
}
static void source_task_runtime_faults(const SourceTaskOracle *oracle) {
    uint32_t entry = UINT32_MAX; XrXirProgram *program = source_task_program(oracle, &entry);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    size_t live = runtime_live, bytes = runtime_bytes;
    XrXirDomainBudgetStats measured = {0};
    runtime_attempts = 0;
    CHECK(source_task_runtime_operation(program, entry, oracle, &config, &measured) == oracle->status);
    size_t sites = runtime_attempts;
    CHECK(sites && sites < 4096 && runtime_live == live && runtime_bytes == bytes);
    for (size_t ordinal = 0; ordinal < sites; ++ordinal) {
        runtime_attempts = 0; runtime_fail_at = ordinal; measured = (XrXirDomainBudgetStats){0};
        XrXirCallStatus status = source_task_runtime_operation(program, entry, oracle, &config, &measured);
        runtime_fail_at = SIZE_MAX;
        if (status != XR_XIR_CALL_OOM) fprintf(stderr, "%s runtime OOM ordinal=%zu status=%u attempts=%zu\n",
            oracle->name, ordinal, status, runtime_attempts);
        CHECK(status == XR_XIR_CALL_OOM && runtime_attempts > ordinal);
        CHECK(runtime_live == live && runtime_bytes == bytes);
        printf("Source Task runtime OOM %s ordinal=%zu/%zu actual attempts=%zu physical baseline exact\n",
            oracle->name, ordinal, sites, runtime_attempts);
    }
    xr_xir_compile_program_drop(program); CHECK(!runtime_live && !runtime_bytes);
    printf("Source Task runtime OOM %s exact unique ordinals=%zu physical=0/0\n", oracle->name, sites);
}
static void source_task_runtime_axes(const SourceTaskOracle *oracle) {
    uint32_t entry = UINT32_MAX; XrXirProgram *program = source_task_program(oracle, &entry);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    CHECK(config.requested_value_limit == UINT64_C(67108864) &&
        config.requested_call_limit == UINT64_C(67108864) && config.work_limit == UINT64_C(128000000));
    XrXirDomainBudgetStats original = {0};
    CHECK(source_task_runtime_operation(program, entry, oracle, &config, &original) == oracle->status);
    uint64_t costs[] = {original.requested_bytes, original.requested_call_bytes, original.work};
    CHECK(costs[0] && costs[1] && costs[2]);
    size_t live = runtime_live, bytes = runtime_bytes;
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrXirInstanceConfig limited = config; XrXirDomainBudgetStats measured = {0};
        uint64_t *limit = axis == 0 ? &limited.requested_value_limit : axis == 1 ?
            &limited.requested_call_limit : &limited.work_limit;
        *limit = costs[axis] - minus;
        XrXirCallStatus status = source_task_runtime_operation(program, entry, oracle, &limited, &measured);
        if (status != (minus ? XR_XIR_CALL_LIMIT : oracle->status)) fprintf(stderr,
            "%s cumulative axis=%u minus=%u status=%u costs=%llu/%llu/%llu\n", oracle->name, axis, minus, status,
            (unsigned long long)costs[0], (unsigned long long)costs[1], (unsigned long long)costs[2]);
        CHECK(status == (minus ? XR_XIR_CALL_LIMIT : oracle->status));
        if (!minus) CHECK(measured.requested_bytes == costs[0] && measured.requested_call_bytes == costs[1] && measured.work == costs[2]);
        CHECK(runtime_live == live && runtime_bytes == bytes);
        printf("Source Task cumulative %s axis=%u minus=%u status=%u physical baseline exact\n", oracle->name, axis, minus, status);
    }
    xr_xir_compile_program_drop(program); CHECK(!runtime_live && !runtime_bytes);
    printf("Source Task cumulative %s exact/minus1 value/call/work=%llu/%llu/%llu physical=0/0\n", oracle->name,
        (unsigned long long)costs[0], (unsigned long long)costs[1], (unsigned long long)costs[2]);
}
#endif // XIR_GO_SOURCE_RUNTIME_CASES_H
