/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_array_repeat_cases.h - Repeated elements retain real owners across roots
 */
#ifndef NATIVE_ARRAY_REPEAT_CASES_H
#define NATIVE_ARRAY_REPEAT_CASES_H

typedef struct ArrayRepeatProbe {
    int64_t length;
    bool string, wrong_fill, completed;
    XrtString *external;
    XrtArray *result;
} ArrayRepeatProbe;

static void repeat_root(void *opaque) {
    ArrayRepeatProbe *probe = opaque;
    XrtString *fill = probe->external;
    if (probe->string && !fill) fill = xrt_string_literal(original, sizeof(original) - 1, 1);
    if (probe->wrong_fill) fill = (XrtString *)xrt_array_i64_new(NULL, 0, 1);
    XrtArray *array = probe->string ? xrt_array_string_repeat(probe->length, fill, 1) :
        xrt_array_i64_repeat(probe->length, 7, 1);
    CHECK(xrt_array_length(array, 1) == probe->length);
    XrtArray *saved = xrt_array_retain(array, 1);
    if (probe->length > 0) {
        if (probe->string) {
            XrtString *next = xrt_string_literal("replacement", 11, 1);
            xrt_array_string_set(&array, 0, next, 1);
            XrtString *old = xrt_array_string_get(saved, 0, 1);
            string_equals(old, original, sizeof(original) - 1);
            xrt_string_drop(&old);
            old = xrt_array_string_get(array, probe->length - 1, 1);
            string_equals(old, original, sizeof(original) - 1);
            xrt_string_drop(&old);
        } else {
            xrt_array_i64_set(&array, 0, 9, 1);
            CHECK(xrt_array_i64_get(saved, 0, 1) == 7 && xrt_array_i64_get(array, 0, 1) == 9);
        }
    }
    xrt_array_drop(&array);
    if (probe->external) probe->result = xrt_array_root_take(saved);
    else xrt_array_drop(&saved);
    probe->completed = true;
}

static void repeat_cases(XrtLocationOwner *locations) {
    for (unsigned family = 0; family < 2; ++family) {
        for (unsigned empty = 0; empty < 2; ++empty) {
            ArrayRepeatProbe probe = {.length=empty ? 0 : 64, .string=family != 0};
            XrtExecutionFailure failure = {0};
            CHECK(xrt_run_embedded(locations, &failure, repeat_root, &probe) == XRT_EXECUTION_COMPLETED);
            CHECK(probe.completed);
        }
        ArrayRepeatProbe negative = {.length=-1, .string=family != 0};
        XrtExecutionFailure failure = {0};
        XrtValueRoot root = {0};
        CHECK(xrt_run_embedded_owned(locations, &failure, repeat_root, &negative, &root) == XRT_EXECUTION_FAILURE);
        CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_BAD_ARGUMENT &&
            failure.detail.value.source_id == 1 && !negative.completed && xrt_value_root_empty(&root));
        negative.length = INT64_MAX;
        CHECK(xrt_run_embedded_owned(locations, &failure, repeat_root, &negative, &root) == XRT_EXECUTION_FAILURE);
        CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_LIMIT &&
            !negative.completed && xrt_value_root_empty(&root));
    }
    ArrayRepeatProbe wrong = {.string=true, .wrong_fill=true};
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded(locations, &failure, repeat_root, &wrong) == XRT_EXECUTION_FAILURE);
    CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_BAD_ARGUMENT && !wrong.completed);
    ArrayRepeatProbe limited = {.length=64};
    XrtValueRoot root = {.byte_limit=1};
    CHECK(xrt_run_embedded_owned(locations, &failure, repeat_root, &limited, &root) == XRT_EXECUTION_FAILURE);
    CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_LIMIT &&
        !limited.completed && xrt_value_root_empty(&root));
}

static void repeat_observe(void *opaque) {
    ArrayRepeatProbe *probe = opaque;
    XrtString *element = xrt_array_string_get(probe->result, 63, 1);
    string_equals(element, original, sizeof(original) - 1);
    XrtArray *copy = xrt_array_retain(probe->result, 1);
    xrt_array_string_push(&copy, element, 1);
    CHECK(xrt_array_length(copy, 1) == 65 && xrt_array_length(probe->result, 1) == 64);
    xrt_array_drop(&copy);
    xrt_string_drop(&element);
}

static void repeat_cross_root(XrtLocationOwner *locations) {
    XrXirDomain *domain = NULL;
    OK(xr_xir_domain_new(UINT64_MAX, &domain));
    uint64_t baseline = xr_xir_domain_stats(domain).live_bytes;
    XrXirValue fill = {0};
    OK(xr_xir_string_new(domain, original, sizeof(original) - 1, &fill));
    ArrayRepeatProbe probe = {.length=64, .string=true, .external=(XrtString *)(intptr_t)fill.payload};
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded(locations, &failure, repeat_root, &probe) == XRT_EXECUTION_COMPLETED && probe.result);
    xr_xir_value_drop(&fill);
    CHECK(xr_xir_domain_stats(domain).live_bytes > baseline);
    CHECK(xrt_run_embedded(locations, &failure, repeat_observe, &probe) == XRT_EXECUTION_COMPLETED);
    xrt_array_owned_drop(&probe.result);
    CHECK(xr_xir_domain_stats(domain).live_bytes == baseline);
    xr_xir_domain_drop(domain);
}

#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
static void repeat_allocation_sweep(XrtLocationOwner *locations, bool string) {
    ArrayRepeatProbe probe = {.length=64, .string=string};
    XrtExecutionFailure failure = {0};
    native_array_calls = 0;
    CHECK(xrt_run_embedded(locations, &failure, repeat_root, &probe) == XRT_EXECUTION_COMPLETED && probe.completed);
    size_t points = native_array_calls;
    CHECK(!native_array_live && !native_array_bytes);
    for (size_t point = 0; point < points; ++point) {
        native_array_calls = 0; native_array_fail_at = point; native_array_injected = false;
        probe.completed = false;
        XrtValueRoot root = {0};
        CHECK(xrt_run_embedded_owned(locations, &failure, repeat_root, &probe, &root) == XRT_EXECUTION_FAILURE);
        CHECK(native_array_injected && failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_OOM);
        CHECK(!probe.completed && xrt_value_root_empty(&root) && !native_array_live && !native_array_bytes);
    }
    native_array_fail_at = SIZE_MAX;
    printf("Array %s repeat allocation failure points: %zu\n", string ? "String" : "i64", points);
}
#endif
#endif // NATIVE_ARRAY_REPEAT_CASES_H
