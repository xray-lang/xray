/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_string_append_cases.h - Logical aliases and actual String transactions
 */
#include "xray_native_string.h"
typedef struct StringAppendProbe {
    bool snapshot, pinned, self, empty, completed;
} StringAppendProbe;
static void native_string_append_in_capacity(void *opaque) {
    unsigned scenario = *(const unsigned *)opaque;
    XrtString *value = xrt_string_new("a", 1, 1);
    XrtString *suffix = xrt_string_literal("x", 1, 1);
    XrtString *original = value;
    XrXirValue borrowed = {XR_XIR_STRING, 0, (int64_t)(intptr_t)value}, saved = {0};
    if (scenario) OK(xr_xir_value_share(&borrowed));
    if (scenario == 2) OK(xr_xir_value_copy(&borrowed, &saved));
    XrXirDomain *foreign = NULL;
    OK(xr_xir_root_domain_new(UINT64_MAX, &foreign));
    CHECK(!xr_xir_root_string_append_in_place(foreign, value, suffix));
    xr_xir_root_domain_release(foreign);
    XrXirDomain *domain = value->object.domain;
    XrXirDomainStats before = xr_xir_domain_stats(domain);
    value = xrt_string_append_owned(value, suffix, 1);
    string_equals(value, "ax", 2);
    if (scenario != 2) {
        XrXirDomainStats after = xr_xir_domain_stats(domain);
        CHECK(value == original && xir_object_references(&value->object) == 1);
        CHECK(after.allocations == before.allocations && after.reallocations == before.reallocations &&
            after.live_bytes == before.live_bytes);
        value = xrt_string_append_owned(value, value, 1);
        CHECK(value == original);
        string_equals(value, "axax", 4);
    } else {
        CHECK(value != original);
        string_equals((XrtString *)(intptr_t)saved.payload, "a", 1);
        CHECK(value->object.graph_registered && !value->object.root_private);
        XrtString *joined = value;
        XrXirDomainStats current = xr_xir_domain_stats(domain);
        value = xrt_string_append_owned(value, suffix, 1);
        XrXirDomainStats after = xr_xir_domain_stats(domain);
        CHECK(value == joined && after.allocations == current.allocations &&
            after.reallocations == current.reallocations && after.live_bytes == current.live_bytes);
        string_equals(value, "axx", 3);
        string_equals((XrtString *)(intptr_t)saved.payload, "a", 1);
    }
    xr_xir_value_drop(&saved);
    xrt_string_drop(&value);
}
static void native_string_append_root(void *opaque) {
    StringAppendProbe *probe = opaque;
    const char *seed = "a long original string to force self-append growth";
    XrtString *value = probe->pinned ? xrt_string_retain(xrt_string_literal(seed, strlen(seed), 1), 1) :
        xrt_string_new(seed, strlen(seed), 1);
    XrtString *saved = probe->snapshot ? xrt_string_retain(value, 1) : NULL;
    XrtString *suffix = probe->self ? value : xrt_string_literal(probe->empty ? "" : "Ω", probe->empty ? 0 : 2, 1);
    XrtStringRef reference = xrt_string_ref_local(&value, 1);
    xrt_string_ref_append(reference, suffix, 1);
    const char *expected = probe->self ?
        "a long original string to force self-append growtha long original string to force self-append growth" :
        probe->empty ? seed : "a long original string to force self-append growthΩ";
    string_equals(value, expected, strlen(expected));
    if (saved) { string_equals(saved, seed, strlen(seed)); xrt_string_drop(&saved); }
    xrt_string_drop(&value);
    probe->completed = true;
}
static void native_string_append_cases(XrtLocationOwner *locations) {
    for (unsigned scenario = 0; scenario != 3; ++scenario) {
        XrtExecutionFailure failure = {0};
        CHECK(xrt_run_embedded(locations, &failure, native_string_append_in_capacity,
            &scenario) == XRT_EXECUTION_COMPLETED);
    }
    for (uint32_t scenario = 0; scenario < 16; ++scenario) {
        StringAppendProbe probe = {.snapshot=(scenario & 1) != 0, .pinned=(scenario & 2) != 0,
            .self=(scenario & 4) != 0, .empty=(scenario & 8) != 0};
        XrtExecutionFailure failure = {0};
        CHECK(xrt_run_embedded(locations, &failure, native_string_append_root, &probe) == XRT_EXECUTION_COMPLETED);
        CHECK(probe.completed);
    }
}
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
static void native_string_append_failure_sweep(XrtLocationOwner *locations) {
    for (uint32_t aliases = 0; aliases < 2; ++aliases) {
        StringAppendProbe probe = {.snapshot=aliases != 0};
        XrtExecutionFailure failure = {0};
        native_array_calls = 0;
        CHECK(xrt_run_embedded(locations, &failure, native_string_append_root, &probe) == XRT_EXECUTION_COMPLETED);
        size_t points = native_array_calls;
        CHECK(points && !native_array_live && !native_array_bytes);
        for (size_t point = 0; point < points; ++point) {
            native_array_calls = 0; native_array_fail_at = point;
            native_array_injected = native_array_pending_failure = false;
            probe.completed = false;
            XrtValueRoot root = {0};
            CHECK(xrt_run_embedded_owned(locations, &failure, native_string_append_root, &probe, &root) == XRT_EXECUTION_FAILURE);
            CHECK(native_array_injected && !probe.completed && failure.kind == XRT_FAILURE_VALUE &&
                failure.detail.value.status == XR_XIR_VALUE_OOM && xrt_value_root_empty(&root));
            CHECK(!native_array_live && !native_array_bytes);
        }
        native_array_fail_at = SIZE_MAX;
        printf("String append aliases=%u allocation failure points: %zu\n", aliases, points);
    }
}
#endif
