/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_group_state_cases.h - Whole I64 publication groups preserve pending owners
 */
static void native_group_state(void) {
    XrtModuleTypeSpec type = {XRT_MODULE_I64, 0};
    XrtModuleSpec module = {0};
    XrtModuleSlotSpec slots[4] = {0};
    for (uint32_t n = 0; n < 4; ++n)
        slots[n] = (XrtModuleSlotSpec){.flags=n == 2 ? 0 : XRT_MODULE_SLOT_MUTABLE,
            .offset=(uint64_t)n * 8, .size=8, .alignment=8};
    XrtModuleLayoutSpec spec = {.types=&type, .type_count=1, .modules=&module,
        .module_count=1, .slots=slots, .slot_count=4, .payload_size=32,
        .payload_alignment=8, .byte_limit=UINT64_MAX};
    XrtModuleLayout *layout = NULL;
    XrtModuleState *state = NULL;
    OK(xrt_module_layout_new(&spec, &layout));
    OK(xrt_module_state_new(layout, UINT64_MAX, &state));
    xrt_module_layout_drop(&layout);
    OK(xrt_module_begin(state, 0));
    int64_t old[] = {7, 8, 0, 9}, values[] = {11, 12, 13};
    XrtModuleRef *initial[4] = {0};
    for (uint32_t n = 0; n < 2; ++n)
        OK(xrt_module_construct(state, n, &old[n], &initial[n]));
    CHECK(xrt_module_initialize_i64_group(state, 1, 3, values) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!state->publication_count && state->remaining[0] == 4);
    OK(xrt_module_construct(state, 3, &old[3], &initial[3]));
    size_t before = live;
    state->byte_limit = state->bytes + sizeof(values) - 1;
    CHECK(xrt_module_initialize_i64_group(state, 1, 3, values) == XR_XIR_VALUE_MEMORY_LIMIT);
    state->byte_limit = UINT64_MAX;
    fail_at = calls;
    CHECK(xrt_module_initialize_i64_group(state, 1, 3, values) == XR_XIR_VALUE_OOM && injected);
    fail_at = SIZE_MAX;
    CHECK(live == before && !state->publication_count && state->construction_count == 3 && state->remaining[0] == 4);
    CHECK(!memcmp(state->payload, old, sizeof(old)));
    CHECK(xrt_module_initialize_i64_group(state, 1, UINT32_MAX, values) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xrt_module_initialize_i64_group(state, 1, 0, values) == XR_XIR_VALUE_BAD_ARGUMENT);
    OK(xrt_module_initialize_i64_group(state, 1, 3, (const int64_t *)state->payload));
    int64_t expected[] = {7, 8, 8, 9};
    CHECK(!memcmp(state->payload, expected, sizeof(expected)) && state->publication_count == 3);
    CHECK(xrt_module_initialize_i64_group(state, 1, 3, values) == XR_XIR_VALUE_BAD_ARGUMENT);
    OK(xrt_module_publish(initial[0]));
    OK(xrt_module_ready(state, 0));
    xrt_module_state_clear(state);
    CHECK(!state->publication_count && !state->construction_count && state->remaining[0] == 4);
    OK(xrt_module_begin(state, 0));
    for (uint32_t n = 0; n < 4; ++n) {
        initial[n] = NULL;
        if (n != 2) OK(xrt_module_construct(state, n, &old[n], &initial[n]));
    }
    OK(xrt_module_initialize_i64_group(state, 1, 3, values));
    int64_t retried[] = {7, 8, 12, 9};
    CHECK(!memcmp(state->payload, retried, sizeof(retried)));
    OK(xrt_module_publish(initial[0]));
    OK(xrt_module_ready(state, 0));
    xrt_module_state_drop(&state);
    CHECK(!live && !state && !layout);
    XrtModuleSpec modules[2] = {{0}, {0}};
    slots[3].module = 1;
    spec.modules = modules;
    spec.module_count = 2;
    OK(xrt_module_layout_new(&spec, &layout));
    OK(xrt_module_state_new(layout, UINT64_MAX, &state));
    xrt_module_layout_drop(&layout);
    OK(xrt_module_begin(state, 0));
    OK(xrt_module_begin(state, 1));
    for (uint32_t n = 0; n < 4; ++n) {
        initial[n] = NULL;
        if (n != 2) OK(xrt_module_construct(state, n, &old[n], &initial[n]));
    }
    CHECK(xrt_module_initialize_i64_group(state, 1, 3, values) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!state->publication_count && !memcmp(state->payload, old, sizeof(old)));
    xrt_module_state_drop(&state);
    CHECK(!live);
    for (uint32_t n = 0; n < 4; ++n) {
        slots[n].module = 0;
        slots[n].flags = n ? 0 : XRT_MODULE_SLOT_MUTABLE;
    }
    spec.modules = &module;
    spec.module_count = 1;
    OK(xrt_module_layout_new(&spec, &layout));
    OK(xrt_module_state_new(layout, UINT64_MAX, &state));
    xrt_module_layout_drop(&layout);
    OK(xrt_module_begin(state, 0));
    initial[0] = NULL;
    OK(xrt_module_construct(state, 0, &old[0], &initial[0]));
    OK(xrt_module_initialize_i64_group(state, 1, 3, (const int64_t *)state->payload));
    int64_t shifted[] = {7, 7, 0, 0};
    CHECK(!memcmp(state->payload, shifted, sizeof(shifted)));
    OK(xrt_module_publish(initial[0]));
    OK(xrt_module_ready(state, 0));
    xrt_module_state_drop(&state);
    CHECK(!live);
    puts("I64 publication groups preserve all pending slots across budget and allocation failures");
}
