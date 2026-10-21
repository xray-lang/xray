/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * task_maximum_budget_cases.h - Maximum resource controls preserve owned teardown
 */
#ifndef TASK_MAXIMUM_BUDGET_CASES_H
#define TASK_MAXIMUM_BUDGET_CASES_H

static XrXirInstanceConfig task_maximum_config(void) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    CHECK(config.metadata_limit == UINT64_MAX && config.value_limit == UINT64_MAX &&
        config.call_limit == UINT64_MAX && config.poll_limit == UINT64_MAX &&
        config.requested_value_limit == UINT64_MAX && config.requested_call_limit == UINT64_MAX &&
        config.work_limit == UINT64_MAX && config.depth_limit == UINT32_MAX);
    XrXirInstanceConfig sandbox;
    CHECK(xr_xir_instance_config_init_sandbox(&sandbox, sizeof(sandbox)) == XR_XIR_CALL_READY);
    CHECK(sandbox.metadata_limit == (UINT64_C(16) << 20) &&
        sandbox.value_limit == (UINT64_C(16) << 20) && sandbox.call_limit == (UINT64_C(16) << 20) &&
        sandbox.poll_limit == UINT64_C(1000000) && sandbox.depth_limit == 4096 &&
        sandbox.requested_value_limit == UINT64_C(67108864) &&
        sandbox.requested_call_limit == UINT64_C(67108864) && sandbox.work_limit == UINT64_C(128000000));
    return config;
}

static void task_maximum_instances(void) {
    for (uint32_t mode = 0; mode < 3; ++mode) {
        TaskFixture fixture = {0}; task_fixture_new(&fixture);
        XrXirInstanceConfig config = task_maximum_config();
        XrXirInstance *instance = NULL;
        CHECK(xr_xir_instance_new(fixture.program, &config, &instance) == XR_XIR_CALL_READY);
        if (mode) {
            XrXirValue argument = {XR_XIR_I64, 0, 21};
            CHECK(xr_xir_instance_start(instance, 1, &argument, 1) == XR_XIR_CALL_READY);
            XrXirInstanceResult result = {0};
            for (uint32_t steps = 0; steps < 64; ++steps) {
                result = xr_xir_instance_poll_bounded(instance, 1);
                if (result.outcome.status != XR_XIR_CALL_READY ||
                    (mode == 2 && fixture.witness.callbacks)) break;
            }
            if (mode == 1) {
                CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && result.outcome.value.payload == 42);
            } else {
                CHECK(result.outcome.status == XR_XIR_CALL_READY && fixture.witness.callbacks == 1);
                CHECK(instance->accounting[instance->accounting_index].depth >= 2 &&
                    instance->budget.release_tickets - instance->budget.released_frames >= 2);
                CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_CANCEL_REQUESTED);
                CHECK(xr_xir_instance_close_activation(instance) == XR_XIR_CALL_READY);
                CHECK(instance->budget.release_tickets == instance->budget.released_frames &&
                    !instance->budget.live_bytes);
            }
        }
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
        CHECK(fixture.witness.releases == (mode ? 1u : 0u));
        task_fixture_drop(&fixture);
        CHECK(fixture.witness.code_releases == 1 && !runtime_live && !runtime_bytes);
    }
    puts("Maximum default Instance controls preserve empty teardown, result42 and nested root cancellation physical0");
}

static void task_maximum_executor(void) {
    for (uint32_t exhaust = 0; exhaust < 2; ++exhaust) {
        TaskFixture fixture = {0}; task_fixture_new(&fixture);
        XrXirInstanceConfig defaults = task_maximum_config();
        XrXirDomain *domain = NULL;
        CHECK(xr_xir_domain_new(defaults.value_limit, &domain) == XR_XIR_VALUE_OK);
        XrXirTaskExecutorConfig config = task_config(&fixture, domain);
        config.call_limit = defaults.call_limit; config.poll_limit = defaults.poll_limit;
        config.requested_value_limit = defaults.requested_value_limit;
        config.requested_call_limit = defaults.requested_call_limit;
        config.work_limit = defaults.work_limit; config.depth_limit = defaults.depth_limit;
        XrXirTaskExecutor *executor = NULL;
        CHECK(task_test_executor_new(&config, &executor) == XR_XIR_CALL_READY);
        XrXirValue argument = {XR_XIR_I64, 0, 21}, handles[2] = {{0}, {0}};
        XrXirCallRequest request = {.entry = 1, .arguments = &argument, .count = 1};
        for (uint32_t i = 0; i < 2; ++i)
            CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &request, &handles[i]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY);
        CHECK(fixture.witness.callbacks == 1 && fixture.budget.release_tickets == 2 &&
            !fixture.budget.released_frames);
        if (exhaust) {
            XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(domain);
            CHECK(before.bound && before.work < UINT64_MAX - 1);
            size_t attempts = runtime_attempts;
            CHECK(xr_xir_domain_work(domain, UINT64_MAX - 1 - before.work));
            CHECK(xr_xir_domain_budget_stats(domain).work == UINT64_MAX - 1);
            CHECK(xr_xir_domain_work(domain, 1));
            CHECK(xr_xir_domain_budget_stats(domain).work == UINT64_MAX);
            CHECK(!xr_xir_domain_work(domain, 1));
            CHECK(xr_xir_domain_budget_stats(domain).work == UINT64_MAX && runtime_attempts == attempts);
        }
        CHECK(xr_xir_task_executor_stop(executor) == XR_XIR_CALL_CANCEL_REQUESTED);
        XrXirCallBudget accounting = {0};
        CHECK(xr_xir_task_executor_free(executor, &accounting) ==
            (exhaust ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_READY));
        CHECK(accounting.release_tickets == 2 && accounting.released_frames == 2 &&
            !accounting.live_bytes && accounting.allocations == accounting.frees && fixture.witness.releases == 2);
        XrXirDomainBudgetStats drained = xr_xir_domain_budget_stats(domain);
        CHECK(!drained.call_live && !drained.metadata_live && (!exhaust || drained.work == UINT64_MAX));
        for (uint32_t i = 0; i < 2; ++i) {
            const XirTask *task = (const XirTask *)(uintptr_t)handles[i].payload;
            CHECK(task->state == XIR_TASK_TERMINAL && !task->call && !task->executor &&
                task->outcome.status == (exhaust ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_CANCELLED));
            xr_xir_value_drop(&handles[i]);
        }
        xr_xir_domain_drop(domain); task_fixture_drop(&fixture);
        CHECK(fixture.witness.code_releases == 1 && !runtime_live && !runtime_bytes);
    }
    puts("Maximum Task controls reject work beyond UINT64_MAX without wrap and retain prepaid drain physical0");
}

static void task_maximum_budget_cases(void) {
    task_maximum_instances();
    task_maximum_executor();
}

#endif /* TASK_MAXIMUM_BUDGET_CASES_H */
