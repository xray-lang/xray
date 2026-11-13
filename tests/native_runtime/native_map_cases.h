/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_map_cases.h - Actual Map bindings survive COW and owner retirement
 *
 * KEY CONCEPT:
 *   Saved owners and actual State bindings expose COW publication failures.
 *   Each failure preserves the old value and leaves allocation accounting whole.
 */
#include "runtime/native/xnative_map.h"
typedef struct NativeMapProbe {
    XrtModuleState *state;
    XrtMap *saved;
    XrXirDomain *domain;
    uint64_t baseline;
    bool completed;
} NativeMapProbe;

static XrtMap *native_map_create(void) {
    XrtString *keys[] = {xrt_string_new("same", 4, 1), xrt_string_new("other", 5, 1),
        xrt_string_new("same", 4, 1)};
    int64_t values[] = {1, 7, 42};
    XrtMap *map = xrt_map_string_i64_new(keys, values, 3, 1);
    for (size_t n = 0; n < 3; ++n) xrt_string_drop(&keys[n]);
    return map;
}
static void native_map_value(const XrtMap *map, const char *text, size_t size, int64_t expected) {
    XrtString *key = xrt_string_new(text, size, 1);
    int64_t value = 0;
    CHECK(xrt_map_string_i64_get(map, key, &value, 1) && value == expected);
    xrt_string_drop(&key);
}
static void native_map_initialize(void *opaque) {
    NativeMapProbe *probe = opaque;
    XrtValueRoot *root = xrt_value_root_current(1);
    probe->domain = xrt_value_root_domain(root, 1);
    CHECK(xr_xir_domain_retain(probe->domain));
    probe->baseline = xr_xir_domain_stats(probe->domain).live_bytes;
    XrtMap *map = native_map_create();
    CHECK(xrt_map_length(map, 1) == 2);
    native_map_value(map, "same", 4, 42);
    probe->saved = xrt_map_root_take(xrt_map_retain(map, 1));
    OK(xrt_module_begin(probe->state, 0));
    XrtModuleRef *reference = NULL;
    OK(xrt_module_construct(probe->state, 0, &map, &reference));
    OK(xrt_module_publish(reference));
    OK(xrt_module_ready(probe->state, 0));
    xrt_map_drop(&map);
}
static void native_map_second_root(void *opaque) {
    NativeMapProbe *probe = opaque;
    XrtModuleRef *reference = NULL;
    OK(xrt_module_reference(probe->state, 0, &reference));
    XrtMapRef cap = xrt_map_ref_state(reference, 1);
    XrtMap *before = xrt_map_ref_read(cap, 1);
    XrtString *key = xrt_string_new("same", 4, 1);
    xrt_map_ref_string_i64_set(cap, key, 99, 1);
    native_map_value(before, "same", 4, 42);
    native_map_value(probe->saved, "same", 4, 42);
    XrtMap *after = xrt_map_ref_read(cap, 1);
    native_map_value(after, "same", 4, 99);
    CHECK(after != before);
    CHECK(xrt_map_ref_string_i64_remove(cap, key, 1));
    CHECK(!xrt_map_ref_string_i64_remove(cap, key, 1));
    xrt_map_ref_clear(cap, 1);
    XrtMap *empty = xrt_map_ref_read(cap, 1);
    CHECK(xrt_map_length(empty, 1) == 0);
    xrt_map_ref_write(cap, probe->saved, 1);
    XrtMap *restored = xrt_map_ref_read(cap, 1);
    native_map_value(restored, "same", 4, 42);
    xrt_module_state_drop(&probe->state);
    native_map_value(restored, "same", 4, 42);
    xrt_map_drop(&before); xrt_map_drop(&after); xrt_map_drop(&empty); xrt_map_drop(&restored);
    xrt_string_drop(&key);
    probe->completed = true;
}
static void native_map_failures_root(void *opaque) {
    bool *completed = opaque;
    XrtMap *map = native_map_create();
    XrtMap *old = xrt_map_retain(map, 1);
    XrtString *key = xrt_string_new("same", 4, 1);
    xrt_map_string_i64_set(&map, key, 99, 1);
    native_map_value(old, "same", 4, 42);
    native_map_value(map, "same", 4, 99);
    CHECK(xrt_map_string_i64_remove(&map, key, 1));
    xrt_map_clear(&map, 1);
    CHECK(!xrt_map_length(map, 1));
    xrt_map_assign(&map, old, 1);
    native_map_value(map, "same", 4, 42);
    xrt_map_drop(&map); xrt_map_drop(&old); xrt_string_drop(&key);
    *completed = true;
}
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
typedef struct NativeMapRetirement {
    XrtMap *value, *previous;
    bool observed;
} NativeMapRetirement;
static NativeMapRetirement *native_map_retirement;
static void native_map_observe_retirement(void) {
    NativeMapRetirement *probe = native_map_retirement;
    if (probe->value == probe->previous) return;
    native_array_before_free = NULL;
    XrtMap *taken = xrt_map_root_take(probe->value);
    XrtMap *restored = xrt_map_retain(taken, 1);
    xrt_map_owned_drop(&taken);
    CHECK(restored == probe->value);
    probe->observed = true;
}
static void native_map_export_unique(void *opaque) {
    NativeMapRetirement *probe = opaque;
    probe->value = xrt_map_root_take(native_map_create());
}
static void native_map_foreign_growth(void *opaque) {
    NativeMapRetirement *probe = opaque;
    XrtMap *external = probe->value;
    probe->value = xrt_map_retain(external, 1);
    xrt_map_owned_drop(&external);
    probe->previous = probe->value;
    CHECK(atomic_load(&((XirObject *)probe->value)->references) == 1);
    native_map_retirement = probe;
    native_array_before_free = native_map_observe_retirement;
    for (unsigned n = 0; n < 3; ++n) {
        char text = (char)('0' + n);
        XrtString *key = xrt_string_new(&text, 1, 1);
        xrt_map_string_i64_set(&probe->value, key, n, 1);
        xrt_string_drop(&key);
    }
    native_array_before_free = NULL;
    native_map_retirement = NULL;
    CHECK(probe->observed && xrt_map_length(probe->value, 1) == 5);
    xrt_map_drop(&probe->value);
}
static void native_map_retirement_callback(XrtLocationOwner *locations) {
    NativeMapRetirement probe = {0};
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded(locations, &failure, native_map_export_unique, &probe) == XRT_EXECUTION_COMPLETED);
    CHECK(xrt_run_embedded(locations, &failure, native_map_foreign_growth, &probe) == XRT_EXECUTION_COMPLETED);
    CHECK(probe.observed && !probe.value && !native_array_live && !native_array_bytes);
    puts("Map replacement registry is published before synchronous deallocator callbacks");
}
static void native_map_state_mutation(void *opaque) {
    NativeMapProbe *probe = opaque;
    XrtModuleRef *reference = NULL;
    OK(xrt_module_reference(probe->state, 0, &reference));
    XrtMapRef cap = xrt_map_ref_state(reference, 1);
    XrtString *key = xrt_string_new("same", 4, 1);
    xrt_map_ref_string_i64_set(cap, key, 99, 1);
    xrt_string_drop(&key);
    probe->completed = true;
}
static void native_map_state_failures(XrtLocationOwner *locations, NativeMapProbe *probe) {
    XrtExecutionFailure failure = {0};
    XrtModuleRef *reference = NULL;
    OK(xrt_module_reference(probe->state, 0, &reference));
    size_t start = native_array_calls;
    CHECK(xrt_run_embedded(locations, &failure, native_map_state_mutation, probe) == XRT_EXECUTION_COMPLETED);
    size_t count = native_array_calls - start;
    OK(xrt_module_replace(reference, &probe->saved));
    size_t live = native_array_live, bytes = native_array_bytes;
    for (size_t n = 0; n < count; ++n) {
        native_array_fail_at = native_array_calls + n;
        native_array_injected = false;
        probe->completed = false;
        CHECK(xrt_run_embedded(locations, &failure, native_map_state_mutation, probe) == XRT_EXECUTION_FAILURE);
        CHECK(native_array_injected && !probe->completed && failure.kind == XRT_FAILURE_VALUE &&
            failure.detail.value.status == XR_XIR_VALUE_OOM);
        native_array_fail_at = SIZE_MAX;
        const void *payload = NULL;
        OK(xrt_module_borrow(reference, &payload));
        XrtMap *actual = NULL;
        memcpy(&actual, payload, sizeof(actual));
        CHECK(actual == probe->saved && native_array_live == live && native_array_bytes == bytes);
        CHECK(xrt_run_embedded(locations, &failure, native_map_state_mutation, probe) == XRT_EXECUTION_COMPLETED && probe->completed);
        OK(xrt_module_replace(reference, &probe->saved));
        CHECK(native_array_live == live && native_array_bytes == bytes);
    }
    printf("Map State COW %zu allocation failures preserve actual binding and retry\n", count);
}
#endif

