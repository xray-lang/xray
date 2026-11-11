/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_array_unsigned_cases.h - Exact high-bit values and independently retained state
 */
#ifndef NATIVE_ARRAY_UNSIGNED_CASES_H
#define NATIVE_ARRAY_UNSIGNED_CASES_H

typedef struct UnsignedArrayProbe {
    XrtModuleState *state;
    XrtModuleRef *reference;
    bool wide, state_first, completed;
} UnsignedArrayProbe;

static uint64_t unsigned_get(const UnsignedArrayProbe *probe, const XrtArray *array, int64_t index) {
    return probe->wide ? xrt_array_u64_get(array, index, 1) : xrt_array_u8_get(array, index, 1);
}
static void unsigned_mutate(const UnsignedArrayProbe *probe, XrtArray **array) {
    if (probe->wide) {
        xrt_array_u64_set(array, 0, UINT64_C(0x8000000000000000), 1);
        xrt_array_u64_push(array, UINT64_MAX, 1);
    } else {
        xrt_array_u8_set(array, 0, 128, 1);
        xrt_array_u8_push(array, 255, 1);
    }
}
static void unsigned_root(void *opaque) {
    UnsignedArrayProbe *probe = opaque;
    const uint64_t wide[] = {0, UINT64_C(0x8000000000000000), UINT64_MAX};
    const uint8_t narrow[] = {0, 128, 255};
    uint64_t maximum = probe->wide ? UINT64_MAX : 255;
    XrtArray *array = probe->wide ? xrt_array_u64_new(wide, 3, 1) : xrt_array_u8_new(narrow, 3, 1);
    CHECK(unsigned_get(probe, array, 2) == maximum);
    xrt_module_require(xrt_module_begin(probe->state, 0), 1);
    xrt_module_require(xrt_module_construct(probe->state, 0, &array, &probe->reference), 1);
    xrt_module_require(xrt_module_publish(probe->reference), 1);
    xrt_module_require(xrt_module_ready(probe->state, 0), 1);
    if (probe->state_first) xrt_module_state_drop(&probe->state);
    unsigned_mutate(probe, &array);
    CHECK(xrt_array_length(array, 1) == 4 && unsigned_get(probe, array, 3) == maximum);
    XrtArray *repeated = probe->wide ? xrt_array_u64_repeat(64, maximum, 1) : xrt_array_u8_repeat(64, 255, 1);
    CHECK(xrt_array_length(repeated, 1) == 64 && unsigned_get(probe, repeated, 63) == maximum);
    xrt_array_drop(&repeated);
    repeated = probe->wide ? xrt_array_u64_repeat(0, maximum, 1) : xrt_array_u8_repeat(0, 255, 1);
    CHECK(xrt_array_length(repeated, 1) == 0);
    xrt_array_drop(&repeated);
    xrt_array_drop(&array);
    probe->completed = true;
}
static void unsigned_state_observe(void *opaque) {
    UnsignedArrayProbe *probe = opaque;
    const void *payload = NULL;
    OK(xrt_module_borrow(probe->reference, &payload));
    XrtArray *array;
    memcpy(&array, payload, sizeof(array));
    CHECK(xrt_array_length(array, 1) == 3 && unsigned_get(probe, array, 0) == 0);
    XrtArray *copy = xrt_array_retain(array, 1);
    xrt_module_state_drop(&probe->state);
    unsigned_mutate(probe, &copy);
    CHECK(xrt_array_length(copy, 1) == 4);
    xrt_array_drop(&copy);
}
static void unsigned_state_new(UnsignedArrayProbe *probe) {
    probe->reference = NULL;
    XrtModuleTypeSpec type = {probe->wide ? XRT_MODULE_ARRAY_U64 : XRT_MODULE_ARRAY_U8};
    XrtModuleSpec module = {0};
    XrtModuleSlotSpec slot = {.flags=XRT_MODULE_SLOT_MUTABLE, .size=sizeof(void *), .alignment=_Alignof(void *)};
    XrtModuleLayoutSpec spec = {.types=&type, .type_count=1, .modules=&module, .module_count=1,
        .slots=&slot, .slot_count=1, .payload_size=sizeof(void *), .payload_alignment=_Alignof(void *), .byte_limit=UINT64_MAX};
    XrtModuleLayout *layout = NULL;
    OK(xrt_module_layout_new(&spec, &layout));
    OK(xrt_module_state_new(layout, UINT64_MAX, &probe->state));
    xrt_module_layout_drop(&layout);
}
static void unsigned_retirement_cases(XrtLocationOwner *locations) {
    for (unsigned family = 0; family < 2; ++family) {
        for (unsigned order = 0; order < 2; ++order) {
            UnsignedArrayProbe probe = {.wide=family != 0, .state_first=order != 0};
            unsigned_state_new(&probe);
            XrtExecutionFailure failure = {0};
            CHECK(xrt_run_embedded(locations, &failure, unsigned_root, &probe) == XRT_EXECUTION_COMPLETED && probe.completed);
            if (!order) CHECK(xrt_run_embedded(locations, &failure, unsigned_state_observe, &probe) == XRT_EXECUTION_COMPLETED);
            CHECK(!probe.state);
        }
    }
}
static void unsigned_foreign_context(void) {
    for (unsigned wide = 0; wide < 2; ++wide) {
        XrXirType element = wide ? XR_XIR_U64 : XR_XIR_U8;
        XrXirTypeNode node = {.kind=XR_XIR_TYPE_ARRAY, .element=element};
        XrXirTypes types = {&node, 1, NULL, NULL};
        XrXirCompileContext context = {.limits=xr_xir_compile_default_limits()};
        XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
        CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
        XrXirTypeArena *arena = NULL, *receiver = NULL;
        OK(xr_xir_compile_type_arena_new(&context, &types, &arena));
        uint64_t bytes = 0, bits = wide ? UINT64_MAX : 255;
        OK(xrt_array_types_new(UINT64_MAX, &receiver, &bytes));
        XrXirDomain *domain = NULL;
        OK(xr_xir_domain_new(UINT64_MAX, &domain));
        uint64_t baseline = xr_xir_domain_stats(domain).live_bytes;
        XrXirValue fill = {.type=(uint32_t)element}, value = {0};
        memcpy(&fill.payload, &bits, sizeof(bits));
        XrXirValueAdmission admission = {arena, domain, NULL, NULL, UINT64_MAX, UINT64_MAX};
        OK(xr_xir_array_new((XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE, &fill, 1, &admission, &value));
        XrtArray *array = (XrtArray *)(intptr_t)value.payload, *copy = NULL;
        CHECK(xrt_array_owned_copy(array, receiver, XRT_ARRAY_I64, &copy) == XR_XIR_VALUE_BAD_ARGUMENT && !copy);
        CHECK(xrt_array_owned_copy(array, receiver, wide ? XRT_ARRAY_U8 : XRT_ARRAY_U64, &copy) == XR_XIR_VALUE_BAD_ARGUMENT && !copy);
        OK(xrt_array_owned_copy(array, receiver, wide ? XRT_ARRAY_U64 : XRT_ARRAY_U8, &copy));
        xr_xir_value_drop(&value);
        xr_xir_compile_type_arena_drop(arena); xr_xir_compile_type_arena_drop(receiver);
        CHECK(xr_xir_domain_stats(domain).live_bytes > baseline);
        xrt_array_owned_drop(&copy);
        CHECK(xr_xir_domain_stats(domain).live_bytes == baseline);
        xr_xir_domain_drop(domain); xr_compile_resources_release(context.resources);
    }
}
typedef struct UnsignedArrayFailure { bool wide; uint32_t operation; } UnsignedArrayFailure;
static void unsigned_failure_root(void *opaque) {
    UnsignedArrayFailure *probe = opaque;
    if (!probe->operation) {
        if (probe->wide) (void)xrt_array_u64_repeat(-1, UINT64_MAX, 1);
        else (void)xrt_array_u8_repeat(-1, 255, 1);
    } else {
        XrtArray *array = probe->wide ? xrt_array_u64_repeat(3, UINT64_MAX, 1) : xrt_array_u8_repeat(3, 255, 1);
        if (probe->operation == 1) (void)xrt_array_i64_get(array, 0, 1);
        else if (probe->wide) (void)xrt_array_u64_get(array, probe->operation == 2 ? -1 : 3, 1);
        else (void)xrt_array_u8_get(array, probe->operation == 2 ? -1 : 3, 1);
    }
    CHECK(false);
}
static void unsigned_failure_cases(XrtLocationOwner *locations) {
    for (unsigned family = 0; family < 2; ++family) {
        for (uint32_t operation = 0; operation < 4; ++operation) {
            UnsignedArrayFailure probe = {.wide=family != 0, .operation=operation};
            XrtExecutionFailure failure = {0};
            XrtValueRoot root = {0};
            CHECK(xrt_run_embedded_owned(locations, &failure, unsigned_failure_root, &probe, &root) == XRT_EXECUTION_FAILURE);
            CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status ==
                (operation < 2 ? XR_XIR_VALUE_BAD_ARGUMENT : XR_XIR_VALUE_BOUNDS));
            CHECK(failure.detail.value.source_id == 1 && xrt_value_root_empty(&root));
        }
    }
}
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
static void unsigned_allocation_sweep(XrtLocationOwner *locations, bool wide) {
    UnsignedArrayProbe probe = {.wide=wide};
    unsigned_state_new(&probe);
    size_t live = native_array_live, bytes = native_array_bytes;
    native_array_calls = 0;
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded(locations, &failure, unsigned_root, &probe) == XRT_EXECUTION_COMPLETED && probe.completed);
    size_t points = native_array_calls;
    xrt_module_state_drop(&probe.state);
    CHECK(!native_array_live && !native_array_bytes);
    for (size_t point = 0; point < points; ++point) {
        native_array_fail_at = SIZE_MAX;
        probe.completed = false;
        unsigned_state_new(&probe);
        CHECK(native_array_live == live && native_array_bytes == bytes);
        native_array_calls = 0; native_array_fail_at = point; native_array_injected = false;
        XrtValueRoot root = {0};
        CHECK(xrt_run_embedded_owned(locations, &failure, unsigned_root, &probe, &root) == XRT_EXECUTION_FAILURE);
        CHECK(native_array_injected && failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_OOM);
        CHECK(!probe.completed && xrt_value_root_empty(&root));
        xrt_module_state_drop(&probe.state);
        CHECK(!native_array_live && !native_array_bytes);
    }
    native_array_fail_at = SIZE_MAX;
    printf("Array %s runtime allocation failure points: %zu\n", wide ? "u64" : "u8", points);
}
#endif
#endif // NATIVE_ARRAY_UNSIGNED_CASES_H
