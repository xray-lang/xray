/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_inference_execution.h - Explicit and inferred witnesses share runtime ownership
 *
 * KEY CONCEPT:
 *   Owned results outlive both instances and Program, while module state stays instance-local.
 */
#ifndef XIR_SOURCE_INFERENCE_EXECUTION_H
#define XIR_SOURCE_INFERENCE_EXECUTION_H
#include "xir/xxir_array.h"
/* Include after the existing runtime allocation fixture helpers. */
typedef struct SourceInferenceEntries { uint32_t values[6], count; } SourceInferenceEntries;
static void source_inference_value_check(const XrXirValue *value, uint32_t index) {
    uint32_t kind = index % 3;
    if (!kind) CHECK(value->type == XR_XIR_I64 && value->payload == 41);
    else if (kind == 1) {
        const char *bytes = NULL; size_t length = 0;
        CHECK(value->type == XR_XIR_STRING && xr_xir_string_view(value,&bytes,&length));
        CHECK(length == 6 && !memcmp(bytes,"mapped",6));
    } else {
        XrXirValueAdmission admission = {xr_xir_value_arena(value),NULL,NULL,NULL,1000000,65536};
        XrXirFaultDetail fault = {0}; XrXirValue item = {0}; int64_t length = 0;
        CHECK(xr_xir_array_len(value,&admission,&length) == XR_XIR_VALUE_OK && length == 1);
        CHECK(xr_xir_array_get(value,0,&admission,&item,&fault) == XR_XIR_VALUE_OK);
        CHECK(item.type == XR_XIR_I64 && item.payload == 41); xr_xir_value_drop(&item);
    }
}
static XrXirCallStatus source_inference_count(XrXirInstance *instance, uint32_t entry, int64_t expected) {
    XrXirCallStatus status = xr_xir_instance_start(instance,entry,NULL,0);
    if (status == XR_XIR_CALL_READY) status = runtime_drive(instance);
    if (status == XR_XIR_CALL_RETURNED) {
        XrXirValue value = {0};
        CHECK(xr_xir_instance_take_result(instance,&value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && value.payload == expected); xr_xir_value_drop(&value);
    }
    return status;
}
static void source_inference_attempt(XrXirProgram *program, SourceInferenceEntries entries, XrXirValue results[6]) {
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, runtime_sink, NULL};
    XrXirInstance *instance = NULL;
    XrXirCallStatus status = xr_xir_instance_new(program,&config,&instance);
    if (status == XR_XIR_CALL_READY) status = source_inference_count(instance,entries.count,0);
    for (uint32_t e = 0; e < 6 && status == XR_XIR_CALL_RETURNED; ++e) {
        status = xr_xir_instance_start(instance,entries.values[e],NULL,0);
        if (status == XR_XIR_CALL_READY) status = runtime_drive(instance);
        if (status == XR_XIR_CALL_RETURNED) {
            CHECK(xr_xir_instance_take_result(instance,&results[e]) == XR_XIR_CALL_RETURNED);
            source_inference_value_check(&results[e],e);
        }
    }
    if (status == XR_XIR_CALL_RETURNED) status = source_inference_count(instance,entries.count,6);
    CHECK(status == XR_XIR_CALL_RETURNED || (runtime_fail_at != SIZE_MAX && status == XR_XIR_CALL_OOM));
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
}
static void source_inference_pair(XrXirProgram *program, SourceInferenceEntries entries, XrXirValue retained[2][6]) {
    for (uint32_t i = 0; i < 2; ++i) source_inference_attempt(program,entries,retained[i]);
}
static void source_inference_retained_drop(XrXirValue retained[2][6]) {
    for (uint32_t i = 0; i < 2; ++i) for (uint32_t e = 0; e < 6; ++e) {
        source_inference_value_check(&retained[i][e],e);
        xr_xir_value_drop(&retained[i][e]);
    }
}
static void source_inference_runtime_failures(XrXirProgram *program, SourceInferenceEntries entries) {
    size_t baseline = runtime_live, bytes = runtime_bytes, sites = 0;
    for (size_t attempt = 0; attempt <= sites; ++attempt) {
        runtime_attempts = 0; runtime_fail_at = attempt ? attempt - 1 : SIZE_MAX;
        XrXirValue values[6] = {{0}};
        source_inference_attempt(program,entries,values);
        for (uint32_t e = 0; e < 6; ++e) xr_xir_value_drop(&values[e]);
        if (!attempt) { sites = runtime_attempts; CHECK(sites > 0); }
        else CHECK(runtime_attempts > runtime_fail_at);
        CHECK(runtime_live == baseline && runtime_bytes == bytes);
    }
    runtime_fail_at = SIZE_MAX;
    printf("Source inference runtime physical release: %zu allocation failure sites\n",sites);
}
#endif // XIR_SOURCE_INFERENCE_EXECUTION_H
