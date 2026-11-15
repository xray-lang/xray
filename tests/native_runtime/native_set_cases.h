/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_set_cases.h - Canonical Set bindings, COW and owner retirement
 */
#include "runtime/native/xnative_set.h"
typedef struct NativeSetProbe {
    XrtModuleState *state;
    XrtSet *saved;
    XrXirDomain *domain;
    uint64_t baseline;
    bool completed;
} NativeSetProbe;
static XrtSet *native_set_create(void) {
    const int64_t values[] = {1, 7, 1};
    return xrt_set_i64_new(values, 3, 1);
}
static void native_set_initialize(void *opaque) {
    NativeSetProbe *probe = opaque;
    XrtValueRoot *root = xrt_value_root_current(1);
    probe->domain = xrt_value_root_domain(root, 1);
    CHECK(xr_xir_domain_retain(probe->domain));
    probe->baseline = xr_xir_domain_stats(probe->domain).live_bytes;
    XrtSet *set = native_set_create();
    CHECK(xrt_set_length(set, 1) == 2 && xrt_set_i64_contains(set, 1, 1));
    probe->saved = xrt_set_root_take(xrt_set_retain(set, 1));
    OK(xrt_module_begin(probe->state, 0));
    XrtModuleRef *reference = NULL;
    OK(xrt_module_construct(probe->state, 0, &set, &reference));
    OK(xrt_module_publish(reference));
    OK(xrt_module_ready(probe->state, 0));
    xrt_set_drop(&set);
}
static void native_set_second_root(void *opaque) {
    NativeSetProbe *probe = opaque;
    XrtModuleRef *reference = NULL;
    OK(xrt_module_reference(probe->state, 0, &reference));
    XrtSetRef cap = xrt_set_ref_state(reference, 1);
    XrtSet *before = xrt_set_ref_read(cap, 1);
    xrt_set_ref_i64_add(cap, 99, 1);
    CHECK(!xrt_set_i64_contains(before, 99, 1));
    CHECK(!xrt_set_i64_contains(probe->saved, 99, 1));
    XrtSet *after = xrt_set_ref_read(cap, 1);
    CHECK(after != before && xrt_set_i64_contains(after, 99, 1));
    CHECK(xrt_set_ref_i64_remove(cap, 99, 1));
    CHECK(!xrt_set_ref_i64_remove(cap, 99, 1));
    xrt_set_ref_clear(cap, 1);
    XrtSet *empty = xrt_set_ref_read(cap, 1);
    CHECK(xrt_set_length(empty, 1) == 0);
    xrt_set_ref_write(cap, probe->saved, 1);
    XrtSet *restored = xrt_set_ref_read(cap, 1);
    xrt_module_state_drop(&probe->state);
    CHECK(xrt_set_length(restored, 1) == 2 && xrt_set_i64_contains(restored, 7, 1));
    xrt_set_drop(&before); xrt_set_drop(&after); xrt_set_drop(&empty); xrt_set_drop(&restored);
    probe->completed = true;
}
static void native_set_local_root(void *opaque) {
    bool *completed = opaque;
    int64_t values[] = {INT64_MIN, 7, INT64_MIN};
    XrtSet *set = xrt_set_i64_new(values, 3, 1);
    values[0] = 99;
    CHECK(xrt_set_length(set, 1) == 2 && xrt_set_i64_contains(set, INT64_MIN, 1));
    CHECK(!xrt_set_i64_contains(set, 99, 1));
    XrtSet *old = xrt_set_retain(set, 1);
    XrtSetRef ref = xrt_set_ref_local(&set, 1);
    xrt_set_ref_i64_add(ref, INT64_MAX, 1);
    CHECK(!xrt_set_i64_contains(old, INT64_MAX, 1));
    CHECK(xrt_set_i64_contains(set, INT64_MAX, 1));
    xrt_set_ref_i64_add(ref, INT64_MAX, 1);
    CHECK(xrt_set_length(set, 1) == 3);
    CHECK(xrt_set_i64_remove(&set, INT64_MIN, 1));
    CHECK(!xrt_set_i64_remove(&set, INT64_MIN, 1));
    xrt_set_clear(&set, 1);
    CHECK(!xrt_set_length(set, 1));
    xrt_set_ref_write(ref, old, 1);
    CHECK(xrt_set_length(set, 1) == 2);
    XrtSet *empty = xrt_set_i64_new(NULL, 0, 1);
    CHECK(!xrt_set_length(empty, 1) && !xrt_set_i64_contains(empty, 0, 1));
    xrt_set_clear(&empty, 1);
    xrt_set_drop(&empty); xrt_set_drop(&set); xrt_set_drop(&old);
    *completed = true;
}
static void native_set_bad_scalar(void *opaque) {
    (void)xrt_set_retain((const XrtSet *)opaque, 1);
}
static void native_set_bad_map(void *opaque) {
    (void)opaque;
    XrtMap *map = native_map_create();
    (void)xrt_set_retain((const XrtSet *)map, 1);
}
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
typedef struct NativeSetRetirement { XrtSet *value, *previous; bool observed; } NativeSetRetirement;
static NativeSetRetirement *native_set_retirement;
static void native_set_observe_retirement(void) {
    NativeSetRetirement *probe = native_set_retirement;
    if (probe->value == probe->previous) return;
    native_array_before_free = NULL;
    XrtSet *taken = xrt_set_root_take(probe->value);
    XrtSet *restored = xrt_set_retain(taken, 1);
    xrt_set_owned_drop(&taken);
    CHECK(restored == probe->value);
    probe->observed = true;
}
static void native_set_export_unique(void *opaque) {
    NativeSetRetirement *probe = opaque;
    probe->value = xrt_set_root_take(native_set_create());
}
static void native_set_foreign_growth(void *opaque) {
    NativeSetRetirement *probe = opaque;
    XrtSet *external = probe->value;
    probe->value = xrt_set_retain(external, 1);
    xrt_set_owned_drop(&external);
    probe->previous = probe->value;
    CHECK(atomic_load(&((XirObject *)probe->value)->references) == 1);
    native_set_retirement = probe;
    native_array_before_free = native_set_observe_retirement;
    for (int64_t n = 10; n < 18; ++n) xrt_set_i64_add(&probe->value, n, 1);
    native_array_before_free = NULL;
    native_set_retirement = NULL;
    CHECK(probe->observed && xrt_set_length(probe->value, 1) == 10);
    xrt_set_drop(&probe->value);
}
static void native_set_state_mutation(void *opaque) {
    NativeSetProbe *probe = opaque;
    XrtModuleRef *reference = NULL;
    OK(xrt_module_reference(probe->state, 0, &reference));
    xrt_set_ref_i64_add(xrt_set_ref_state(reference, 1), 99, 1);
    probe->completed = true;
}
static void native_set_state_failures(XrtLocationOwner *locations, NativeSetProbe *probe) {
    XrtExecutionFailure failure = {0};
    XrtModuleRef *reference = NULL;
    OK(xrt_module_reference(probe->state, 0, &reference));
    size_t start = native_array_calls;
    CHECK(xrt_run_embedded(locations, &failure, native_set_state_mutation, probe) == XRT_EXECUTION_COMPLETED);
    size_t count = native_array_calls - start;
    OK(xrt_module_replace(reference, &probe->saved));
    size_t live = native_array_live, bytes = native_array_bytes;
    CHECK(count);
    for (size_t n = 0; n < count; ++n) {
        native_array_fail_at = native_array_calls + n;
        native_array_injected = false; probe->completed = false;
        CHECK(xrt_run_embedded(locations, &failure, native_set_state_mutation, probe) == XRT_EXECUTION_FAILURE);
        CHECK(native_array_injected && !probe->completed && failure.kind == XRT_FAILURE_VALUE &&
            failure.detail.value.status == XR_XIR_VALUE_OOM);
        native_array_fail_at = SIZE_MAX;
        const void *payload = NULL;
        OK(xrt_module_borrow(reference, &payload));
        XrtSet *actual = NULL; memcpy(&actual, payload, sizeof(actual));
        CHECK(actual == probe->saved && native_array_live == live && native_array_bytes == bytes);
        CHECK(xrt_run_embedded(locations, &failure, native_set_state_mutation, probe) == XRT_EXECUTION_COMPLETED && probe->completed);
        OK(xrt_module_replace(reference, &probe->saved));
        CHECK(native_array_live == live && native_array_bytes == bytes);
    }
    printf("Set State COW %zu allocation failures preserve binding and retry\n", count);
}
#endif
static void native_set_cases(XrtLocationOwner *locations) {
    XrtExecutionFailure failure = {0};
    XirObject scalar = {.arena=(XrXirTypeArena *)(uintptr_t)1, .type=XR_XIR_I64};
    CHECK(xrt_run_embedded(locations, &failure, native_set_bad_scalar, &scalar) == XRT_EXECUTION_FAILURE);
    CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xrt_run_embedded(locations, &failure, native_set_bad_map, NULL) == XRT_EXECUTION_FAILURE);
    CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_BAD_ARGUMENT);
    NativeSetProbe probe = {0};
    XrtModuleTypeSpec type = {XRT_MODULE_SET_I64, 0};
    XrtModuleSpec module = {0};
    XrtModuleSlotSpec slot = {.flags=XRT_MODULE_SLOT_MUTABLE, .size=8, .alignment=8};
    XrtModuleLayoutSpec spec = {.metadata=XRT_MODULE_LAYOUT_HEADER,.types=&type, .type_count=1, .modules=&module, .module_count=1,
        .slots=&slot, .slot_count=1, .payload_size=8, .payload_alignment=8, .byte_limit=UINT64_MAX};
    XrtModuleLayout *layout = NULL;
    OK(xrt_module_layout_new(&spec, &layout));
    OK(xrt_module_state_new(layout, UINT64_MAX, &probe.state));
    xrt_module_layout_drop(&layout);
    CHECK(xrt_run_embedded(locations, &failure, native_set_initialize, &probe) == XRT_EXECUTION_COMPLETED);
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
    native_set_state_failures(locations, &probe);
