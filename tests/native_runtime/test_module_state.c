/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_module_state.c - State ownership through failures and root retirement
 */
#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_module_state.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_XIR_VALUE_OK)

static size_t calls, live, fail_at = SIZE_MAX;
static bool injected;
static bool allocation_fails(void) {
    if (calls++ != fail_at) return false;
    injected = true;
    return true;
}
static void *counted_malloc(size_t size) {
    if (allocation_fails()) return NULL;
    void *memory = xr_malloc(size);
    if (memory) ++live;
    return memory;
}
static void *counted_calloc(size_t count, size_t size) {
    if (allocation_fails()) return NULL;
    void *memory = xr_calloc(count, size);
    if (memory) ++live;
    return memory;
}
static void *counted_aligned(size_t size, size_t alignment) {
    if (allocation_fails()) return NULL;
    void *memory = xr_malloc_aligned(size, alignment);
    if (memory) ++live;
    return memory;
}
static void counted_free(void *memory) {
    if (memory) { CHECK(live); --live; }
    xr_free(memory);
}
static void counted_aligned_free(void *memory, size_t alignment) {
    (void)alignment;
    if (memory) { CHECK(live); --live; }
    xr_free_aligned(memory, alignment);
}
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#undef xr_malloc_aligned
#undef xr_free_aligned
#define xr_malloc(size) counted_malloc(size)
#define xr_calloc(count, size) counted_calloc(count, size)
#define xr_free(memory) counted_free(memory)
#define xr_malloc_aligned(size, alignment) counted_aligned(size, alignment)
#define xr_free_aligned(memory, alignment) counted_aligned_free(memory, alignment)
#include "runtime/native/xnative_module_state.c"

static XrtModuleStatus make_layout(XrtModuleLayout **out) {
    XrtModuleTypeSpec types[] = {{XRT_MODULE_I64, 0}, {XRT_MODULE_STRING, 0}, {XRT_MODULE_BOOL, 0}, {XRT_MODULE_F64, 0}};
    XrtModuleSpec modules[] = {{0, 0}, {0, 1}};
    uint32_t dependencies[] = {0};
    XrtModuleSlotSpec slots[] = {
        {.module=0, .type=0, .flags=XRT_MODULE_SLOT_MUTABLE, .offset=0, .size=8, .alignment=8},
        {.module=0, .type=1, .offset=8, .size=8, .alignment=8},
        {.module=1, .type=2, .flags=XRT_MODULE_SLOT_MUTABLE, .offset=16, .size=1, .alignment=1},
        {.module=1, .type=3, .offset=24, .size=8, .alignment=8}};
    XrtModuleLayoutSpec spec = {.types=types, .type_count=4, .modules=modules, .module_count=2,
        .dependencies=dependencies, .dependency_count=1, .slots=slots, .slot_count=4,
        .payload_size=32, .payload_alignment=8, .byte_limit=UINT64_MAX};
    return xrt_module_layout_new(&spec, out);
}

static void allocation_failures(void) {
    for (size_t failure = 0; failure < 64; ++failure) {
        calls = 0; fail_at = failure; injected = false;
        XrtModuleLayout *layout = NULL;
        XrtModuleState *state = NULL;
        XrtModuleStatus status = make_layout(&layout);
        if (!status) status = xrt_module_state_new(layout, UINT64_MAX, &state);
        xrt_module_layout_drop(&layout);
        xrt_module_state_drop(&state);
        CHECK(!live && !layout && !state);
        if (!injected) { CHECK(status == XR_XIR_VALUE_OK); break; }
        CHECK(status == XR_XIR_VALUE_OOM);
        CHECK(failure != 63);
    }
    fail_at = SIZE_MAX;
}

typedef struct StateRootContext { XrtModuleState *state; bool panic; } StateRootContext;