static void native_map_layout_budget(XrtModuleLayoutSpec spec) {
    uint64_t low = 0, high = UINT64_C(1048576);
    while (low < high) {
        uint64_t limit = low + (high - low) / 2;
        spec.byte_limit = limit;
        XrtModuleLayout *layout = NULL;
        XrtModuleStatus status = xrt_module_layout_new(&spec, &layout);
        if (status == XR_XIR_VALUE_OK) { xrt_module_layout_drop(&layout); high = limit; }
        else { CHECK(status == XR_XIR_VALUE_MEMORY_LIMIT && !layout); low = limit + 1; }
    }
    CHECK(low);
    XrtModuleLayout *layout = NULL;
    spec.byte_limit = low - 1;
    CHECK(xrt_module_layout_new(&spec, &layout) == XR_XIR_VALUE_MEMORY_LIMIT && !layout);
    spec.byte_limit = low;
    OK(xrt_module_layout_new(&spec, &layout));
    XrtModuleState *state = NULL;
    CHECK(xrt_module_state_new(layout, 0, &state) == XR_XIR_VALUE_MEMORY_LIMIT && !state);
    OK(xrt_module_state_new(layout, UINT64_MAX, &state));
    xrt_module_layout_drop(&layout);
    xrt_module_state_drop(&state);
}
#include "native_map_captured_cases.h"
static void native_map_cases(XrtLocationOwner *locations) {
    native_map_captured_cases(locations);
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
    native_map_retirement_callback(locations);
#endif
    NativeMapProbe probe = {0};
    XrtModuleTypeSpec type = {XRT_MODULE_MAP_STRING_I64, 0};
    XrtModuleSpec module = {0};
    XrtModuleSlotSpec slot = {.flags=XRT_MODULE_SLOT_MUTABLE, .size=8, .alignment=8};
    XrtModuleLayoutSpec spec = {.types=&type, .type_count=1, .modules=&module, .module_count=1,
        .slots=&slot, .slot_count=1, .payload_size=8, .payload_alignment=8, .byte_limit=UINT64_MAX};
    native_map_layout_budget(spec);
    XrtModuleLayout *layout = NULL;
    OK(xrt_module_layout_new(&spec, &layout));
    OK(xrt_module_state_new(layout, UINT64_MAX, &probe.state));
    xrt_module_layout_drop(&layout);
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded(locations, &failure, native_map_initialize, &probe) == XRT_EXECUTION_COMPLETED);
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
    native_map_state_failures(locations, &probe);
