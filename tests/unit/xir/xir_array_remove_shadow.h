/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_remove_shadow.h - Ordinary same-spelling nominal methods
 *
 * KEY CONCEPT:
 *   A user Array class and its read methods have no governed Array authority.
 */
#ifndef XIR_ARRAY_REMOVE_SHADOW_H
#define XIR_ARRAY_REMOVE_SHADOW_H
static XrXirInstance *shadow_open(RemoveCompile *run) {
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.value_limit = 1048576;
    XrXirInstance *instance = NULL; CHECK(xr_xir_instance_new(run->program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirValue result = remove_run(instance, run->program->declarations->entry_function);
    CHECK(result.type == XR_XIR_I64 && result.payload == 0); xr_xir_value_drop(&result); return instance;
}
static void remove_goldens(RemoveCompile *run) {
    for (unsigned row = 0; row < 2; ++row) {
        const uint32_t function = remove_find(run->module, row ? "case1" : "case0");
        XrXirInstance *instance = shadow_open(run); runtime_attempts = 0;
        XrXirValue result = remove_run(instance, function);
        CHECK(result.type == (uint32_t)(row ? XR_XIR_BOOL : XR_XIR_I64) && result.payload == (row ? 1 : 16));
        const size_t sites = runtime_attempts;
        xr_xir_value_drop(&result); CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && !runtime_live && !runtime_bytes);
        for (size_t failure = 0; failure < sites; ++failure) {
            instance = shadow_open(run); runtime_attempts = 0; runtime_fail_at = failure;
            const XrXirInstance *previous_instance = instance_current(instance);
            const uint64_t previous_epoch = previous_instance->epoch;
            const unsigned previous_call = previous_instance->call ? (unsigned)xr_xir_call_state(previous_instance->call) : UINT32_MAX;
            XrXirCallStatus status = xr_xir_instance_start(instance, function, NULL, 0);
            const XrXirCallStatus started = status; unsigned polled = 0;
            while (status == XR_XIR_CALL_READY) { status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status; ++polled; }
            CHECK(status == XR_XIR_CALL_OOM && runtime_attempts > failure); runtime_fail_at = SIZE_MAX;
            result = (XrXirValue){0};
            const XrXirCallStatus taken = xr_xir_instance_take_result(instance, &result);
            const XrXirInstance *result_instance = instance_current(instance);
            XrXirCallStatus expected_take = XR_XIR_CALL_BAD_STATE;
            if (result_instance->epoch != previous_epoch) {
                CHECK(result_instance->epoch == previous_epoch + 1);
                if (result_instance->call && result_instance->state != XR_XIR_INSTANCE_FAILED) {
                    CHECK(xr_xir_task_executor_root_idle(result_instance->executor));
                    CHECK(xr_xir_task_executor_completion_status(result_instance->executor) == status);
                    expected_take = status;
                }
            }
            if (taken != expected_take || result.type || result.payload) {
                const XrXirInstance *take_instance = instance_current(instance);
                fprintf(stderr, "remove take shadow group%u row%u failure%zu start%u polled%u terminal%u take%u state%u call%u rootidle%u completion%u expected%u epochBefore%llu epochAfter%llu previousCall%u budgetExhausted%u valueType%u valuePayload%lld\n",
                    (unsigned)XR_REMOVE_MATRIX, row, failure, started, polled, status, taken, take_instance ? (unsigned)take_instance->state : UINT32_MAX,
                    take_instance && take_instance->call, take_instance && take_instance->executor ?
                        xr_xir_task_executor_root_idle(take_instance->executor) : 0,
                    take_instance && take_instance->executor ? xr_xir_task_executor_completion_status(take_instance->executor) : UINT32_MAX,
                    expected_take, (unsigned long long)previous_epoch, (unsigned long long)result_instance->epoch,
                    previous_call, instance_root(instance)->budget.exhausted,
                    result.type, (long long)result.payload);
            }
            CHECK(taken == expected_take && !result.type && !result.payload);
            XrXirCallStatus expected_free = XR_XIR_CALL_READY;
            if (result_instance->epoch != previous_epoch) {
                CHECK(expected_take == XR_XIR_CALL_OOM); expected_free = XR_XIR_CALL_OOM;
            }
            const unsigned retire_state = (unsigned)result_instance->state;
            const uint64_t retire_epoch = result_instance->epoch;
            const unsigned retire_completion = result_instance->executor ?
                (unsigned)xr_xir_task_executor_completion_status(result_instance->executor) : UINT32_MAX;
            const XrXirCallStatus freed = xr_xir_instance_free(instance);
            if (freed != expected_free)
                fprintf(stderr, "remove shadow retirement failure%zu free%u expected%u epochBefore%llu epochAfter%llu preFreeState%u preFreeCompletion%u blocks%zu bytes%zu\n",
                    failure, freed, expected_free, (unsigned long long)previous_epoch, (unsigned long long)retire_epoch,
                    retire_state, retire_completion, runtime_live, runtime_bytes);
            CHECK(freed == expected_free && !runtime_live && !runtime_bytes);
        }
        printf("user Array ordinary read-pop/read-shift case%u runtimeOOM%zu physical0\n", row, sites);
    }
}
#endif // XIR_ARRAY_REMOVE_SHADOW_H