static void initialize_root(void *context) {
    StateRootContext *root = context;
    XrtModuleState *state = root->state;
    CHECK(xrt_module_begin(state, 1) == XR_XIR_VALUE_BAD_ARGUMENT);
    OK(xrt_module_begin(state, 0));
    CHECK(xrt_module_ready(state, 0) == XR_XIR_VALUE_BAD_ARGUMENT);
    XrtModuleAccess cached;
    OK(xrt_module_cache(state, 0, &cached));
    CHECK(!*cached.flags && cached.required_flags == SLOT_PUBLISHED);
    int64_t number = 17;
    XrtModuleRef *initial = NULL, *published = NULL;
    OK(xrt_module_construct(state, 0, &number, &initial));
    const void *payload = NULL;
    OK(xrt_module_borrow(initial, &payload));
    CHECK(*(const int64_t *)payload == 17);
    OK(xrt_module_replace(initial, payload));
    CHECK(xrt_module_reference(state, 0, &published) == XR_XIR_VALUE_BAD_ARGUMENT && !published);
    OK(xrt_module_publish(initial));
    CHECK(xrt_module_publish(initial) == XR_XIR_VALUE_BAD_ARGUMENT);
    XrtString *string = xrt_string_new("module\0owner", 12, 0);
    OK(xrt_module_initialize(state, 1, &string));
    xrt_string_drop(&string);
    OK(xrt_module_reference(state, 1, &published));
    CHECK(xrt_module_replace(published, &string) == XR_XIR_VALUE_BAD_ARGUMENT);
    OK(xrt_module_ready(state, 0));
    CHECK(xrt_module_begin(state, 0) == XR_XIR_VALUE_BAD_ARGUMENT);
    OK(xrt_module_begin(state, 1));
    uint8_t boolean = 0;
    initial = NULL;
    OK(xrt_module_construct(state, 2, &boolean, &initial));
    boolean = 2;
    CHECK(xrt_module_replace(initial, &boolean) == XR_XIR_VALUE_BAD_ARGUMENT);
    OK(xrt_module_borrow(initial, &payload));
    CHECK(*(const uint8_t *)payload == 0);
    boolean = 1;
    OK(xrt_module_replace(initial, &boolean));
    if (root->panic) xrt_arithmetic_panic(420, 0);
    OK(xrt_module_publish(initial));
    uint64_t floating = UINT64_C(0x4004000000000000);
    OK(xrt_module_initialize(state, 3, &floating));
    OK(xrt_module_ready(state, 1));
}

static void independent_lifetimes(void) {
    XrtModuleLayout *layout = NULL;
    OK(make_layout(&layout));
    XrtModuleLayoutSpec view;
    OK(xrt_module_layout_view(layout, &view));
    CHECK(view.modules[1].dependency_count == 1 && view.dependencies[0] == 0);
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(NULL, 0, &locations) == XRT_LOCATION_OK);
    for (unsigned panic = 0; panic < 2; ++panic) {
        XrtModuleState *state = NULL;
        CHECK(xrt_module_state_new(layout, 0, &state) == XR_XIR_VALUE_MEMORY_LIMIT && !state);
        OK(xrt_module_state_new(layout, UINT64_MAX, &state));
        StateRootContext context = {state, panic != 0};
        XrtExecutionFailure failure = {0};
        XrtExecutionStatus status = xrt_run_embedded(locations, &failure, initialize_root, &context);
        CHECK(status == (panic ? XRT_EXECUTION_FAILURE : XRT_EXECUTION_COMPLETED));
        if (panic) CHECK(failure.kind == XRT_FAILURE_ARITHMETIC_PANIC && failure.detail.panic.code == 420);
        XrtModuleRef *reference = NULL;
        OK(xrt_module_reference(state, 1, &reference));
        const void *payload = NULL;
        OK(xrt_module_borrow(reference, &payload));
        XrtString *string;
        memcpy(&string, payload, sizeof(string));
        size_t length = 0;
        const char *bytes = xrt_string_bytes(string, &length);
        CHECK(length == 12 && !memcmp(bytes, "module\0owner", 12));
        CHECK(state->publication_count == (panic ? 2u : 4u));
        xrt_module_state_clear(state);
        CHECK(!state->publication_count && !state->construction_count);
        reference = NULL;
        CHECK(xrt_module_reference(state, 1, &reference) == XR_XIR_VALUE_BAD_ARGUMENT);
        xrt_module_state_clear(state);
        xrt_module_state_drop(&state);
    }
    xrt_locations_release(locations);
    xrt_module_layout_drop(&layout);
    CHECK(!live);
}

