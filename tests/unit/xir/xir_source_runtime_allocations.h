/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_runtime_allocations.h - Exercise allocation failures in the source fixture
 *
 * KEY CONCEPT:
 *   The source fixture runs its exact entry and suspension cases with physical accounting.
 */
#ifndef XIR_SOURCE_RUNTIME_ALLOCATIONS_H
#define XIR_SOURCE_RUNTIME_ALLOCATIONS_H
#include "xir_runtime_allocations.h"

static bool runtime_sink(void *context, const XrXirOutputGroup *group) {
    (void) context;
    CHECK(group && group->count <= 8); return true;
}
static XrXirCallStatus runtime_drive(XrXirInstance *instance) {
    XrXirInstanceResult result = xr_xir_instance_poll(instance);
    unsigned wakes = 0;
    while (result.outcome.status == XR_XIR_CALL_SUSPENDED) {
        CHECK(++wakes <= 2);
        CHECK(xr_xir_instance_resume(instance, result.epoch, result.outcome.wake) == XR_XIR_CALL_READY);
        result = xr_xir_instance_poll(instance);
    }
    return result.outcome.status;
}
static void runtime_source_attempt(XrXirProgram *program, uint32_t entry, uint32_t resume_text, uint32_t numeric_pause) {
    XrXirInstanceConfig config = xr_xir_instance_defaults();
    config.output = (XrXirOutputProvider) {runtime_sink, NULL};
    XrXirInstance *instance = NULL; XrXirValue result = {0}, argument = {XR_XIR_BOOL, 0, 1};
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
    if (status == XR_XIR_CALL_READY) status = xr_xir_instance_start(instance, entry, NULL, 0);
    if (status == XR_XIR_CALL_READY) status = runtime_drive(instance);
    for (uint32_t i = 0; i < 2 && status == XR_XIR_CALL_RETURNED; ++i) {
        argument.payload = i;
        status = xr_xir_instance_start(instance, numeric_pause, &argument, 1);
        if (status == XR_XIR_CALL_READY) status = runtime_drive(instance);
        if (status == XR_XIR_CALL_RETURNED) {
            XrXirValue numeric = {0};
            CHECK(xr_xir_instance_take_result(instance, &numeric) == XR_XIR_CALL_RETURNED);
            CHECK(numeric.type == XR_XIR_I64 && numeric.payload == (i ? -128 : 32767));
            xr_xir_value_drop(&numeric);
        }
    }
    argument.payload = 1;
    if (status == XR_XIR_CALL_RETURNED) status = xr_xir_instance_start(instance, resume_text, &argument, 1);
    if (status == XR_XIR_CALL_READY) status = runtime_drive(instance);
    if (status == XR_XIR_CALL_RETURNED)
        CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
    else CHECK(runtime_fail_at != SIZE_MAX && status == XR_XIR_CALL_OOM);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    if (status == XR_XIR_CALL_RETURNED) {
        const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(&result, &bytes, &length) && length == 3 && !memcmp(bytes, "ry!", 3));
        xr_xir_value_drop(&result);
    }
}
static void runtime_source_failures(XrXirProgram *program, uint32_t entry, uint32_t resume_text, uint32_t numeric_pause) {
    size_t baseline = runtime_live, bytes = runtime_bytes, sites = 0;
    for (size_t attempt = 0; attempt <= sites; ++attempt) {
        runtime_attempts = 0; runtime_fail_at = attempt ? attempt - 1 : SIZE_MAX;
        runtime_source_attempt(program, entry, resume_text, numeric_pause);
        if (!attempt) sites = runtime_attempts;
        CHECK(runtime_live == baseline && runtime_bytes == bytes);
    }
    runtime_fail_at = SIZE_MAX;
    printf("Real source runtime physical release: %zu allocation failure sites\n", sites);
}
#endif // XIR_SOURCE_RUNTIME_ALLOCATIONS_H
