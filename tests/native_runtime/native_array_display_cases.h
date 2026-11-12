/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_array_display_cases.h - Owned text from the canonical Array formatter
 */
#ifndef NATIVE_ARRAY_DISPLAY_CASES_H
#define NATIVE_ARRAY_DISPLAY_CASES_H

typedef struct ArrayDisplayProbe {
    unsigned family;
    bool empty, wrong_family, export_text, publication_growth, completed;
    XrtArray *array;
    XrtString *text;
} ArrayDisplayProbe;

static XrtString *(*const array_display_functions[])(const XrtArray *, uint32_t) = {
    xrt_array_i64_display, xrt_array_string_display, xrt_array_u8_display, xrt_array_u64_display
};
static const char *const array_display_expected[] = {
    "[-9223372036854775808, 9223372036854775807]",
    "[\"a\"b\\c\n\", \"\xe4\xbd\xa0\xe5\xa5\xbd\xf0\x9f\x8c\x8d\"]",
    "[0, 255]", "[9223372036854775808, 18446744073709551615]"
};

static XrtArray *display_array_new(unsigned family, bool empty) {
    size_t count = empty ? 0 : 2;
    switch (family) {
    case 0: {
        const int64_t values[] = {INT64_MIN, INT64_MAX};
        return xrt_array_i64_new(values, count, 1);
    }
    case 1: {
        XrtString *values[] = {xrt_string_literal("a\"b\\c\n", 6, 1),
            xrt_string_literal("\xe4\xbd\xa0\xe5\xa5\xbd\xf0\x9f\x8c\x8d", 10, 1)};
        return xrt_array_string_new(values, count, 1);
    }
    case 2: {
        const uint8_t values[] = {0, UINT8_MAX};
        return xrt_array_u8_new(values, count, 1);
    }
    default: {
        const uint64_t values[] = {UINT64_C(9223372036854775808), UINT64_MAX};
        return xrt_array_u64_new(values, count, 1);
    }
    }
}

static void display_root(void *opaque) {
    ArrayDisplayProbe *probe = opaque;
    XrtArray *array = probe->array ? probe->array : display_array_new(probe->family, probe->empty);
    if (probe->publication_growth) {
        static const char *const literals[] = {"0", "1", "2", "3", "4", "5", "6", "7"};
        unsigned count = probe->family == 1 ? 6 : 8;
        for (unsigned i = 0; i < count; ++i) (void)xrt_string_literal(literals[i], 1, 1);
    }
    unsigned family = probe->wrong_family ? (probe->family + 1) % 4 : probe->family;
    XrtString *text = array_display_functions[family](array, 1);
    const char *expected = probe->empty ? "[]" : array_display_expected[probe->family];
    string_equals(text, expected, strlen(expected));
    if (probe->export_text) probe->text = xrt_string_root_take(text);
    else xrt_string_drop(&text);
    if (!probe->array) xrt_array_drop(&array);
    probe->completed = true;
}

static void display_foreign_create(void *opaque) {
    ArrayDisplayProbe *probe = opaque;
    probe->array = xrt_array_root_take(display_array_new(probe->family, false));
}

static void array_display_cases(XrtLocationOwner *locations) {
    for (unsigned family = 0; family < 4; ++family) {
        XrtExecutionFailure failure = {0};
        for (unsigned empty = 0; empty < 2; ++empty) {
            ArrayDisplayProbe probe = {.family=family, .empty=empty != 0};
            CHECK(xrt_run_embedded(locations, &failure, display_root, &probe) == XRT_EXECUTION_COMPLETED);
            CHECK(probe.completed);
        }
        ArrayDisplayProbe wrong = {.family=family, .wrong_family=true};
        XrtValueRoot root = {0};
        CHECK(xrt_run_embedded_owned(locations, &failure, display_root, &wrong, &root) == XRT_EXECUTION_FAILURE);
        CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_BAD_ARGUMENT &&
            failure.detail.value.source_id == 1 && !wrong.completed && xrt_value_root_empty(&root));
        ArrayDisplayProbe foreign = {.family=family, .export_text=true};
        CHECK(xrt_run_embedded(locations, &failure, display_foreign_create, &foreign) == XRT_EXECUTION_COMPLETED);
        CHECK(xrt_run_embedded(locations, &failure, display_root, &foreign) == XRT_EXECUTION_COMPLETED);
        xrt_array_owned_drop(&foreign.array);
        CHECK(foreign.completed && foreign.text);
        string_equals(foreign.text, array_display_expected[family], strlen(array_display_expected[family]));
        xrt_string_owned_drop(&foreign.text);
    }
}

#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
static void display_allocation_sweep(XrtLocationOwner *locations, unsigned family) {
    ArrayDisplayProbe probe = {.family=family, .publication_growth=true};
    XrtExecutionFailure failure = {0};
    native_array_calls = 0;
    CHECK(xrt_run_embedded(locations, &failure, display_root, &probe) == XRT_EXECUTION_COMPLETED);
    size_t points = native_array_calls;
    CHECK(!native_array_live && !native_array_bytes);
    bool publication_failure = false;
    for (size_t point = 0; point < points; ++point) {
        native_array_calls = 0; native_array_fail_at = point;
        native_array_injected = native_array_pending_failure = false;
        probe.completed = false;
        XrtValueRoot root = {0};
        CHECK(xrt_run_embedded_owned(locations, &failure, display_root, &probe, &root) == XRT_EXECUTION_FAILURE);
        CHECK(native_array_injected && failure.kind == XRT_FAILURE_VALUE &&
            failure.detail.value.status == XR_XIR_VALUE_OOM && !probe.completed);
        CHECK(xrt_value_root_empty(&root) && !native_array_live && !native_array_bytes);
        publication_failure |= native_array_pending_failure;
    }
    native_array_fail_at = SIZE_MAX;
    CHECK(publication_failure);
    printf("Array display family %u allocation failure points: %zu\n", family, points);
}
#endif
#endif // NATIVE_ARRAY_DISPLAY_CASES_H
