/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_nominal_expression_cases.h - Independent typed instance expectations
 */
#ifndef XIR_NOMINAL_EXPRESSION_CASES_H
#define XIR_NOMINAL_EXPRESSION_CASES_H
static XrXirOutputStatus nominal_expression_output(void *context, const XrXirOutputGroup *group) {
    unsigned *calls = context; ++*calls;
    CHECK(group->stream == XR_XIR_STDOUT && group->line && group->count == 3);
    CHECK(group->values[0].type == XR_XIR_I64 && group->values[0].payload == 7);
    CHECK(group->values[1].type == XR_XIR_U8 && group->values[1].payload == 9);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&group->values[2], &bytes, &length) && length == 7 && !memcmp(bytes,"generic",7));
    return XR_XIR_OUTPUT_OK;
}
static void nominal_expression_failure_attempt(XrXirProgram *program) {
    XrXirInstance *instance = NULL; XrXirValue result = {0}; unsigned outputs = 0;
    bool driven_oom = false; uint64_t epoch = 0; XrXirCall *call = NULL;
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, nominal_expression_output, &outputs};
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
    if (status == XR_XIR_CALL_READY) status = xr_xir_instance_start(instance, 0, NULL, 0);
    if (status == XR_XIR_CALL_READY) {
        epoch = instance->epoch; call = instance->call;
        status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
        driven_oom = status == XR_XIR_CALL_OOM;
    }
    if (status == XR_XIR_CALL_RETURNED) {
        CHECK(outputs == 1);
        CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
        CHECK(result.type == XR_XIR_I64 && result.payload == 7);
        xr_xir_value_drop(&result);
        status = xr_xir_instance_start(instance, 2, NULL, 0);
        if (status == XR_XIR_CALL_READY) {
            epoch = instance->epoch; call = instance->call;
            status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
            driven_oom = status == XR_XIR_CALL_OOM;
        }
        if (status == XR_XIR_CALL_RETURNED)
            CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
    }
    CHECK(status == (runtime_fail_at == SIZE_MAX ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_OOM));
    if (instance) {
        CHECK(xr_xir_task_executor_root_idle(instance->executor));
        if (instance->call) {
            const XrXirExecutorBinding binding = {instance->executor,instance->call->executor_activation,
                instance->call->executor_generation,instance->call->executor_ticket};
            XrXirCallStatus failure = XR_XIR_CALL_BAD_STATE;
            CHECK(xr_xir_call_driver_failure(instance->call,&binding,&failure) &&
                failure == (driven_oom ? XR_XIR_CALL_OOM : XR_XIR_CALL_READY));
        }
        CHECK(xr_xir_task_executor_completion_status(instance->executor) ==
            (driven_oom ? XR_XIR_CALL_OOM : XR_XIR_CALL_READY));
        if (driven_oom) CHECK(instance->epoch == epoch && instance->call == call && epoch);
    } else CHECK(!driven_oom);
    CHECK(xr_xir_instance_free(instance) == (driven_oom ? XR_XIR_CALL_OOM : XR_XIR_CALL_READY));
    if (status == XR_XIR_CALL_RETURNED) {
        const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(&result, &bytes, &length) && length == 7 && !memcmp(bytes,"generic",7));
    } else CHECK(!result.type && !result.payload);
    xr_xir_value_drop(&result);
}
static void nominal_expression_failures(XrXirProgram *program) {
    size_t baseline = runtime_live, bytes = runtime_bytes, sites = 0;
    for (size_t attempt = 0; attempt <= sites; ++attempt) {
        runtime_attempts = 0; runtime_fail_at = attempt ? attempt - 1 : SIZE_MAX;
        nominal_expression_failure_attempt(program);
        if (!attempt) sites = runtime_attempts;
        CHECK(runtime_live == baseline && runtime_bytes == bytes);
    }
    runtime_fail_at = SIZE_MAX;
    CHECK(sites > 0);
    printf("Generic nominal runtime physical release: %zu allocation failure sites\n", sites);
}
static void nominal_expression_cases(XrXirProgram *program) {
    nominal_expression_failures(program);
    XrXirInstance *instances[2] = {0}; unsigned calls = 0;
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, nominal_expression_output, &calls};
    for (unsigned i = 0; i < 2; ++i)
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    XrXirValue escaped[2] = {{0}};
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instances[i], 0, NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instances[i], UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        XrXirValue value = {0};
        CHECK(xr_xir_instance_take_result(instances[i], &value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && value.payload == 7 && calls == i + 1);
        xr_xir_value_drop(&value);
        CHECK(xr_xir_instance_start(instances[i], 2, NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instances[i], UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instances[i], &escaped[i]) == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
    for (unsigned i = 0; i < 2; ++i) {
        const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(&escaped[i], &bytes, &length) && length == 7 && !memcmp(bytes,"generic",7));
        xr_xir_value_drop(&escaped[i]);
    }
}
#endif // XIR_NOMINAL_EXPRESSION_CASES_H