static void replace_string_root(void *context) {
    XrtModuleState *state = context;
    OK(xrt_module_begin(state, 0));
    XrtString *string = xrt_string_new("pending", 7, 0);
    XrtModuleRef *reference = NULL;
    OK(xrt_module_construct(state, 1, &string, &reference));
    xrt_string_drop(&string);
    const void *before = NULL, *after = NULL;
    OK(xrt_module_borrow(reference, &before));
    XrtString *original;
    memcpy(&original, before, sizeof(original));
    XrtString *empty = NULL;
    CHECK(xrt_module_replace(reference, &empty) == XR_XIR_VALUE_BAD_ARGUMENT);
    OK(xrt_module_borrow(reference, &after));
    XrtString *preserved;
    memcpy(&preserved, after, sizeof(preserved));
    CHECK(before == after && original == preserved && !memcmp(state->scratch, &empty, sizeof(empty)));
    OK(xrt_module_replace(reference, before));
    CHECK(state->construction_count == 1 && !state->publication_count);
    xrt_arithmetic_panic(421, 0);
}

static void pending_string_failure(void) {
    XrtModuleLayout *layout = NULL, *mutable_layout = NULL;
    OK(make_layout(&layout));
    XrtModuleLayoutSpec spec;
    OK(xrt_module_layout_view(layout, &spec));
    XrtModuleSlotSpec slots[4];
    memcpy(slots, spec.slots, sizeof(slots));
    slots[1].flags = XRT_MODULE_SLOT_MUTABLE;
    spec.slots = slots;
    OK(xrt_module_layout_new(&spec, &mutable_layout));
    xrt_module_layout_drop(&layout);
    XrtModuleState *state = NULL;
    OK(xrt_module_state_new(mutable_layout, UINT64_MAX, &state));
    xrt_module_layout_drop(&mutable_layout);
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(NULL, 0, &locations) == XRT_LOCATION_OK);
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded(locations, &failure, replace_string_root, state) == XRT_EXECUTION_FAILURE);
    CHECK(failure.kind == XRT_FAILURE_ARITHMETIC_PANIC && failure.detail.panic.code == 421);
    CHECK(state->construction_count == 1 && !state->publication_count);
    xrt_module_state_drop(&state);
    xrt_locations_release(locations);
    CHECK(!live);
}

static void invalid_layouts(void) {
    XrtModuleLayout *layout = NULL;
    OK(make_layout(&layout));
    XrtModuleLayoutSpec original;
    OK(xrt_module_layout_view(layout, &original));
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        XrtModuleLayoutSpec spec = original;
        XrtModuleSlotSpec slots[4];
        memcpy(slots, spec.slots, sizeof(slots));
        spec.slots = slots;
        if (scenario == 0) slots[1].offset = 0;
        if (scenario == 1) slots[0].type = UINT32_MAX;
        if (scenario == 2) slots[0].module = UINT32_MAX;
        if (scenario == 3) slots[0].size = 1;
        if (scenario == 4) slots[0].alignment = 4;
        if (scenario == 5) slots[0].flags = UINT32_MAX;
        if (scenario == 6) spec.payload_size = UINT64_MAX;
        if (scenario == 7) spec.byte_limit = 0;
        XrtModuleLayout *output = NULL;
        CHECK(xrt_module_layout_new(&spec, &output) != XR_XIR_VALUE_OK && !output);
    }
    xrt_module_layout_drop(&layout);
    CHECK(!live);
}

#include "native_fixed_state_cases.h"
#include "native_nullable_state_cases.h"
#include "native_callable_state_cases.h"

int main(void) {
    callable_state_values();
    nullable_state_values(); nullable_state_invalid_shapes(); nullable_state_deep_copy(); nullable_state_allocation_failures();
    allocation_failures(); independent_lifetimes(); pending_string_failure(); invalid_layouts();
    fixed_state_values(); fixed_state_invalid_shapes(); fixed_state_allocation_failures();
    puts("Typed module state ownership and failures passed");
    return 0;
}
