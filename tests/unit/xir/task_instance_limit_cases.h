/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * task_instance_limit_cases.h - Instance resource outcomes never transfer values
 */
#ifndef TASK_INSTANCE_LIMIT_CASES_H
#define TASK_INSTANCE_LIMIT_CASES_H

static XrXirInstanceResult task_instance_result_terminal(XrXirInstance *instance) {
    XrXirInstanceResult result = {0};
    for (uint32_t step = 0; step < 64; ++step) {
        result = xr_xir_instance_poll_bounded(instance, 1);
        if (result.outcome.status != XR_XIR_CALL_READY) return result;
    }
    CHECK(false);
    return result;
}

static void task_instance_limit_destinations(XrXirInstance *instance) {
    const XrXirValue occupied[] = {{XR_XIR_I64, 0, 17}, {XR_XIR_UNIT, 1, 0}, {XR_XIR_UNIT, 0, 1}};
    for (uint32_t index = 0; index < 3; ++index) {
        XrXirValue output = occupied[index];
        CHECK(xr_xir_instance_take_result(instance, &output) == XR_XIR_CALL_BAD_ARGUMENT &&
            !memcmp(&output, &occupied[index], sizeof(output)));
    }
    CHECK(xr_xir_instance_take_result(instance, NULL) == XR_XIR_CALL_BAD_ARGUMENT);
    const XrXirValue empty = {0};
    for (uint32_t repeat = 0; repeat < 2; ++repeat) {
        XrXirValue output = {0};
        XrXirCallStatus taken = xr_xir_instance_take_result(instance, &output);
        fprintf(stderr, "Instance limit take=%u\n", (unsigned) taken);
        CHECK(taken == XR_XIR_CALL_LIMIT && !memcmp(&output, &empty, sizeof(output)));
    }
}

static void task_instance_result_limit(void) {
    TaskFixture fixture = {0}; task_fixture_new(&fixture);
    XrXirInstanceConfig config = task_maximum_config();
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(fixture.program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirValue argument = {XR_XIR_I64, 0, 21};
    CHECK(xr_xir_instance_start(instance, 1, &argument, 1) == XR_XIR_CALL_READY);
    XrXirInstanceResult result = task_instance_result_terminal(instance);
    CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && result.outcome.value.payload == 42);
    XrXirValue returned = {0};
    CHECK(xr_xir_instance_take_result(instance, &returned) == XR_XIR_CALL_RETURNED && returned.payload == 42);
    xr_xir_value_drop(&returned);
    CHECK(xr_xir_instance_take_result(instance, &returned) == XR_XIR_CALL_BAD_STATE);
    uint64_t original_limit = instance->domain->limit;
    instance->domain->limit = instance->domain->stats.live_bytes;
    CHECK(xr_xir_instance_start(instance, 4, NULL, 0) == XR_XIR_CALL_READY);
    result = task_instance_result_terminal(instance);
    CHECK(result.outcome.status == XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_resume(instance, result.epoch, result.outcome.wake) == XR_XIR_CALL_READY);
    result = task_instance_result_terminal(instance);
    instance->domain->limit = original_limit;
    CHECK(result.outcome.status == XR_XIR_CALL_LIMIT && xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    task_instance_limit_destinations(instance);
    XrXirValue absent = {0};
    CHECK(xr_xir_call_take_result(instance->call, &absent) == XR_XIR_CALL_BAD_STATE && !absent.payload);
    CHECK(xr_xir_instance_take_result(instance, &absent) == XR_XIR_CALL_LIMIT);
    CHECK(xr_xir_instance_start(instance, 1, &argument, 1) == XR_XIR_CALL_READY);
    CHECK(task_instance_result_terminal(instance).outcome.status == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance, &absent) == XR_XIR_CALL_RETURNED && absent.payload == 42);
    xr_xir_value_drop(&absent);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    task_fixture_drop(&fixture);
    CHECK(fixture.witness.releases == 3 && fixture.witness.code_releases == 1 && !runtime_live && !runtime_bytes);
    puts("Instance root LIMIT is repeatable, preserves destinations and releases physical owners");
}

static void task_instance_group_limit(void) {
    TaskFixture fixture = {0}; task_fixture_new(&fixture);
    XrXirInstanceConfig config = task_maximum_config();
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(fixture.program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirValue argument = {XR_XIR_I64, 0, 21};
    CHECK(xr_xir_instance_start(instance, 1, &argument, 1) == XR_XIR_CALL_READY);
    CHECK(task_instance_result_terminal(instance).outcome.status == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_call_state(instance->call) == XR_XIR_CALL_RETURNED);
    /* Replace the executor's retained Task-type lease with the host fixture's. */
    CHECK(xr_xir_compile_type_arena_retain(fixture.arena));
    xr_xir_compile_type_arena_drop(instance->executor->config.task_arena);
    instance->executor->config.task_arena = fixture.arena;
    instance->executor->config.poll_limit = 1;
    XrXirCallRequest request = {.entry = 1, .arguments = &argument, .count = 1};
    XrXirValue child = {0}, output = {0};
    XrXirCallStatus spawned = xr_xir_task_executor_spawn(instance->executor, (XrXirType)256, &request, &child);
    if (spawned != XR_XIR_CALL_READY) fprintf(stderr, "Instance group child spawn=%u\n", (unsigned) spawned);
    CHECK(spawned == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_take_result(instance, &output) == XR_XIR_CALL_BAD_STATE);
    XrXirInstanceResult result = task_instance_result_terminal(instance);
    CHECK(result.outcome.status == XR_XIR_CALL_LIMIT && xr_xir_call_state(instance->call) == XR_XIR_CALL_RETURNED);
    task_instance_limit_destinations(instance);
    CHECK(xr_xir_call_state(instance->call) == XR_XIR_CALL_RETURNED);
    xr_xir_value_drop(&child);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_LIMIT);
    task_fixture_drop(&fixture);
    CHECK(fixture.witness.releases == 2 && fixture.witness.code_releases == 1 && !runtime_live && !runtime_bytes);
    puts("Instance group LIMIT blocks the successful root value and preserves destination validation");
}

static void task_instance_initialization_limit(void) {
    TaskFixture fixture = {0}; task_fixture_new(&fixture);
    XrXirInstanceConfig config = task_maximum_config(); config.poll_limit = 2;
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(fixture.program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirValue argument = {XR_XIR_I64, 0, 21};
    CHECK(xr_xir_instance_start(instance, 1, &argument, 1) == XR_XIR_CALL_READY);
    CHECK(task_instance_result_terminal(instance).outcome.status == XR_XIR_CALL_LIMIT &&
        xr_xir_instance_state(instance) == XR_XIR_INSTANCE_FAILED && !fixture.witness.callbacks);
    XrXirValue output = {XR_XIR_I64, 0, 17}, saved = output;
    CHECK(xr_xir_instance_take_result(instance, &output) == XR_XIR_CALL_BAD_STATE &&
        !memcmp(&output, &saved, sizeof(output)));
    XrXirCallResult failure = {0};
    CHECK(xr_xir_instance_copy_failure(instance, &failure) == XR_XIR_CALL_LIMIT);
    xr_xir_call_result_drop(&failure);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_LIMIT);
    task_fixture_drop(&fixture);
    CHECK(fixture.witness.code_releases == 1 && !runtime_live && !runtime_bytes);
    puts("Initialization LIMIT remains instance-owned and does not transfer through ordinary results");
}

static void task_instance_limit_result_cases(void) {
    task_instance_initialization_limit();
    task_instance_result_limit();
    task_instance_group_limit();
}

#endif // TASK_INSTANCE_LIMIT_CASES_H
