/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_resize_error_matrix.h - Erased Error and caught PanicInfo owners
 *
 * KEY CONCEPT:
 *   Real language conversions preserve payloads beyond the final execution owner.
 */
#ifndef XIR_ARRAY_RESIZE_ERROR_MATRIX_H
#define XIR_ARRAY_RESIZE_ERROR_MATRIX_H
#include "xir/xxir_error.h"
#include "xir/xxir_panic.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_class.h"
static XrXirInstance *error_matrix_open(ResizeCompile *run) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.value_limit = 1048576;
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(run->program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirValue entry = resize_run(instance, run->program->declarations->entry_function);
    CHECK(entry.type == XR_XIR_I64 && entry.payload == 0);
    xr_xir_value_drop(&entry); return instance;
}
static int64_t error_matrix_payload(unsigned row, const XrXirValue *value, XrXirValueAdmission *admission) {
    XrXirValue text = {0}; int64_t identity = 0;
    if (!row) {
        XrXirValue concrete = {0}, box = {0}, number = {0}; uint32_t variant = UINT32_MAX;
        CHECK(value->type == XR_XIR_ERROR && xr_xir_error_borrow(value, &concrete));
        CHECK(xr_xir_enum_variant(&concrete, &variant) == XR_XIR_VALUE_OK && variant == 0);
        CHECK(xr_xir_enum_get(&concrete, 0, 0, admission, &box) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_class_get(&box, 0, admission, &number) == XR_XIR_VALUE_OK);
        CHECK(number.type == XR_XIR_I64 && number.payload == 7); identity = box.payload;
        CHECK(xr_xir_enum_get(&concrete, 0, 1, admission, &text) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&number); xr_xir_value_drop(&box);
        /* The concrete value borrows Error ownership and is never dropped. */
    } else {
        XrXirFaultDetail detail = {0};
        CHECK(value->type == XR_XIR_PANIC_INFO && xr_xir_panic_info_detail(value, &detail));
        CHECK(detail.code == 420 && !detail.reserved && !detail.index && !detail.length);
        CHECK(xr_xir_panic_info_message(value, &text) == XR_XIR_VALUE_OK);
    }
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&text, &bytes, &length));
    if (!row) CHECK(length == 8 && !memcmp(bytes, "owned中", 8));
    else CHECK(length == 16 && !memcmp(bytes, "division by zero", 16));
    xr_xir_value_drop(&text); return identity;
}
static void error_matrix_value(unsigned row, const XrXirValue *array) {
    CHECK(xr_xir_value_valid(array)); XrXirDomain *domain = object_pointer(array)->domain;
    CHECK(domain);
    XrXirValueAdmission admission = {xr_xir_value_arena(array), domain, NULL, NULL, 100000, 1048576};
    int64_t length = 0, identity = 0;
    CHECK(xr_xir_array_len(array, &admission, &length) == XR_XIR_VALUE_OK && length == 6);
    for (int64_t i = 0; i < length; ++i) {
        XrXirValue element = {0}; XrXirFaultDetail fault = {0};
        CHECK(xr_xir_array_get(array, i, &admission, &element, &fault) == XR_XIR_VALUE_OK);
        int64_t actual = error_matrix_payload(row, &element, &admission);
        if (!row) { if (!i) identity = actual; else CHECK(actual == identity); }
        xr_xir_value_drop(&element);
    }
}
static void error_matrix_faults(ResizeCompile *run, uint32_t function) {
    const size_t live = runtime_live, bytes = runtime_bytes;
    XrXirInstance *instance = error_matrix_open(run);
    runtime_attempts = 0; XrXirValue value = resize_run(instance, function);
    const size_t sites = runtime_attempts; xr_xir_value_drop(&value);
    CHECK(sites && xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    CHECK(runtime_live == live && runtime_bytes == bytes);
    for (size_t failure = 0; failure < sites; ++failure) {
        instance = error_matrix_open(run); runtime_attempts = 0; runtime_fail_at = failure;
        XrXirCallStatus status = xr_xir_instance_start(instance, function, NULL, 0);
        while (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
        CHECK(runtime_attempts > failure && status == XR_XIR_CALL_OOM); runtime_fail_at = SIZE_MAX;
        value = (XrXirValue){0};
        CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_BAD_STATE && !value.type && !value.payload);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
        CHECK(runtime_live == live && runtime_bytes == bytes);
    }
    printf("matrix7 entry%u runtimeOOM%zu physical0\n", function, sites);
}
static void resize_goldens(ResizeCompile *run) {
    uint32_t functions[] = {resize_find(run->module, "case0"), resize_find(run->module, "case1")};
    XrXirInstance *instance = error_matrix_open(run);
    for (unsigned row = 0; row < 2; ++row) {
        XrXirValue value = resize_run(instance, functions[row]);
        error_matrix_value(row, &value); xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && !runtime_live && !runtime_bytes);
    for (unsigned row = 0; row < 2; ++row) error_matrix_faults(run, functions[row]);
    instance = error_matrix_open(run);
    XrXirValue escaped[] = {resize_run(instance, functions[0]), resize_run(instance, functions[1])};
    xr_xir_compile_artifact_free(run->lowered); run->lowered = NULL;
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(run->program); run->program = NULL; run->module = NULL;
    CHECK(resize_stats(&run->context).live_bytes > run->baseline.live_bytes);
    for (unsigned row = 0; row < 2; ++row) error_matrix_value(row, &escaped[row]);
    xr_xir_value_drop(&escaped[0]);
    CHECK(runtime_live && resize_stats(&run->context).live_bytes > run->baseline.live_bytes);
    xr_xir_value_drop(&escaped[1]);
    CHECK(!runtime_live && !runtime_bytes && resize_stats(&run->context).live_bytes == run->baseline.live_bytes);
    puts("matrix7 Error class identity/String7 and PanicInfo420/message exact; escaped owners final physical0");
}
#endif // XIR_ARRAY_RESIZE_ERROR_MATRIX_H
