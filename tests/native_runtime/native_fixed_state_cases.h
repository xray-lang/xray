/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_fixed_state_cases.h - Inline byte payload ownership and transactions
 */
static XrtModuleStatus fixed_state_layout(uint32_t count, bool mutable, XrtModuleLayout **out) {
    XrtModuleTypeSpec type = {XRT_MODULE_FIXED_U8, count};
    XrtModuleSpec module = {0};
    XrtModuleSlotSpec slot = {.flags=mutable ? XRT_MODULE_SLOT_MUTABLE : 0,
        .size=count, .alignment=1};
    XrtModuleLayoutSpec spec = {.types=&type, .type_count=1, .modules=&module,
        .module_count=1, .slots=&slot, .slot_count=1, .payload_size=count,
        .payload_alignment=1, .byte_limit=UINT64_MAX};
    return xrt_module_layout_new(&spec, out);
}

static void fixed_state_values(void) {
    const uint32_t counts[] = {1, 4, 16, 17, 256, 65535};
    for (size_t n = 0; n < sizeof(counts) / sizeof(*counts); ++n) {
        uint32_t count = counts[n];
        uint8_t *source = xr_malloc(count), *snapshot = xr_malloc(count);
        CHECK(source && snapshot);
        for (uint32_t i = 0; i < count; ++i) source[i] = (uint8_t)(128u + 37u * i);
        memcpy(snapshot, source, count);
        XrtModuleLayout *layout = NULL;
        XrtModuleState *state = NULL;
        OK(fixed_state_layout(count, true, &layout));
        XrtModuleLayoutSpec owned;
        OK(xrt_module_layout_view(layout, &owned));
        CHECK(owned.types[0].kind == XRT_MODULE_FIXED_U8 && owned.types[0].fixed_count == count);
        CHECK(owned.slots[0].size == count && owned.slots[0].alignment == 1 &&
            owned.payload_size == count);
        OK(xrt_module_state_new(layout, UINT64_MAX, &state));
        xrt_module_layout_drop(&layout);
        XrtModuleAccess access;
        OK(xrt_module_cache(state, 0, &access));
        CHECK(access.kind == XRT_MODULE_FIXED_U8 && access.size == count &&
            access.alignment == 1 && access.is_mutable && !*access.flags);
        const void *payload = NULL;
        CHECK(xrt_module_borrow(access.reference, &payload) == XR_XIR_VALUE_BAD_ARGUMENT && !payload);
        OK(xrt_module_begin(state, 0));
        XrtModuleRef *initial = NULL;
        size_t before = calls;
        OK(xrt_module_construct(state, 0, source, &initial));
        CHECK(calls == before);
        memset(source, 49, count);
        OK(xrt_module_borrow(initial, &payload));
        CHECK(!memcmp(payload, snapshot, count));
        payload = NULL;
        CHECK(xrt_module_borrow(access.reference, &payload) == XR_XIR_VALUE_BAD_ARGUMENT && !payload);
        OK(xrt_module_publish(initial));
        OK(xrt_module_ready(state, 0));
        OK(xrt_module_borrow(access.reference, &payload));
        CHECK(!memcmp(payload, snapshot, count));
        before = calls;
        OK(xrt_module_replace(access.reference, source));
        CHECK(calls == before && !memcmp(access.payload, source, count));
        OK(xrt_module_replace(access.reference, access.payload));
        CHECK(calls == before && !memcmp(access.payload, source, count));
        CHECK(xrt_module_replace(access.reference, NULL) == XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(!memcmp(access.payload, source, count));
        xrt_module_state_drop(&state);
        for (uint32_t i = 0; i < count; ++i) CHECK(snapshot[i] == (uint8_t)(128u + 37u * i));
        xr_free(snapshot); xr_free(source);
        CHECK(!live && !state && !layout);
    }
    XrtModuleLayout *layout = NULL;
    XrtModuleState *state = NULL;
    uint8_t source = 7;
    OK(fixed_state_layout(1, false, &layout));
    OK(xrt_module_state_new(layout, UINT64_MAX, &state));
    OK(xrt_module_begin(state, 0));
    OK(xrt_module_initialize(state, 0, &source));
    XrtModuleRef *reference = NULL;
    OK(xrt_module_reference(state, 0, &reference));
    source = 9;
    CHECK(xrt_module_replace(reference, &source) == XR_XIR_VALUE_BAD_ARGUMENT);
    const void *payload = NULL;
    OK(xrt_module_borrow(reference, &payload));
    CHECK(*(const uint8_t *)payload == 7);
    xrt_module_layout_drop(&layout);
    xrt_module_state_drop(&state);
    CHECK(!live);
}

static void fixed_state_invalid_shapes(void) {
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        XrtModuleTypeSpec type = {XRT_MODULE_FIXED_U8, 16};
        XrtModuleSpec module = {0};
        XrtModuleSlotSpec slot = {.size=16, .alignment=1};
        XrtModuleLayoutSpec spec = {.types=&type, .type_count=1, .modules=&module,
            .module_count=1, .slots=&slot, .slot_count=1, .payload_size=16,
            .payload_alignment=2, .byte_limit=UINT64_MAX};
        if (scenario == 0) type.fixed_count = 0;
        if (scenario == 1) type.fixed_count = 65536;
        if (scenario == 2) type.fixed_count = UINT32_MAX;
        if (scenario == 3) type.kind = XRT_MODULE_I64;
        if (scenario == 4) type.kind = XRT_MODULE_TYPE_INVALID;
        if (scenario == 5) slot.size = 15;
        if (scenario == 6) slot.alignment = 2;
        if (scenario == 7) type.kind = (XrtModuleTypeKind)UINT32_MAX;
        XrtModuleLayout *layout = NULL;
        CHECK(xrt_module_layout_new(&spec, &layout) == XR_XIR_VALUE_BAD_ARGUMENT && !layout);
        CHECK(!live);
    }
}

static void fixed_state_allocation_failures(void) {
    for (size_t failure = 0; failure < 64; ++failure) {
        calls = 0; fail_at = failure; injected = false;
        XrtModuleLayout *layout = NULL;
        XrtModuleState *state = NULL;
        XrtModuleStatus status = fixed_state_layout(17, true, &layout);
        if (!status) status = xrt_module_state_new(layout, UINT64_MAX, &state);
        if (!status) {
            uint8_t bytes[17] = {0, 255, 128};
            status = xrt_module_begin(state, 0);
            XrtModuleRef *initial = NULL;
            if (!status) status = xrt_module_construct(state, 0, bytes, &initial);
            if (!status) status = xrt_module_publish(initial);
        }
        xrt_module_layout_drop(&layout);
        xrt_module_state_drop(&state);
        CHECK(!live && !layout && !state);
        if (!injected) {
            CHECK(status == XR_XIR_VALUE_OK);
            printf("Fixed byte module state verified %zu allocation sites\n", failure);
            break;
        }
        CHECK(status == XR_XIR_VALUE_OOM && failure != 63);
    }
    fail_at = SIZE_MAX;
}
