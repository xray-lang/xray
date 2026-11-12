/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_array_ref_cases.h - Current binding aliases share frame and state writes
 */
#ifndef NATIVE_ARRAY_REF_CASES_H
#define NATIVE_ARRAY_REF_CASES_H
typedef struct ArrayRefProbe {
    XrtModuleState *state;
    XrtModuleTypeKind kind;
    XrXirDomain *pin;
    uint64_t baseline;
    unsigned failure;
    bool completed, retire_state_first;
} ArrayRefProbe;

static XrtArray *reference_array(XrtModuleTypeKind kind, int64_t seed) {
    switch (kind) {
    case XRT_MODULE_ARRAY_I64: return xrt_array_i64_repeat(1, seed, 1);
    case XRT_MODULE_ARRAY_U8: return xrt_array_u8_repeat(1, (uint8_t)seed, 1);
    case XRT_MODULE_ARRAY_U64: return xrt_array_u64_repeat(1, (uint64_t)seed, 1);
    default: {
        XrtString *text = xrt_string_i64(seed, 1);
        XrtArray *array = xrt_array_string_repeat(1, text, 1);
        xrt_string_drop(&text);
        return array;
    }
    }
}
static void reference_element(const XrtArray *array, XrtModuleTypeKind kind, int64_t index, int64_t expected) {
    if (kind == XRT_MODULE_ARRAY_STRING) {
        XrtString *text = xrt_array_string_get(array, index, 1);
        CHECK(xrt_string_length(text, 1) == 1);
        size_t count = 0;
        CHECK(xrt_string_bytes(text, &count)[0] == '0' + expected && count == 1);
        xrt_string_drop(&text);
    } else if (kind == XRT_MODULE_ARRAY_U8) CHECK(xrt_array_u8_get(array, index, 1) == expected);
    else if (kind == XRT_MODULE_ARRAY_U64) CHECK(xrt_array_u64_get(array, index, 1) == (uint64_t)expected);
    else CHECK(xrt_array_i64_get(array, index, 1) == expected);
}
static void reference_value(const XrtArray *array, XrtModuleTypeKind kind, int64_t expected) {
    CHECK(xrt_array_length(array, 1) == 1);
    reference_element(array, kind, 0, expected);
}
static void reference_push(XrtModuleTypeKind kind, XrtArrayRef reference, int64_t seed, uint32_t source) {
    if (kind == XRT_MODULE_ARRAY_I64) xrt_array_ref_i64_push(reference, seed, source);
    else if (kind == XRT_MODULE_ARRAY_U8) xrt_array_ref_u8_push(reference, (uint8_t)seed, source);
    else if (kind == XRT_MODULE_ARRAY_U64) xrt_array_ref_u64_push(reference, (uint64_t)seed, source);
    else {
        XrtString *text = xrt_string_i64(seed, source);
        xrt_array_ref_string_push(reference, text, source);
        xrt_string_drop(&text);
    }
}
static void reference_set(XrtModuleTypeKind kind, XrtArrayRef reference, int64_t index, int64_t seed) {
    if (kind == XRT_MODULE_ARRAY_I64) xrt_array_ref_i64_set(reference, index, seed, 1);
    else if (kind == XRT_MODULE_ARRAY_U8) xrt_array_ref_u8_set(reference, index, (uint8_t)seed, 1);
    else if (kind == XRT_MODULE_ARRAY_U64) xrt_array_ref_u64_set(reference, index, (uint64_t)seed, 1);
    else {
        XrtString *text = xrt_string_i64(seed, 1);
        xrt_array_ref_string_set(reference, index, text, 1);
        xrt_string_drop(&text);
    }
}
static void reference_state_new(ArrayRefProbe *probe) {
    XrtModuleTypeSpec type = {probe->kind, 0};
    XrtModuleSpec module = {0};
    XrtModuleSlotSpec slot = {.flags=probe->failure == 2 ? 0 : XRT_MODULE_SLOT_MUTABLE,
        .size=8, .alignment=8};
    XrtModuleLayoutSpec spec = {.types=&type, .type_count=1, .modules=&module, .module_count=1,
        .slots=&slot, .slot_count=1, .payload_size=8, .payload_alignment=8, .byte_limit=UINT64_MAX};
    XrtModuleLayout *layout = NULL;
    OK(xrt_module_layout_new(&spec, &layout));
    OK(xrt_module_state_new(layout, UINT64_MAX, &probe->state));
    xrt_module_layout_drop(&layout);
}
static void reference_root(void *opaque) {
    ArrayRefProbe *probe = opaque;
    OK(xrt_module_begin(probe->state, 0));
    XrtModuleAccess cache = {0};
    OK(xrt_module_cache(probe->state, 0, &cache));
    XrtModuleRef *slot = cache.reference;
    XrtArrayRef state = xrt_array_ref_state(slot, probe->kind, 1);
    if (probe->failure == 1) { (void)xrt_array_ref_read(state, 2); return; }
    if (probe->failure == 5) {
        XrtArray *unpublished = reference_array(probe->kind, 7);
        xrt_array_ref_write(state, unpublished, 2);
        return;
    }
    if (probe->failure == 6) { reference_push(probe->kind, state, 7, 2); return; }
    XrtValueRoot *root = xrt_value_root_current(1);
    probe->pin = xrt_value_root_domain(root, 1);
    CHECK(xr_xir_domain_retain(probe->pin));
    probe->baseline = xr_xir_domain_stats(probe->pin).live_bytes;
    XrtArray *original_value = reference_array(probe->kind, 4);
    XrtModuleRef *initializer = NULL;
    xrt_module_require(xrt_module_construct(probe->state, 0, &original_value, &initializer), 1);
    XrtArrayRef initial = xrt_array_ref_state(initializer, probe->kind, 1);
    XrtArray *initial_read = xrt_array_ref_read(initial, 1);
    reference_value(initial_read, probe->kind, 4); xrt_array_drop(&initial_read);
    OK(xrt_module_publish(initializer));
    OK(xrt_module_ready(probe->state, 0));
    XrtArray *local = xrt_array_retain(original_value, 1);
    XrtArrayRef cell = xrt_array_ref_local(&local, probe->kind, 1);
    if (probe->failure == 3) {
        XrtModuleTypeKind wrong = probe->kind == XRT_MODULE_ARRAY_STRING ? XRT_MODULE_ARRAY_I64 : XRT_MODULE_ARRAY_STRING;
        cell = xrt_array_ref_local(&local, wrong, 1);
        (void)xrt_array_ref_read(cell, 2);
        return;
    }
    XrtArray *next = reference_array(probe->kind, 9);
    if (probe->failure == 4) {
        XrtModuleTypeKind wrong = probe->kind == XRT_MODULE_ARRAY_STRING ? XRT_MODULE_ARRAY_I64 : XRT_MODULE_ARRAY_STRING;
        XrtArray *bad = reference_array(wrong, 7);
        xrt_array_ref_write(state, bad, 2);
        return;
    }
    xrt_array_ref_write(cell, next, 1);
    XrtArray *read = xrt_array_ref_read(cell, 1);
    reference_value(read, probe->kind, 9); xrt_array_drop(&read);
    read = xrt_array_ref_read(state, 1);
    reference_value(read, probe->kind, 4); xrt_array_drop(&read);
    xrt_module_require(xrt_module_replace(slot, &next), 1);
    XrtArrayRef alias = state;
    read = xrt_array_ref_read(alias, 1);
    reference_value(read, probe->kind, 9); xrt_array_drop(&read);
    xrt_array_ref_write(alias, original_value, 1);
    const void *payload = NULL;
    OK(xrt_module_borrow(slot, &payload));
    XrtArray *actual = NULL;
    memcpy(&actual, payload, sizeof(actual));
    reference_value(actual, probe->kind, 4);
    reference_value(original_value, probe->kind, 4);
    reference_push(probe->kind, cell, 5, 1);
    CHECK(xrt_array_length(local, 1) == 2);
    reference_element(local, probe->kind, 1, 5);
    reference_value(next, probe->kind, 9);
    reference_push(probe->kind, state, 7, 1);
    read = xrt_array_ref_read(state, 1);
    CHECK(xrt_array_length(read, 1) == 2);
    reference_element(read, probe->kind, 0, 4);
    reference_set(probe->kind, state, 0, 8);
    OK(xrt_module_borrow(slot, &payload));
    memcpy(&actual, payload, sizeof(actual));
    reference_element(actual, probe->kind, 0, 8);
    reference_element(actual, probe->kind, 1, 7);
    reference_element(read, probe->kind, 0, 4);
    xrt_array_drop(&read);
    reference_value(original_value, probe->kind, 4);
    if (probe->retire_state_first) {
        read = xrt_array_ref_read(state, 1);
        xrt_module_state_drop(&probe->state);
        XrtArrayRef surviving = xrt_array_ref_local(&read, probe->kind, 1);
        reference_push(probe->kind, surviving, 9, 1);
        CHECK(xrt_array_length(read, 1) == 3);
        reference_element(read, probe->kind, 0, 8);
        reference_element(read, probe->kind, 2, 9);
        xrt_array_drop(&read);
    }
    xrt_array_drop(&next); xrt_array_drop(&local); xrt_array_drop(&original_value);
    probe->completed = true;
}
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
typedef struct ArrayRefMutationProbe {
    ArrayRefProbe fixture;
    XrtArray *saved;
    bool push;
} ArrayRefMutationProbe;
static void reference_hold_root(void *opaque) {
    ArrayRefMutationProbe *probe = opaque;
    XrtModuleAccess cache = {0};
    OK(xrt_module_cache(probe->fixture.state, 0, &cache));
    XrtArrayRef reference = xrt_array_ref_state(cache.reference, probe->fixture.kind, 1);
    probe->saved = xrt_array_root_take(xrt_array_ref_read(reference, 1));
}
static void reference_mutation_root(void *opaque) {
    ArrayRefMutationProbe *probe = opaque;
    XrtModuleAccess cache = {0};
    OK(xrt_module_cache(probe->fixture.state, 0, &cache));
    XrtArrayRef reference = xrt_array_ref_state(cache.reference, probe->fixture.kind, 1);
    if (probe->push) reference_push(probe->fixture.kind, reference, 9, 1);
    else reference_set(probe->fixture.kind, reference, 0, 9);
}
static void reference_check_saved(void *opaque) {
    ArrayRefMutationProbe *probe = opaque;
    CHECK(xrt_array_length(probe->saved, 1) == 2);
    reference_element(probe->saved, probe->fixture.kind, 0, 8);
    reference_element(probe->saved, probe->fixture.kind, 1, 7);
}
static void reference_mutation_setup(ArrayRefMutationProbe *probe, XrtLocationOwner *owner) {
    reference_state_new(&probe->fixture);
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded(owner, &failure, reference_root, &probe->fixture) == XRT_EXECUTION_COMPLETED);
    CHECK(xrt_run_embedded(owner, &failure, reference_hold_root, probe) == XRT_EXECUTION_COMPLETED);
}
static void reference_mutation_dispose(ArrayRefMutationProbe *probe) {
    xrt_module_state_drop(&probe->fixture.state);
    xrt_array_owned_drop(&probe->saved);
    CHECK(xr_xir_domain_stats(probe->fixture.pin).live_bytes == probe->fixture.baseline);
    xr_xir_domain_drop(probe->fixture.pin);
    CHECK(!native_array_live && !native_array_bytes);
}
static void reference_mutation_sweep(XrtLocationOwner *owner, XrtModuleTypeKind kind, bool push) {
    ArrayRefMutationProbe probe = {.fixture={.kind=kind}, .push=push};
    reference_mutation_setup(&probe, owner);
    native_array_calls = 0;
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded(owner, &failure, reference_mutation_root, &probe) == XRT_EXECUTION_COMPLETED);
    size_t points = native_array_calls;
    CHECK(xrt_run_embedded(owner, &failure, reference_check_saved, &probe) == XRT_EXECUTION_COMPLETED);
    reference_mutation_dispose(&probe);
    CHECK(points > 0);
    for (size_t point = 0; point < points; ++point) {
        probe = (ArrayRefMutationProbe){.fixture={.kind=kind}, .push=push};
        reference_mutation_setup(&probe, owner);
        native_array_calls = 0; native_array_fail_at = point; native_array_injected = false;
        XrtValueRoot root = {0};
        CHECK(xrt_run_embedded_owned(owner, &failure, reference_mutation_root, &probe, &root) == XRT_EXECUTION_FAILURE);
        CHECK(native_array_injected && failure.kind == XRT_FAILURE_VALUE &&
            failure.detail.value.status == XR_XIR_VALUE_OOM && xrt_value_root_empty(&root));
        native_array_fail_at = SIZE_MAX;
        XrtModuleAccess cache = {0};
        OK(xrt_module_cache(probe.fixture.state, 0, &cache));
        const void *payload = NULL;
        OK(xrt_module_borrow(cache.reference, &payload));
        XrtArray *unchanged = NULL;
        memcpy(&unchanged, payload, sizeof(unchanged));
        CHECK(unchanged == probe.saved);
        CHECK(xrt_run_embedded(owner, &failure, reference_check_saved, &probe) == XRT_EXECUTION_COMPLETED);
        xrt_module_state_drop(&probe.fixture.state);
        CHECK(xrt_run_embedded(owner, &failure, reference_check_saved, &probe) == XRT_EXECUTION_COMPLETED);
        reference_mutation_dispose(&probe);
    }
    printf("Array Ref %s %s mutation failure points: %zu\n",
        kind == XRT_MODULE_ARRAY_STRING ? "String" : "i64", push ? "push" : "set", points);
}
static void reference_allocation_sweep(XrtLocationOwner *owner, XrtModuleTypeKind kind) {
    ArrayRefProbe probe = {.kind=kind};
    reference_state_new(&probe);
    native_array_calls = 0;
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded(owner, &failure, reference_root, &probe) == XRT_EXECUTION_COMPLETED && probe.completed);
    size_t points = native_array_calls;
    xrt_module_state_drop(&probe.state);
    xr_xir_domain_drop(probe.pin);
    CHECK(!native_array_live && !native_array_bytes);
    for (size_t point = 0; point < points; ++point) {
        native_array_fail_at = SIZE_MAX;
        probe = (ArrayRefProbe){.kind=kind};
        reference_state_new(&probe);
        native_array_calls = 0; native_array_fail_at = point; native_array_injected = false;
        XrtValueRoot root = {0};
        CHECK(xrt_run_embedded_owned(owner, &failure, reference_root, &probe, &root) == XRT_EXECUTION_FAILURE);
        CHECK(native_array_injected && failure.kind == XRT_FAILURE_VALUE &&
            failure.detail.value.status == XR_XIR_VALUE_OOM && !probe.completed && xrt_value_root_empty(&root));
        xrt_module_state_drop(&probe.state);
        xr_xir_domain_drop(probe.pin);
        CHECK(!native_array_live && !native_array_bytes);
    }
    native_array_fail_at = SIZE_MAX;
    printf("Array reference %s allocation failure points: %zu\n", kind == XRT_MODULE_ARRAY_STRING ? "String" : "i64", points);
}
#endif
static void array_reference_cases(void) {
    XrtSourceLocation sources[] = {{"array-ref.xr", 12, 3, 1, 3, 9}, {"array-ref.xr", 12, 7, 2, 7, 9}};
    XrtLocationOwner *owner = NULL;
    CHECK(xrt_locations_create(sources, 2, &owner) == XRT_LOCATION_OK);
    const XrtModuleTypeKind kinds[] = {XRT_MODULE_ARRAY_I64, XRT_MODULE_ARRAY_STRING,
        XRT_MODULE_ARRAY_U8, XRT_MODULE_ARRAY_U64};
    for (uint32_t family = 0; family < 4; ++family) {
        for (unsigned mode = 0; mode <= 7; ++mode) {
            unsigned failure_mode = mode == 7 ? 0 : mode;
            ArrayRefProbe probe = {.kind=kinds[family], .failure=failure_mode, .retire_state_first=mode == 7};
            reference_state_new(&probe);
            XrtExecutionFailure failure = {0};
            XrtValueRoot root = {0};
            XrtExecutionStatus status = xrt_run_embedded_owned(owner, &failure, reference_root, &probe, &root);
            CHECK(status == (failure_mode ? XRT_EXECUTION_FAILURE : XRT_EXECUTION_COMPLETED));
            CHECK(probe.completed == !failure_mode && xrt_value_root_empty(&root));
            if (failure_mode) CHECK(failure.kind == XRT_FAILURE_VALUE &&
                failure.detail.value.status == XR_XIR_VALUE_BAD_ARGUMENT &&
                failure.detail.value.source_id == (mode == 2 ? 1u : 2u));
            xrt_module_state_drop(&probe.state);
            if (probe.pin) {
                CHECK(xr_xir_domain_stats(probe.pin).live_bytes == probe.baseline);
                xr_xir_domain_drop(probe.pin);
            }
        }
    }
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
    reference_allocation_sweep(owner, XRT_MODULE_ARRAY_I64);
    reference_allocation_sweep(owner, XRT_MODULE_ARRAY_STRING);
    reference_mutation_sweep(owner, XRT_MODULE_ARRAY_I64, false);
    reference_mutation_sweep(owner, XRT_MODULE_ARRAY_I64, true);
    reference_mutation_sweep(owner, XRT_MODULE_ARRAY_STRING, false);
    reference_mutation_sweep(owner, XRT_MODULE_ARRAY_STRING, true);
#endif
    xrt_locations_release(owner);
}
#endif // NATIVE_ARRAY_REF_CASES_H
