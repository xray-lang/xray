/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_nullable_state_cases.h - Owned nested payloads and atomic replacement
 */
static XrtModuleStatus nullable_state_layout(XrtModuleLayout **output) {
    XrtModuleTypeSpec types[] = {{XRT_MODULE_I64, 0}, {XRT_MODULE_NULLABLE, 0},
        {XRT_MODULE_NULLABLE, 1}, {XRT_MODULE_ARRAY_I64, 0}};
    XrtModuleSpec module = {0};
    XrtModuleSlotSpec slot = {.type=2, .flags=XRT_MODULE_SLOT_MUTABLE, .size=24, .alignment=8};
    XrtModuleLayoutSpec spec = {.types=types, .type_count=4, .modules=&module, .module_count=1,
        .slots=&slot, .slot_count=1, .payload_size=24, .payload_alignment=8, .byte_limit=UINT64_MAX};
    return xrt_module_layout_new(&spec, output);
}

static void nullable_state_values(void) {
    XrtModuleLayout *layout = NULL;
    OK(nullable_state_layout(&layout));
    CHECK(!layout->array_types);
    XrtModuleState *state = NULL;
    OK(xrt_module_state_new(layout, UINT64_MAX, &state));
    xrt_module_layout_drop(&layout);
    uint8_t value[24];
    memset(value, 0xa5, sizeof(value)); value[0] = 0;
    XrtModuleRef *reference = NULL;
    OK(xrt_module_begin(state, 0));
    OK(xrt_module_construct(state, 0, value, &reference));
    OK(xrt_module_publish(reference));
    const void *borrowed = NULL;
    OK(xrt_module_borrow(reference, &borrowed));
    for (uint32_t n = 0; n < sizeof(value); ++n) CHECK(!((const uint8_t *)borrowed)[n]);
    value[0] = 1; value[8] = 0;
    OK(xrt_module_replace(reference, value));
    OK(xrt_module_borrow(reference, &borrowed));
    CHECK(((const uint8_t *)borrowed)[0] == 1);
    for (uint32_t n = 1; n < sizeof(value); ++n) CHECK(!((const uint8_t *)borrowed)[n]);
    value[8] = 2;
    CHECK(xrt_module_replace(reference, value) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(((const uint8_t *)borrowed)[0] == 1 && !((const uint8_t *)borrowed)[8]);
    OK(xrt_module_replace(reference, borrowed));
    memset(value, 0, sizeof(value)); value[0] = value[8] = 1;
    int64_t payload = -73;
    memcpy(value + 16, &payload, sizeof(payload));
    OK(xrt_module_replace(reference, value));
    memset(value, 0xa5, sizeof(value));
    OK(xrt_module_borrow(reference, &borrowed));
    memcpy(&payload, (const uint8_t *)borrowed + 16, sizeof(payload));
    CHECK(payload == -73);
    xrt_module_state_drop(&state);
    CHECK(!live);
}

static void nullable_state_invalid_shapes(void) {
    for (uint32_t scenario = 0; scenario < 6; ++scenario) {
        XrtModuleTypeSpec types[] = {{XRT_MODULE_I64, 0}, {XRT_MODULE_NULLABLE, 0}};
        XrtModuleSpec module = {0};
        XrtModuleSlotSpec slot = {.type=1, .size=16, .alignment=8};
        XrtModuleLayoutSpec spec = {.types=types, .type_count=2, .modules=&module, .module_count=1,
            .slots=&slot, .slot_count=1, .payload_size=16, .payload_alignment=8, .byte_limit=UINT64_MAX};
        if (scenario == 0) types[1].argument = 1;
        if (scenario == 1) types[1].argument = UINT32_MAX;
        if (scenario == 2) types[0].kind = XRT_MODULE_STRING;
        if (scenario == 3) types[0].kind = XRT_MODULE_ARRAY_I64;
        if (scenario == 4) slot.size = 8;
        if (scenario == 5) slot.alignment = 1;
        XrtModuleLayout *output = NULL;
        CHECK(xrt_module_layout_new(&spec, &output) == XR_XIR_VALUE_BAD_ARGUMENT && !output && !live);
    }
}

static void nullable_state_deep_copy(void) {
    for (uint32_t leaf = 0; leaf < 2; ++leaf) {
        uint32_t alignment = leaf ? 8u : 1u;
        uint32_t size = 128u * alignment + (leaf ? 8u : 0u);
        XrtModuleTypeSpec types[129] = {{0}};
        types[0].kind = leaf ? XRT_MODULE_I64 : XRT_MODULE_UNIT;
        for (uint32_t n = 1; n < 129; ++n)
            types[n] = (XrtModuleTypeSpec){XRT_MODULE_NULLABLE, n - 1};
        XrtModuleSpec module = {0};
        XrtModuleSlotSpec slot = {.type=128, .flags=XRT_MODULE_SLOT_MUTABLE, .size=size, .alignment=alignment};
        XrtModuleLayoutSpec spec = {.types=types, .type_count=129, .modules=&module, .module_count=1,
            .slots=&slot, .slot_count=1, .payload_size=size, .payload_alignment=alignment, .byte_limit=UINT64_MAX};
        XrtModuleLayout *layout = NULL;
        XrtModuleState *state = NULL;
        OK(xrt_module_layout_new(&spec, &layout));
        OK(xrt_module_state_new(layout, UINT64_MAX, &state));
        xrt_module_layout_drop(&layout);
        memset(types, 0xa5, sizeof(types));
        uint8_t value[1032] = {0};
        for (uint32_t n = 0; n < 128; ++n) value[n * alignment] = 1;
        if (leaf) { int64_t number = -73; memcpy(value + 1024, &number, sizeof(number)); }
        XrtModuleRef *reference = NULL;
        OK(xrt_module_begin(state, 0));
        OK(xrt_module_construct(state, 0, value, &reference));
        OK(xrt_module_publish(reference));
        const void *borrowed = NULL;
        OK(xrt_module_borrow(reference, &borrowed));
        CHECK(memcmp(value, borrowed, size) == 0);
        value[64 * alignment] = 0;
        memset(value + 64 * alignment + 1, 0xa5, size - 64 * alignment - 1);
        OK(xrt_module_replace(reference, value));
        OK(xrt_module_borrow(reference, &borrowed));
        for (uint32_t n = 64 * alignment; n < size; ++n) CHECK(!((const uint8_t *)borrowed)[n]);
        xrt_module_state_drop(&state);
        CHECK(!live);
    }
}

static void nullable_state_allocation_failures(void) {
    for (size_t failure = 0; failure < 64; ++failure) {
        calls = 0; fail_at = failure; injected = false;
        XrtModuleLayout *layout = NULL;
        XrtModuleState *state = NULL;
        XrtModuleStatus status = nullable_state_layout(&layout);
        if (!status) status = xrt_module_state_new(layout, UINT64_MAX, &state);
        if (!status) {
            uint8_t none[24] = {0};
            XrtModuleRef *reference = NULL;
            status = xrt_module_begin(state, 0);
            if (!status) status = xrt_module_construct(state, 0, none, &reference);
            if (!status) status = xrt_module_publish(reference);
        }
        xrt_module_layout_drop(&layout);
        xrt_module_state_drop(&state);
        CHECK(!live && !layout && !state);
        if (!injected) {
            CHECK(status == XR_XIR_VALUE_OK);
            printf("Nullable module state verified %zu allocation sites\n", failure);
            break;
        }
        CHECK(status == XR_XIR_VALUE_OOM && failure != 63);
    }
    fail_at = SIZE_MAX;
}
