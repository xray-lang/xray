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
    XrtString *initial_value = value;
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
        CHECK(value == initial_value && xir_object_references(&value->object) == 1);
        CHECK(after.allocations == before.allocations && after.reallocations == before.reallocations &&
            after.live_bytes == before.live_bytes);
        value = xrt_string_append_owned(value, value, 1);
        CHECK(value == initial_value);
        string_equals(value, "axax", 4);
    } else {
        CHECK(value != initial_value);
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
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
static XrtString **string_append_binding, **string_append_replacement;
static bool string_append_reservation_observed;
static void string_append_reservation_callback(void) {
    xrt_string_drop(string_append_binding);
    xrt_string_move(string_append_binding, string_append_replacement);
    string_append_reservation_observed = true;
}
static void native_string_append_after_reservation(void *opaque) {
    (void)opaque;
    XrtString *value = xrt_string_new("old", 3, 1);
    XrtString *replacement = xrt_string_new("new", 3, 1);
    const char *bytes = " plus a suffix that exceeds the inline capacity";
    XrtString *suffix = xrt_string_literal(bytes, strlen(bytes), 1);
    XrtString *padding[5];
    for (size_t i = 0; i < 5; ++i) padding[i] = xrt_string_new("pad", 3, 1);
    XrtValueRoot *root = xrt_value_root_current(1);
    CHECK(root->table_capacity == 16 && root->table_count == 8);
    string_append_binding = &value;
    string_append_replacement = &replacement;
    string_append_reservation_observed = false;
    native_array_before_allocate = string_append_reservation_callback;
    xrt_string_cell_append(&value, suffix, 1);
    CHECK(string_append_reservation_observed && !replacement && !native_array_before_allocate);
    static const char expected[] = "new plus a suffix that exceeds the inline capacity";
    string_equals(value, expected, sizeof(expected) - 1);
    string_append_binding = string_append_replacement = NULL;
    for (size_t i = 0; i < 5; ++i) xrt_string_drop(&padding[i]);
    xrt_string_drop(&value);
}
#endif
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
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
    XrtExecutionFailure callback_failure = {0};
    CHECK(xrt_run_embedded(locations, &callback_failure, native_string_append_after_reservation,
        NULL) == XRT_EXECUTION_COMPLETED);
#endif
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