#endif
    CHECK(xrt_run_embedded(locations, &failure, native_map_second_root, &probe) == XRT_EXECUTION_COMPLETED && probe.completed);
    xrt_map_owned_drop(&probe.saved);
    CHECK(xr_xir_domain_stats(probe.domain).live_bytes == probe.baseline);
    xr_xir_domain_drop(probe.domain);
    bool completed = false;
    CHECK(xrt_run_embedded(locations, &failure, native_map_failures_root, &completed) == XRT_EXECUTION_COMPLETED && completed);
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
    CHECK(!native_array_live && !native_array_bytes);
    size_t start = native_array_calls;
    completed = false;
    CHECK(xrt_run_embedded(locations, &failure, native_map_failures_root, &completed) == XRT_EXECUTION_COMPLETED && completed);
    size_t count = native_array_calls - start;
    CHECK(count && !native_array_live && !native_array_bytes);
    for (size_t n = 0; n < count; ++n) {
        native_array_fail_at = native_array_calls + n;
        native_array_injected = false;
        completed = false;
        CHECK(xrt_run_embedded(locations, &failure, native_map_failures_root, &completed) == XRT_EXECUTION_FAILURE);
        CHECK(native_array_injected && !completed && failure.kind == XRT_FAILURE_VALUE &&
            failure.detail.value.status == XR_XIR_VALUE_OOM && !native_array_live && !native_array_bytes);
        native_array_fail_at = SIZE_MAX;
    }
    printf("Map canonical mutation and registry %zu allocation failures drained\n", count);
#endif
    puts("Map actual State binding, COW aliases and two Root retirement orders passed");
}
