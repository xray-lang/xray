/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_pod_display_cases.h - Inline text owns no borrowed value metadata
 */
#ifndef NATIVE_POD_DISPLAY_CASES_H
#define NATIVE_POD_DISPLAY_CASES_H
#include "xray_native_pod.h"
typedef struct PodDisplayProbe {
    XrPodType types[128];
    uint8_t bytes[1032];
    uint32_t count, type;
    size_t size;
    const char *expected;
    XrtString *text;
    bool completed, growth, missing;
} PodDisplayProbe;
static void pod_probe_init(PodDisplayProbe *probe, XrPodScalar scalar, uint32_t depth,
    uint64_t bits, const char *expected) {
    memset(probe, 0, sizeof(*probe));
    XrPodLayout layout;
    CHECK(depth && depth <= 128 && xr_pod_scalar_layout(scalar, &layout));
    for (uint32_t n = 0; n < depth; ++n) {
        CHECK(xr_pod_nullable_layout(layout, &layout));
        probe->types[n] = (XrPodType){XR_POD_NULLABLE, 0,
            n ? XR_POD_SCALAR_COUNT + n - 1 : (uint32_t)scalar, layout};
    }
    probe->type = XR_POD_SCALAR_COUNT + depth - 1;
    probe->count = depth; probe->size = layout.size; probe->expected = expected;
    CHECK(layout.size <= sizeof(probe->bytes));
    size_t offset = 0;
    for (uint32_t n = depth; n; --n) {
        probe->bytes[offset] = 1;
        XrPodLayout payload;
        if (n > 1) payload = probe->types[n - 2].physical;
        else CHECK(xr_pod_scalar_layout(scalar, &payload));
        offset += payload.alignment;
    }
    CHECK(xr_pod_scalar_layout(scalar, &layout));
    if (layout.size) memcpy(probe->bytes + offset, &bits, layout.size);
}
static void pod_display_root(void *opaque) {
    PodDisplayProbe *probe = opaque;
    if (probe->growth) {
        static const char *const literals[] = {"0", "1", "2", "3", "4", "5", "6", "7"};
        for (uint32_t n = 0; n < 8; ++n) (void)xrt_string_literal(literals[n], 1, 1);
    }
    XrtString *text = xrt_pod_display(probe->types, probe->count, probe->type,
        probe->missing ? NULL : probe->bytes, probe->size, 1);
    string_equals(text, probe->expected, strlen(probe->expected));
    probe->text = xrt_string_root_take(text);
    probe->completed = true;
}
static void pod_display_cases(XrtLocationOwner *locations) {
    static const struct { XrPodScalar scalar; uint64_t bits; const char *text; } cases[] = {
        {XR_POD_UNIT, 0, "()"}, {XR_POD_BOOL, 1, "true"}, {XR_POD_BOOL, 0, "false"},
        {XR_POD_I8, 128, "-128"}, {XR_POD_I16, 32768, "-32768"},
        {XR_POD_I32, UINT64_C(2147483648), "-2147483648"},
        {XR_POD_I64, UINT64_C(9223372036854775808), "-9223372036854775808"},
        {XR_POD_U8, 255, "255"}, {XR_POD_U16, 65535, "65535"},
        {XR_POD_U32, UINT64_C(4294967295), "4294967295"},
        {XR_POD_U64, UINT64_MAX, "18446744073709551615"},
        {XR_POD_F32, UINT64_C(1067450368), "1.25"}, {XR_POD_F64, UINT64_C(4608308318706860032), "1.25"}
    };
    XrtExecutionFailure failure = {0};
    for (uint32_t n = 0; n < sizeof(cases) / sizeof(*cases); ++n) {
        for (uint32_t depth = 1; depth <= 128; depth *= 128) {
            PodDisplayProbe probe;
            pod_probe_init(&probe, cases[n].scalar, depth, cases[n].bits, cases[n].text);
            CHECK(xrt_run_embedded(locations, &failure, pod_display_root, &probe) == XRT_EXECUTION_COMPLETED);
            CHECK(probe.completed && probe.text);
            memset(probe.types, 0xa5, sizeof(probe.types));
            memset(probe.bytes, 0xa5, sizeof(probe.bytes));
            string_equals(probe.text, cases[n].text, strlen(cases[n].text));
            xrt_string_owned_drop(&probe.text);
            pod_probe_init(&probe, cases[n].scalar, depth, UINT64_MAX, "null");
            memset(probe.bytes, 0xa5, sizeof(probe.bytes)); probe.bytes[0] = 0;
            CHECK(xrt_run_embedded(locations, &failure, pod_display_root, &probe) == XRT_EXECUTION_COMPLETED);
            xrt_string_owned_drop(&probe.text);
        }
    }
    for (uint32_t n = 0; n < 4; ++n) {
        PodDisplayProbe probe;
        pod_probe_init(&probe, n == 3 ? XR_POD_BOOL : XR_POD_I64, 1, n == 3 ? 2 : 7, "");
        if (n == 0) probe.bytes[0] = 2;
        if (n == 1) --probe.size;
        if (n == 2) probe.missing = true;
        XrtValueRoot root = {0};
        CHECK(xrt_run_embedded_owned(locations, &failure, pod_display_root, &probe, &root) == XRT_EXECUTION_FAILURE);
        CHECK(!probe.completed && !probe.text && xrt_value_root_empty(&root) && failure.kind == XRT_FAILURE_VALUE &&
            failure.detail.value.status == XR_XIR_VALUE_BAD_ARGUMENT && failure.detail.value.source_id == 1);
    }
}
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
static void pod_display_allocation_sweep(XrtLocationOwner *locations) {
    PodDisplayProbe probe;
    pod_probe_init(&probe, XR_POD_I64, 128, 7, "7"); probe.growth = true;
    XrtExecutionFailure failure = {0};
    native_array_calls = 0;
    CHECK(xrt_run_embedded(locations, &failure, pod_display_root, &probe) == XRT_EXECUTION_COMPLETED);
    size_t points = native_array_calls;
    xrt_string_owned_drop(&probe.text);
    CHECK(points && !native_array_live && !native_array_bytes);
    bool publication_failure = false;
    for (size_t point = 0; point < points; ++point) {
        native_array_calls = 0; native_array_fail_at = point;
        native_array_injected = native_array_pending_failure = false;
        probe.completed = false;
        XrtValueRoot root = {0};
        CHECK(xrt_run_embedded_owned(locations, &failure, pod_display_root, &probe, &root) == XRT_EXECUTION_FAILURE);
        CHECK(native_array_injected && failure.kind == XRT_FAILURE_VALUE &&
            failure.detail.value.status == XR_XIR_VALUE_OOM && !probe.completed && !probe.text);
        CHECK(xrt_value_root_empty(&root) && !native_array_live && !native_array_bytes);
        publication_failure |= native_array_pending_failure;
    }
    native_array_fail_at = SIZE_MAX;
    CHECK(publication_failure);
    printf("Inline Nullable display allocation failure points: %zu\n", points);
}
#endif
#endif // NATIVE_POD_DISPLAY_CASES_H
