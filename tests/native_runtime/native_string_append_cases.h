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
