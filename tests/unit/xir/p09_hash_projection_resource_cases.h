/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_projection_resource_cases.h - Shared physical allocation and cancellation proof
 */
#ifndef P09_HASH_PROJECTION_RESOURCE_CASES_H
#define P09_HASH_PROJECTION_RESOURCE_CASES_H
#include "xir/xxir_array.h"
#include "xir/xxir_tuple.h"
static XrXirCallStatus projection_poll(XrXirInstance *instance) {
    for (uint32_t tick = 0; tick < 10000; ++tick) {
        XrXirCallStatus status = xr_xir_instance_poll_bounded(instance,1).outcome.status;
        if (status != XR_XIR_CALL_READY) return status;
    }
    CHECK(false); return XR_XIR_CALL_BAD_STATE;
}
static XrXirCallStatus projection_run(XrXirInstance *instance, uint32_t entry, XrXirValue *output, bool *driven_oom) {
    if (driven_oom) *driven_oom = false;
    XrXirCallStatus status = xr_xir_instance_start(instance,entry,NULL,0);
    if (status == XR_XIR_CALL_READY) {
        status = projection_poll(instance);
        if (driven_oom) *driven_oom = status == XR_XIR_CALL_OOM;
    }
    if (status == XR_XIR_CALL_RETURNED)
        CHECK(xr_xir_instance_take_result(instance,output) == status);
    else CHECK(!output->type && !output->reserved && !output->payload);
    return status;
}
static void projection_result(const XrXirValue *value, uint32_t entry) {
    if (!entry) { CHECK(value->type == XR_XIR_I64 && value->payload == 123); return; }
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {xr_xir_value_arena(value), domain, NULL, NULL, 10000, 65536};
    int64_t length = 0;
    CHECK(xr_xir_array_len(value, &admission, &length) == XR_XIR_VALUE_OK && length == 2);
    for (uint32_t j = 0; j < 2; ++j) {
        XrXirValue item = {0}; XrXirFaultDetail fault = {0};
        CHECK(xr_xir_array_get(value, j, &admission, &item, &fault) == XR_XIR_VALUE_OK);
        if (entry == 4) CHECK(item.type == XR_XIR_I64 && item.payload == (j ? 3 : 5));
        else {
            XrXirValue field = {0}, key = {0}; const XrXirValue *text = &item;
            if (entry == 3) {
                CHECK(xr_xir_tuple_get(&item, 0, &key) == XR_XIR_VALUE_OK);
                CHECK(key.type == XR_XIR_I64 && key.payload == (j ? 1 : 2));
                CHECK(xr_xir_tuple_get(&item, 1, &field) == XR_XIR_VALUE_OK); text = &field;
            }
            const char *expected = entry == 1 ? (j ? "b" : "a") :
                entry == 2 ? (j ? "tail" : "alive") : (j ? "one" : "two");
            const char *bytes = NULL; size_t count = 0;
            CHECK(xr_xir_string_view(text, &bytes, &count) && count == strlen(expected) && !memcmp(bytes, expected, count));
            xr_xir_value_drop(&field); xr_xir_value_drop(&key);
        }
        xr_xir_value_drop(&item);
    }
    xr_xir_domain_drop(domain);
}
static void projection_allocations(XrXirProgram *program, const uint32_t *entries, bool full) {
    const size_t live = runtime_live, bytes = runtime_bytes;
    for (uint32_t entry = 0; entry < 5; ++entry) {
        size_t points = 0;
        for (size_t pass = 0; pass <= (full ? points : 0); ++pass) {
            XrXirInstanceConfig config = {0}; XrXirInstance *instance = NULL; XrXirValue result = {0};
            bool driven_oom = false;
            CHECK(xr_xir_instance_config_init(&config,sizeof(config)) == XR_XIR_CALL_READY);
            runtime_attempts = 0; runtime_fail_at = pass ? pass - 1 : SIZE_MAX;
            XrXirCallStatus status = xr_xir_instance_new(program,&config,&instance);
            if (status == XR_XIR_CALL_READY) status = projection_run(instance,entries[entry],&result, &driven_oom);
            size_t attempts = runtime_attempts; runtime_fail_at = SIZE_MAX;
            if (!pass) {
                CHECK(status == XR_XIR_CALL_RETURNED); projection_result(&result,entry);
                points = attempts; CHECK(points && points < 20000);
            } else CHECK(attempts > pass - 1 && status == XR_XIR_CALL_OOM &&
                !result.type && !result.reserved && !result.payload);
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
            if (pass && instance && xr_xir_instance_state(instance) != XR_XIR_INSTANCE_FAILED) {
                CHECK(projection_run(instance, entries[entry], &result, &driven_oom) == XR_XIR_CALL_RETURNED && !driven_oom);
                projection_result(&result, entry); xr_xir_value_drop(&result);
                CHECK(xr_xir_task_executor_root_idle(instance->executor));
                expected_free = XR_XIR_CALL_READY;
                CHECK(xr_xir_task_executor_completion_status(instance->executor) == expected_free);
            }
            CHECK(xr_xir_instance_free(instance) == expected_free);
            CHECK(runtime_live == live && runtime_bytes == bytes);
        }
        printf("hash members entry=%u actual runtime allocation sites=%zu mode=%s physical refund PASS\n",
            entry,points,full ? "full" : "baseline");
    }
}
static void projection_cancellation(XrXirProgram *program, const uint32_t *entries) {
    const size_t live = runtime_live, bytes = runtime_bytes;
    for (uint32_t entry = 0; entry < 5; ++entry) {
        uint32_t quanta = 0, initialization_prefixes = 0, retry_prefixes = 0;
        for (uint32_t prefix = 0; prefix < 10000; ++prefix) {
            XrXirInstanceConfig config = {0}; XrXirInstance *instance = NULL; XrXirValue result = {0};
            CHECK(xr_xir_instance_config_init(&config,sizeof(config)) == XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_new(program,&config,&instance) == XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_start(instance,entries[entry],NULL,0) == XR_XIR_CALL_READY);
            XrXirCallStatus status = XR_XIR_CALL_READY;
            for (uint32_t tick = 0; tick < prefix && status == XR_XIR_CALL_READY; ++tick)
                status = xr_xir_instance_poll_bounded(instance,1).outcome.status;
            if (status == XR_XIR_CALL_RETURNED) {
                CHECK(xr_xir_instance_take_result(instance,&result) == status);
                projection_result(&result,entry); quanta = prefix; xr_xir_value_drop(&result);
            } else {
                CHECK(status == XR_XIR_CALL_READY);
                CHECK(xr_xir_instance_take_result(instance,&result) == XR_XIR_CALL_BAD_STATE);
                CHECK(!result.type && !result.reserved && !result.payload);
                CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_CANCEL_REQUESTED);
                CHECK(projection_poll(instance) == XR_XIR_CALL_CANCELLED);
                CHECK(xr_xir_instance_take_result(instance,&result) == XR_XIR_CALL_BAD_STATE);
                /* Cancelling initialization permanently fails the Instance.
                 * After initialization, an ordinary cancelled call can retry. */
                bool initialization = xr_xir_instance_state(instance) == XR_XIR_INSTANCE_FAILED;
                XrXirCallStatus retry = projection_run(instance,entries[entry],&result, NULL);
                if (initialization) {
                    CHECK(retry == XR_XIR_CALL_CANCELLED && !result.type && !result.reserved && !result.payload);
                    ++initialization_prefixes;
                } else {
                    CHECK(retry == XR_XIR_CALL_RETURNED); projection_result(&result,entry);
                    xr_xir_value_drop(&result); ++retry_prefixes;
                }
            }
            CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
            CHECK(runtime_live == live && runtime_bytes == bytes);
            if (quanta) break;
        }
        CHECK(quanta && initialization_prefixes && retry_prefixes);
        printf("hash members entry=%u cancellation prefixes=%u initializing=%u retry=%u physical refund PASS\n",
            entry,quanta,initialization_prefixes,retry_prefixes);
    }
}
#endif // P09_HASH_PROJECTION_RESOURCE_CASES_H
