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
#include "xir/xxir_array.h"

typedef struct RuntimeSourceEntries {
    uint32_t entry, resume_text, numeric_pause, enum_witness, enum_generic_witness;
    uint32_t generic_method_number, generic_method_text, generic_method_array;
} RuntimeSourceEntries;

static XrXirOutputStatus runtime_sink(void *context, const XrXirOutputGroup *group) {
    (void) context;
    CHECK(group && group->count <= 18); return XR_XIR_OUTPUT_OK;
}
static XrXirCallStatus runtime_drive(XrXirInstance *instance) {
    XrXirInstanceResult result = xr_xir_instance_poll_bounded(instance, UINT64_MAX);
    unsigned wakes = 0;
    while (result.outcome.status == XR_XIR_CALL_SUSPENDED) {
        CHECK(++wakes <= 13);
        CHECK(xr_xir_instance_resume(instance, result.epoch, result.outcome.wake) == XR_XIR_CALL_READY);
        result = xr_xir_instance_poll_bounded(instance, UINT64_MAX);
    }
    return result.outcome.status;
}
static void runtime_source_attempt(XrXirProgram *program, RuntimeSourceEntries entries) {
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, runtime_sink, NULL};
    XrXirInstance *instance = NULL; XrXirValue result = {0}, argument = {XR_XIR_BOOL, 0, 1};
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
    if (status == XR_XIR_CALL_READY) status = xr_xir_instance_start(instance, entries.entry, NULL, 0);
    if (status == XR_XIR_CALL_READY) status = runtime_drive(instance);
    uint32_t enum_entries[] = {entries.enum_witness,entries.enum_generic_witness};
    for (uint32_t f = 0; f < 2 && status == XR_XIR_CALL_RETURNED; ++f) {
        status = xr_xir_instance_start(instance,enum_entries[f],NULL,0);
        if (status == XR_XIR_CALL_READY) status = runtime_drive(instance);
        if (status == XR_XIR_CALL_RETURNED) {
            XrXirValue value = {0};
            CHECK(xr_xir_instance_take_result(instance,&value) == XR_XIR_CALL_RETURNED);
            CHECK(value.type == XR_XIR_I64 && value.payload == 41);
            xr_xir_value_drop(&value);
        }
    }
    XrXirValue generic[3] = {{0}};
    uint32_t generic_entries[] = {entries.generic_method_number,entries.generic_method_text,entries.generic_method_array};
    for (uint32_t f = 0; f < 3 && status == XR_XIR_CALL_RETURNED; ++f) {
        status = xr_xir_instance_start(instance,generic_entries[f],NULL,0);
        if (status == XR_XIR_CALL_READY) status = runtime_drive(instance);
        if (status == XR_XIR_CALL_RETURNED) {
            CHECK(xr_xir_instance_take_result(instance,&generic[f]) == XR_XIR_CALL_RETURNED);
            if (!f) CHECK(generic[f].type == XR_XIR_I64 && generic[f].payload == 41);
            else if (f == 1) {
                const char *bytes = NULL; size_t length = 0;
                CHECK(generic[f].type == XR_XIR_STRING && xr_xir_string_view(&generic[f],&bytes,&length));
                CHECK(length == 6 && !memcmp(bytes,"mapped",6));
            } else {
                XrXirValueAdmission admission = {xr_xir_value_arena(&generic[f]),NULL,NULL,NULL,1000000,65536};
                int64_t length = 0;
                CHECK(xr_xir_array_len(&generic[f],&admission,&length) == XR_XIR_VALUE_OK && length == 1);
            }
        }
    }
    for (uint32_t i = 0; i < 2 && status == XR_XIR_CALL_RETURNED; ++i) {
        argument.payload = i;
        status = xr_xir_instance_start(instance, entries.numeric_pause, &argument, 1);
        if (status == XR_XIR_CALL_READY) status = runtime_drive(instance);
        if (status == XR_XIR_CALL_RETURNED) {
            XrXirValue numeric = {0};
            CHECK(xr_xir_instance_take_result(instance, &numeric) == XR_XIR_CALL_RETURNED);
            CHECK(numeric.type == XR_XIR_I64 && numeric.payload == (i ? -128 : 32767));
            xr_xir_value_drop(&numeric);
        }
    }
    argument.payload = 1;
    if (status == XR_XIR_CALL_RETURNED) status = xr_xir_instance_start(instance, entries.resume_text, &argument, 1);
    if (status == XR_XIR_CALL_READY) status = runtime_drive(instance);
    if (status == XR_XIR_CALL_RETURNED)
        CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
    else CHECK(runtime_fail_at != SIZE_MAX && status == XR_XIR_CALL_OOM);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    for (uint32_t f = 0; f < 3; ++f) xr_xir_value_drop(&generic[f]);
    if (status == XR_XIR_CALL_RETURNED) {
        const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(&result, &bytes, &length) && length == 3 && !memcmp(bytes, "ry!", 3));
        xr_xir_value_drop(&result);
    }
}
static void runtime_source_failures(XrXirProgram *program, RuntimeSourceEntries entries) {
    size_t baseline = runtime_live, bytes = runtime_bytes, sites = 0;
    for (size_t attempt = 0; attempt <= sites; ++attempt) {
        runtime_attempts = 0; runtime_fail_at = attempt ? attempt - 1 : SIZE_MAX;
        runtime_source_attempt(program,entries);
        if (!attempt) sites = runtime_attempts;
        CHECK(runtime_live == baseline && runtime_bytes == bytes);
    }
    runtime_fail_at = SIZE_MAX;
    printf("Real source runtime physical release: %zu allocation failure sites\n", sites);
}
#endif // XIR_SOURCE_RUNTIME_ALLOCATIONS_H