#endif
    CHECK(xrt_run_embedded(locations, &failure, native_set_second_root, &probe) == XRT_EXECUTION_COMPLETED && probe.completed);
    xrt_set_owned_drop(&probe.saved);
    CHECK(xr_xir_domain_stats(probe.domain).live_bytes == probe.baseline);
    xr_xir_domain_drop(probe.domain);
    bool completed = false;
    CHECK(xrt_run_embedded(locations, &failure, native_set_local_root, &completed) == XRT_EXECUTION_COMPLETED && completed);
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
    NativeSetRetirement retirement = {0};
    CHECK(xrt_run_embedded(locations, &failure, native_set_export_unique, &retirement) == XRT_EXECUTION_COMPLETED);
    CHECK(xrt_run_embedded(locations, &failure, native_set_foreign_growth, &retirement) == XRT_EXECUTION_COMPLETED);
    CHECK(retirement.observed && !retirement.value && !native_array_live && !native_array_bytes);
    size_t start = native_array_calls;
    completed = false;
    CHECK(xrt_run_embedded(locations, &failure, native_set_local_root, &completed) == XRT_EXECUTION_COMPLETED && completed);
    size_t count = native_array_calls - start;
    CHECK(count && !native_array_live && !native_array_bytes);
    for (size_t n = 0; n < count; ++n) {
        native_array_fail_at = native_array_calls + n;
        native_array_injected = false; completed = false;
        CHECK(xrt_run_embedded(locations, &failure, native_set_local_root, &completed) == XRT_EXECUTION_FAILURE);
        CHECK(native_array_injected && !completed && failure.kind == XRT_FAILURE_VALUE &&
            failure.detail.value.status == XR_XIR_VALUE_OOM && !native_array_live && !native_array_bytes);
        native_array_fail_at = SIZE_MAX;
    }
    printf("Set canonical mutation and registry %zu allocation failures drained\n", count);
#endif
    puts("Set actual State binding, duplicate values, COW aliases and two Root retirement orders passed");
}
