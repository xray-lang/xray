/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_resize_matrix.h - Exact ordinary copy/store element families
 *
 * KEY CONCEPT:
 *   No Equal, Compare, ToString or NotNullable predicate is requested.
 */
#ifndef XIR_ARRAY_RESIZE_MATRIX_H
#define XIR_ARRAY_RESIZE_MATRIX_H
#include "xir/xxir_struct.h"
#include "xir/xxir_class.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_tuple.h"
#include "xir/xxir_atomic.h"
static XrXirInstance *matrix_open(ResizeCompile *run) {
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.value_limit = 1048576;
    XrXirInstance *instance = NULL; CHECK(xr_xir_instance_new(run->program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirValue entry = resize_run(instance, run->program->declarations->entry_function);
    CHECK(entry.type == XR_XIR_I64 && entry.payload == 0); xr_xir_value_drop(&entry); return instance;
}
#if XR_RESIZE_MATRIX == 3
static void matrix_text(const XrXirValue *value) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length) && length == 5 && !memcmp(bytes, "a\0中", 5));
}
#endif
static void matrix_payload(unsigned row, const XrXirValue *value, XrXirValueAdmission *admission) {
    (void)row;
#if XR_RESIZE_MATRIX == 1
    static const uint32_t types[] = {XR_XIR_I8, XR_XIR_I16, XR_XIR_I32, XR_XIR_I64, XR_XIR_U8, XR_XIR_U16};
    (void)admission; CHECK(value->type == types[row] && value->payload == (row < 4 ? -7 : 7));
#elif XR_RESIZE_MATRIX == 2
    static const uint32_t types[] = {XR_XIR_U32, XR_XIR_U64, XR_XIR_BOOL, XR_XIR_RUNE, XR_XIR_F32, XR_XIR_F64};
    static const int64_t bits[] = {7, 7, 1, 0x4e2d, 0x3fc00000, INT64_C(0x3ff8000000000000)};
    (void)admission; CHECK(value->type == types[row] && value->payload == bits[row]);
#elif XR_RESIZE_MATRIX == 3
    XrXirValue field = {0}; XrXirFaultDetail fault = {0};
    if (row == 0) matrix_text(value);
    else if (row == 1) {
        int64_t length = 0; CHECK(xr_xir_array_len(value, admission, &length) == XR_XIR_VALUE_OK && length == 1);
        CHECK(xr_xir_array_get(value, 0, admission, &field, &fault) == XR_XIR_VALUE_OK && field.type == XR_XIR_I64 && field.payload == 7);
    } else if (row == 2) {
        CHECK(xr_xir_tuple_get(value, 0, &field) == XR_XIR_VALUE_OK && field.type == XR_XIR_I64 && field.payload == 7);
        xr_xir_value_drop(&field); CHECK(xr_xir_tuple_get(value, 1, &field) == XR_XIR_VALUE_OK); matrix_text(&field);
    } else if (row == 3) {
        CHECK(xr_xir_struct_get(value, 0, admission, &field) == XR_XIR_VALUE_OK && field.type == XR_XIR_I64 && field.payload == 7);
    } else if (row == 4) CHECK(xr_xir_type_is_struct(xr_xir_compile_type_arena_types(admission->arena), value->type));
    else { uint32_t variant = UINT32_MAX;
        CHECK(xr_xir_enum_variant(value, &variant) == XR_XIR_VALUE_OK && variant == 1);
        CHECK(xr_xir_enum_get(value, 1, 0, admission, &field) == XR_XIR_VALUE_OK); matrix_text(&field);
    }
    xr_xir_value_drop(&field);
#elif XR_RESIZE_MATRIX == 4
    XrXirValue field = {0};
    if (row < 2) {
        bool some = true; const XrXirValue *payload = NULL;
        CHECK(xr_xir_nullable_view(value, &some, &payload) && some == (row == 1));
        if (some) CHECK(xr_xir_nullable_view(payload, &some, &payload) && !some && !payload);
        else CHECK(!payload);
    } else if (row == 3) {
        CHECK(xr_xir_class_get(value, 0, admission, &field) == XR_XIR_VALUE_OK && field.type == XR_XIR_I64 && field.payload == 7);
    } else if (row >= 4) {
        const XrXirValue *payload = value; bool some = false;
        unsigned depth = row == 4 ? 2 : 3;
        for (unsigned layer = 0; layer < depth; ++layer) {
            CHECK(xr_xir_nullable_view(payload, &some, &payload));
            CHECK(some == !(row == 5 && layer == 2));
            if (!some) CHECK(!payload);
        }
        if (row != 5) CHECK(payload && payload->type == XR_XIR_I64 && payload->payload == 7);
    }
    xr_xir_value_drop(&field);
#elif XR_RESIZE_MATRIX == 5
    XrXirValue field = {0};
    {
        XrXirAtomicProgress progress = {0};
        XrXirType type = row == 0 ? XR_XIR_I64 : row == 1 ? XR_XIR_BOOL : XR_XIR_F64;
        XrXirAtomicRequest request = {value, NULL, NULL, 0, XR_XIR_ATOMIC_OPERATION_LOAD, type};
        XrXirAtomicOutcome result = xr_xir_atomic_start(&request, admission, NULL, &progress, &field);
        CHECK(result.status == XR_XIR_RUN_OK && !result.continuing && field.type == (uint32_t)type);
        CHECK(field.payload == (row == 0 ? 7 : row == 1 ? 1 : INT64_C(0x3ff8000000000000)));
        xr_xir_atomic_progress_clear(&progress);
    }
    xr_xir_value_drop(&field);
#endif
}
static void matrix_value(unsigned row, const XrXirValue *array) {
    CHECK(xr_xir_value_valid(array));
    XrXirDomain *domain = object_pointer(array)->domain; CHECK(domain);
    XrXirValueAdmission admission = {xr_xir_value_arena(array), domain, NULL, NULL, 100000, 1048576};
    int64_t length = 0; CHECK(xr_xir_array_len(array, &admission, &length) == XR_XIR_VALUE_OK && length == 6);
    int64_t identity = 0;
    for (int64_t i = 0; i < 6; ++i) {
        XrXirValue element = {0}; XrXirFaultDetail fault = {0};
        CHECK(xr_xir_array_get(array, i, &admission, &element, &fault) == XR_XIR_VALUE_OK);
        matrix_payload(row, &element, &admission);
        bool shared_identity = XR_RESIZE_MATRIX == 5 || (XR_RESIZE_MATRIX == 4 && row == 3);
        if (shared_identity) { if (!i) identity = element.payload; else CHECK(element.payload == identity); }
        xr_xir_value_drop(&element);
    }
}
static void matrix_faults(ResizeCompile *run, uint32_t function) {
    const size_t live = runtime_live, bytes = runtime_bytes;
    XrXirInstance *instance = matrix_open(run);
    runtime_attempts = 0; XrXirValue value = resize_run(instance, function);
    const size_t sites = runtime_attempts; xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && runtime_live == live && runtime_bytes == bytes);
    for (size_t failure = 0; failure < sites; ++failure) {
        instance = matrix_open(run); runtime_attempts = 0; runtime_fail_at = failure;
        XrXirCallStatus status = xr_xir_instance_start(instance, function, NULL, 0);
        while (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
        CHECK(runtime_attempts > failure && status == XR_XIR_CALL_OOM); runtime_fail_at = SIZE_MAX;
        value = (XrXirValue){0}; CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_BAD_STATE && !value.type && !value.payload);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && runtime_live == live && runtime_bytes == bytes);
    }
    printf("matrix%d entry%u runtimeOOM%zu physical0\n", XR_RESIZE_MATRIX, function, sites);
}
static void resize_goldens(ResizeCompile *run) {
    XrXirInstance *instance = matrix_open(run);
    const unsigned count = XR_RESIZE_MATRIX == 4 ? 7 : XR_RESIZE_MATRIX == 5 ? 3 : 6;
    for (unsigned row = 0; row < count; ++row) {
        char name[16]; CHECK(snprintf(name, sizeof(name), "case%u", row) > 0);
        uint32_t function = resize_find(run->module, name);
        XrXirValue value = resize_run(instance, function);
#if XR_RESIZE_MATRIX == 4
        if (row == 2) CHECK(value.type == XR_XIR_I64 && value.payload == 28);
        else
#endif
        matrix_value(row, &value);
        xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && !runtime_live && !runtime_bytes);
    for (unsigned row = 0; row < count; ++row) {
        char name[16]; CHECK(snprintf(name, sizeof(name), "case%u", row) > 0);
        matrix_faults(run, resize_find(run->module, name));
    }
    printf("matrix%d ordinary copy/store %u categories, Some/None exact, independent payloads\n", XR_RESIZE_MATRIX, count);
}
#endif // XIR_ARRAY_RESIZE_MATRIX_H
