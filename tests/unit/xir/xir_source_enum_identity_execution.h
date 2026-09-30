/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_enum_identity_execution.h - Enum identity strings preserve original names without inspecting payloads
 *
 * KEY CONCEPT:
 *   Owned results outlive both instances and Program, while module state stays instance-local.
 */
#ifndef XIR_SOURCE_ENUM_IDENTITY_EXECUTION_H
#define XIR_SOURCE_ENUM_IDENTITY_EXECUTION_H
/* Include after the existing runtime allocation fixture helpers. */
typedef struct SourceEnumIdentityEntries { uint32_t values[4], count; } SourceEnumIdentityEntries;
static void source_enum_identity_value_check(const XrXirValue *value, uint32_t index) {
    uint32_t kind = index % 4;
    if (!kind) CHECK(value->type == XR_XIR_I64 && value->payload == 41);
    else if (kind == 1) {
        const char *bytes = NULL; size_t length = 0;
        CHECK(value->type == XR_XIR_STRING && xr_xir_string_view(value,&bytes,&length));
        CHECK(length == 8 && !memcmp(bytes,"Box.Full",8));
    } else {
        const char *bytes = NULL; size_t length = 0;
        CHECK(value->type == XR_XIR_STRING && xr_xir_string_view(value,&bytes,&length));
        CHECK(length == (kind == 2 ? 703u : 401u));
        for (size_t i=0; i<length; ++i)
            CHECK(bytes[i] == (kind == 3 ? 'V' : i < 301 ? 'E' : i == 301 ? '.' : 'V'));
    }
}
static XrXirCallStatus source_enum_identity_count(XrXirInstance *instance, uint32_t entry, int64_t expected) {
    XrXirCallStatus status = xr_xir_instance_start(instance,entry,NULL,0);
    if (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll(instance).outcome.status;
    if (status == XR_XIR_CALL_RETURNED) {
        XrXirValue value = {0};
        CHECK(xr_xir_instance_take_result(instance,&value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && value.payload == expected); xr_xir_value_drop(&value);
    }
    return status;
}
static void source_enum_identity_attempt(XrXirProgram *program, SourceEnumIdentityEntries entries, XrXirValue results[4]) {
    XrXirInstanceConfig config = xr_xir_instance_defaults();
    XrXirInstance *instance = NULL;
    XrXirCallStatus status = xr_xir_instance_new(program,&config,&instance);
    if (status == XR_XIR_CALL_READY) status = source_enum_identity_count(instance,entries.count,0);
    for (uint32_t e = 0; e < 4 && status == XR_XIR_CALL_RETURNED; ++e) {
        status = xr_xir_instance_start(instance,entries.values[e],NULL,0);
        if (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll(instance).outcome.status;
        if (status == XR_XIR_CALL_RETURNED) {
            CHECK(xr_xir_instance_take_result(instance,&results[e]) == XR_XIR_CALL_RETURNED);
            source_enum_identity_value_check(&results[e],e);
        }
    }
    if (status == XR_XIR_CALL_RETURNED) status = source_enum_identity_count(instance,entries.count,1);
    CHECK(status == XR_XIR_CALL_RETURNED || (runtime_fail_at != SIZE_MAX && status == XR_XIR_CALL_OOM));
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
}
static void source_enum_identity_pair(XrXirProgram *program, SourceEnumIdentityEntries entries, XrXirValue retained[2][4]) {
    for (uint32_t i = 0; i < 2; ++i) source_enum_identity_attempt(program,entries,retained[i]);
}
static void source_enum_identity_retained_drop(XrXirValue retained[2][4]) {
    for (uint32_t i = 0; i < 2; ++i) for (uint32_t e = 0; e < 4; ++e) {
        source_enum_identity_value_check(&retained[i][e],e);
        xr_xir_value_drop(&retained[i][e]);
    }
}
static void source_enum_identity_runtime_failures(XrXirProgram *program, SourceEnumIdentityEntries entries) {
    size_t baseline = runtime_live, bytes = runtime_bytes, sites = 0;
    for (size_t attempt = 0; attempt <= sites; ++attempt) {
        runtime_attempts = 0; runtime_fail_at = attempt ? attempt - 1 : SIZE_MAX;
        XrXirValue values[4] = {{0}};
        source_enum_identity_attempt(program,entries,values);
        for (uint32_t e = 0; e < 4; ++e) xr_xir_value_drop(&values[e]);
        if (!attempt) sites = runtime_attempts;
        CHECK(runtime_live == baseline && runtime_bytes == bytes);
    }
    runtime_fail_at = SIZE_MAX;
    printf("Enum identity runtime physical release: %zu allocation failure sites\n",sites);
}
#endif // XIR_SOURCE_ENUM_IDENTITY_EXECUTION_H
