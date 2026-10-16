/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_map_index_resource_cases.h - Actual runtime failures and cancellation prefixes
 */
#ifndef P09_MAP_INDEX_RESOURCE_CASES_H
#define P09_MAP_INDEX_RESOURCE_CASES_H
#include "xir/xxir_nullable.h"
#include "xir/xxir_panic.h"
static XrXirCallStatus index_expected_status(uint32_t entry) {
    return entry == INDEX_MISSING ? XR_XIR_CALL_RUNTIME_PANIC : XR_XIR_CALL_RETURNED;
}
static XrXirCallStatus index_resource_poll(XrXirInstance *instance) {
    for (uint32_t tick = 0; tick < 10000; ++tick) {
        XrXirCallResult result = xr_xir_instance_poll_bounded(instance, 1).outcome;
        if (result.status == XR_XIR_CALL_RUNTIME_PANIC) CHECK(result.panic.detail.code == 431);
        if (result.status != XR_XIR_CALL_READY) return result.status;
    }
    CHECK(false); return XR_XIR_CALL_BAD_STATE;
}
static XrXirCallStatus index_resource_run(XrXirInstance *instance, uint32_t entry, XrXirValue *output, bool *driven_oom) {
    if (driven_oom) *driven_oom = false;
    XrXirCallStatus status = xr_xir_instance_start(instance, entry, NULL, 0);
    if (status == XR_XIR_CALL_READY) {
        status = index_resource_poll(instance);
        if (driven_oom) *driven_oom = status == XR_XIR_CALL_OOM;
    }
    if (status == XR_XIR_CALL_RETURNED) CHECK(xr_xir_instance_take_result(instance, output) == status);
    else CHECK(!output->type && !output->reserved && !output->payload);
    return status;
}
static void index_resource_value(const XrXirValue *value, uint32_t entry) {
    const int64_t expected[] = {1130, 9, 203, 1020, 730, 1230730};
    if (entry <= INDEX_ORDER) CHECK(value->type == XR_XIR_I64 && value->payload == expected[entry]);
    else if (entry == INDEX_CAUGHT) CHECK(value->type == XR_XIR_I64 && value->payload == 431);
    else if (entry == INDEX_LATE || entry == INDEX_MESSAGE) {
        const char *text = entry == INDEX_LATE ? "alive" : "map key not found", *bytes = NULL;
        size_t length = 0;
        CHECK(xr_xir_string_view(value, &bytes, &length));
        CHECK(length == strlen(text) && !memcmp(bytes, text, length));
    } else if (entry == INDEX_NULL_VALUE) {
        bool some = true; const XrXirValue *element = (const XrXirValue *)(uintptr_t)1;
        CHECK(xr_xir_nullable_view(value, &some, &element) && !some && !element);
    } else if (entry == INDEX_INFO) {
        XrXirFaultDetail detail = {0}; CHECK(xr_xir_panic_info_detail(value, &detail) && detail.code == 431);
    } else CHECK(entry == INDEX_MISSING && !value->type && !value->reserved && !value->payload);
}
static void index_allocations(XrXirProgram *program, const uint32_t *entries, bool full) {
    const size_t live = runtime_live, bytes = runtime_bytes;
    for (uint32_t entry = 0; entry < INDEX_COUNT; ++entry) {
        size_t points = 0;
        for (size_t pass = 0; pass <= (full ? points : 0); ++pass) {
            XrXirInstanceConfig config = {0}; XrXirInstance *instance = NULL; XrXirValue result = {0};
            bool driven_oom = false;
            CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
            runtime_attempts = 0; runtime_fail_at = pass ? pass - 1 : SIZE_MAX;
            XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
            if (status == XR_XIR_CALL_READY) status = index_resource_run(instance, entries[entry], &result, &driven_oom);
            size_t attempts = runtime_attempts; runtime_fail_at = SIZE_MAX;
            if (!pass) {
                CHECK(status == index_expected_status(entry)); index_resource_value(&result, entry);
                points = attempts; CHECK(points && points < 20000);
            } else {
                CHECK(attempts > pass - 1 && status == XR_XIR_CALL_OOM);
                CHECK(!result.type && !result.reserved && !result.payload);
                if (instance && xr_xir_instance_state(instance) != XR_XIR_INSTANCE_FAILED) {
                    CHECK(index_resource_run(instance, entries[entry], &result, &driven_oom) == index_expected_status(entry));
                    index_resource_value(&result, entry);
                }
            }
            xr_xir_value_drop(&result);
            XrXirCallStatus expected_free = driven_oom ? XR_XIR_CALL_OOM : XR_XIR_CALL_READY;
            if (instance) {
                CHECK(xr_xir_task_executor_root_idle(instance->executor));
                CHECK(xr_xir_task_executor_completion_status(instance->executor) == expected_free);
                if (driven_oom) {
                    CHECK(instance->call);
                    const XrXirExecutorBinding binding = {instance->executor, instance->call->executor_activation,
                        instance->call->executor_generation, instance->call->executor_ticket};
                    XrXirCallStatus failure = XR_XIR_CALL_BAD_STATE;
                    CHECK(xr_xir_call_driver_failure(instance->call, &binding, &failure) && failure == XR_XIR_CALL_OOM);
                }
            }
            CHECK(xr_xir_instance_free(instance) == expected_free);
            CHECK(runtime_live == live && runtime_bytes == bytes);
        }
        printf("Map index entry=%u runtime allocation sites=%zu mode=%s physical refund PASS\n",
            entry, points, full ? "full" : "baseline");
    }
}
static void index_cancellation(XrXirProgram *program, const uint32_t *entries) {
    const size_t live = runtime_live, bytes = runtime_bytes;
    for (uint32_t entry = 0; entry < INDEX_COUNT; ++entry) {
        uint32_t quanta = 0, initialization = 0, ordinary = 0;
        for (uint32_t prefix = 0; prefix < 10000; ++prefix) {
            XrXirInstanceConfig config = {0}; XrXirInstance *instance = NULL; XrXirValue result = {0};
            CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_start(instance, entries[entry], NULL, 0) == XR_XIR_CALL_READY);
            XrXirCallStatus status = XR_XIR_CALL_READY;
            for (uint32_t tick = 0; tick < prefix && status == XR_XIR_CALL_READY; ++tick)
                status = xr_xir_instance_poll_bounded(instance, 1).outcome.status;
            if (status == index_expected_status(entry)) {
                if (status == XR_XIR_CALL_RETURNED) CHECK(xr_xir_instance_take_result(instance, &result) == status);
                index_resource_value(&result, entry); quanta = prefix; xr_xir_value_drop(&result);
            } else {
                CHECK(status == XR_XIR_CALL_READY);
                CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_CANCEL_REQUESTED);
                CHECK(index_resource_poll(instance) == XR_XIR_CALL_CANCELLED);
                bool failed = xr_xir_instance_state(instance) == XR_XIR_INSTANCE_FAILED;
                XrXirCallStatus retried = index_resource_run(instance, entries[entry], &result, NULL);
                if (failed) { CHECK(retried == XR_XIR_CALL_CANCELLED); ++initialization; }
                else { CHECK(retried == index_expected_status(entry)); index_resource_value(&result, entry); ++ordinary; }
                xr_xir_value_drop(&result);
            }
            CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
            CHECK(runtime_live == live && runtime_bytes == bytes);
            if (quanta) break;
        }
        CHECK(quanta && initialization && ordinary);
        printf("Map index entry=%u cancellation prefixes=%u initializing=%u retry=%u physical refund PASS\n",
            entry, quanta, initialization, ordinary);
    }
}
#endif // P09_MAP_INDEX_RESOURCE_CASES_H
