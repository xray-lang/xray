/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_compile_request_memory.c - Observe physical ownership and request isolation
 */
#include "base/xcompile_resources.h"
#include "base/xcompile_state.h"
#include "base/xcompile_request.h"
#include "base/xmalloc.h"
#include <stdio.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_COMPILE_RESOURCE_OK)

typedef struct PhysicalBlock { void *memory; size_t bytes; } PhysicalBlock;
static PhysicalBlock blocks[128];
static size_t attempts, fail_at = SIZE_MAX, held, peak, total;
static void *observed_allocate(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes);
    CHECK(memory != NULL);
    size_t i = 0;
    while (i < 128 && blocks[i].memory) ++i;
    CHECK(i < 128);
    blocks[i] = (PhysicalBlock){memory, bytes};
    held += bytes; total += bytes;
    if (held > peak) peak = held;
    return memory;
}
static void observed_free(void *memory) {
    size_t i = 0;
    while (i < 128 && blocks[i].memory != memory) ++i;
    CHECK(i < 128 && memory);
    held -= blocks[i].bytes;
    blocks[i] = (PhysicalBlock){0};
    xr_free(memory);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) observed_allocate(bytes)
#define xr_free(memory) observed_free(memory)
#include "base/xcompile_resources.c"
#include "base/xcompile_state.c"

static const XrCompileResourceLimits ordinary = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
static void reset_observer(void) {
    CHECK(!held);
    for (size_t i = 0; i < 128; ++i) CHECK(!blocks[i].memory);
    attempts = peak = total = 0; fail_at = SIZE_MAX;
}
static XrCompileResourceStats snapshot(XrCompileResources *owner) {
    XrCompileResourceStats stats;
    OK(xr_compile_resources_stats(owner, &stats));
    CHECK(stats.held_bytes == held && stats.peak_bytes == peak);
    return stats;
}
static void memory_policy(void) {
    reset_observer();
    XrCompileResources *owner = NULL;
    XrCompileResourceLimits finite = {XR_COMPILE_MEMORY_FINITE, 0, true};
    CHECK(xr_compile_resources_new(&finite, &owner) == XR_COMPILE_RESOURCE_MEMORY_LIMIT && !owner);
    CHECK(!attempts);
    XrCompileResourceLimits invalid = {XR_COMPILE_MEMORY_ORDINARY, 1, false};
    CHECK(xr_compile_resources_new(&invalid, &owner) == XR_COMPILE_RESOURCE_BAD_ARGUMENT && !owner);
    OK(xr_compile_resources_new(&ordinary, &owner));
    uint64_t owner_bytes = snapshot(owner).held_bytes;
    void *memory = NULL;
    OK(xr_compile_resources_calloc(owner, 16, 1, &memory));
    for (size_t i = 0; i < 16; ++i) CHECK(!((unsigned char *)memory)[i]);
    uint64_t old_block = snapshot(owner).held_bytes - owner_bytes;
    CHECK(old_block > 16 && snapshot(owner).allocated_bytes == total);
    memset(memory, 0x5a, 16);
    size_t call_count = attempts;
    void *overflow = NULL;
    CHECK(xr_compile_resources_alloc(owner, SIZE_MAX, &overflow) == XR_COMPILE_RESOURCE_OVERFLOW && !overflow);
    CHECK(xr_compile_resources_calloc(owner, SIZE_MAX, 2, &overflow) == XR_COMPILE_RESOURCE_OVERFLOW && !overflow);
    CHECK(attempts == call_count);
    OK(xr_compile_resources_resize(owner, &memory, 32));
    uint64_t new_block = snapshot(owner).held_bytes - owner_bytes;
    CHECK(snapshot(owner).peak_bytes == owner_bytes + old_block + new_block);
    for (size_t i = 0; i < 16; ++i) CHECK(((unsigned char *)memory)[i] == 0x5a);
    xr_compile_resources_free(memory); xr_compile_resources_release(owner);
    CHECK(!held);
    for (unsigned pass = 0; pass < 2; ++pass) {
        reset_observer(); owner = NULL; memory = NULL;
        finite.memory_limit = owner_bytes + old_block + new_block - (pass ? 0 : 1);
        OK(xr_compile_resources_new(&finite, &owner));
        OK(xr_compile_resources_alloc(owner, 16, &memory));
        memset(memory, 0x37, 16); void *saved = memory;
        if (!pass) {
            size_t before = attempts;
            CHECK(xr_compile_resources_resize(owner, &memory, 32) == XR_COMPILE_RESOURCE_MEMORY_LIMIT);
            CHECK(memory == saved && attempts == before);
            CHECK(snapshot(owner).held_bytes == owner_bytes + old_block);
        } else {
            OK(xr_compile_resources_resize(owner, &memory, 32));
            CHECK(snapshot(owner).peak_bytes == finite.memory_limit);
        }
        for (size_t i = 0; i < 16; ++i) CHECK(((unsigned char *)memory)[i] == 0x37);
        xr_compile_resources_free(memory); xr_compile_resources_release(owner);
        CHECK(!held);
    }
    reset_observer(); owner = NULL;
    finite.memory_limit = owner_bytes;
    OK(xr_compile_resources_new(&finite, &owner)); memory = NULL;
    CHECK(xr_compile_resources_admit(owner, 1) == XR_COMPILE_RESOURCE_MEMORY_LIMIT);
    CHECK(xr_compile_resources_alloc(owner, 1, &memory) == XR_COMPILE_RESOURCE_MEMORY_LIMIT && !memory);
    xr_compile_resources_release(owner); CHECK(!held);
}
static void extent_and_transfer(void) {
    reset_observer();
    XrCompileResources *owner = NULL, *foreign = NULL;
    OK(xr_compile_resources_new(&ordinary, &owner));
    OK(xr_compile_resources_new(&ordinary, &foreign));
    void *memory = NULL; OK(xr_compile_resources_alloc(owner, 8, &memory));
    OK(xr_compile_resources_extent(owner, (char *)memory + 7, 1));
    OK(xr_compile_resources_allocation(owner, memory, 8));
    CHECK(xr_compile_resources_extent(owner, (char *)memory + 7, 2) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_allocation(owner, (char *)memory + 1, 1) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_extent(foreign, memory, 1) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_extent(owner, (void *)(uintptr_t)1, 1) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_extent(owner, (void *)UINTPTR_MAX, SIZE_MAX) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    void *saved = memory;
    CHECK(xr_compile_resources_resize(foreign, &memory, 9) == XR_COMPILE_RESOURCE_BAD_ARGUMENT && memory == saved);
    void *unreadable = (void *)(uintptr_t)1;
    CHECK(xr_compile_resources_resize(owner, &unreadable, 9) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(unreadable == (void *)(uintptr_t)1);
    size_t advanced = 99; bool found = false;
    OK(xr_compile_resources_scan_delimiter(owner, "abc:tail", 8, ':', &advanced, &found));
    CHECK(advanced == 3 && found);
    CHECK(xr_compile_resources_scan_delimiter(owner, "", 65, ':', &advanced, &found) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(advanced == 3 && found);
    uintptr_t retired = (uintptr_t)memory;
    xr_compile_resources_free(memory);
    CHECK(xr_compile_resources_extent(owner, (void *)retired, 1) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    xr_compile_resources_release(foreign);
    XrCompileState *state = NULL; OK(xr_compile_state_new(owner, &state));
    OK(xr_compile_state_session_open(state)); CHECK(xr_compile_state_has_sessions(state));
    xr_compile_state_session_close(state); CHECK(!xr_compile_state_has_sessions(state));
    char *text = NULL; OK(xr_compile_state_strdup(state, "alive", &text));
    char copied[6] = {0}; OK(xr_compile_state_copy(state, copied, text, 6));
    CHECK(!strcmp(copied, "alive")); OK(xr_compile_state_zero(state, copied, sizeof(copied)));
    for (size_t i = 0; i < sizeof(copied); ++i) CHECK(!copied[i]);
    char *slice = NULL; OK(xr_compile_state_strndup(state, "abc-suffix", 3, &slice));
    CHECK(!strcmp(slice, "abc")); xr_compile_state_free(slice);
    xr_compile_state_release(state); xr_compile_resources_release(owner);
    CHECK(!strcmp(text, "alive") && held > 0);
    xr_compile_resources_free(text); CHECK(!held);
}
static void allocation_failure_and_profile(void) {
    reset_observer();
    XrCompileResources *owner = NULL;
    fail_at = 0;
    CHECK(xr_compile_resources_new(&ordinary, &owner) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !owner && !held);
    fail_at = SIZE_MAX;
    OK(xr_compile_resources_new(&ordinary, &owner));
    XrCompileState *failed = NULL; OK(xr_compile_state_new(owner, &failed));
    size_t before = held; fail_at = attempts;
    void *memory = NULL;
    CHECK(xr_compile_state_alloc(failed, 8, &memory) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !memory);
    CHECK(snapshot(owner).held_bytes == before);
    fail_at = SIZE_MAX;
    CHECK(xr_compile_state_calloc(failed, 1, 8, &memory) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !memory);
    xr_compile_state_release(failed);
    XrCompileState *fresh = NULL; OK(xr_compile_state_new(owner, &fresh));
    OK(xr_compile_state_alloc(fresh, 8, &memory)); memset(memory, 0x19, 8);
    void *saved = memory; before = held; fail_at = attempts;
    CHECK(xr_compile_state_resize(fresh, &memory, 16) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    CHECK(memory == saved && held == before);
    for (size_t i = 0; i < 8; ++i) CHECK(((unsigned char *)memory)[i] == 0x19);
    xr_compile_state_free(memory); xr_compile_state_release(fresh); fail_at = SIZE_MAX;
    /* Saturating telemetry cannot reject a new physical allocation. */
    owner->stats.allocated_bytes = UINT64_MAX - 1; owner->stats.allocation_count = UINT64_MAX;
    memory = NULL; OK(xr_compile_resources_alloc(owner, 8, &memory));
    XrCompileResourceStats stats = snapshot(owner);
    CHECK(stats.allocated_bytes == UINT64_MAX && stats.allocation_count == UINT64_MAX);
    xr_compile_resources_free(memory); xr_compile_resources_release(owner); CHECK(!held);
    reset_observer(); owner = NULL;
    XrCompileResourceLimits no_profile = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    OK(xr_compile_resources_new(&no_profile, &owner));
    memory = NULL; OK(xr_compile_resources_alloc(owner, 128, &memory));
    stats = snapshot(owner); CHECK(!stats.allocated_bytes && !stats.allocation_count);
    xr_compile_resources_free(memory); xr_compile_resources_release(owner); CHECK(!held);
}
typedef struct CancelContext { XrCompileResources *owner; unsigned polls; bool cancel; } CancelContext;
static bool cancel_observe(const void *pointer) {
    CancelContext *context = (CancelContext *)pointer;
    XrCompileResourceStats stats;
    OK(xr_compile_resources_stats(context->owner, &stats));
    CHECK(stats.held_bytes > 0); ++context->polls;
    return context->cancel;
}
static void request_isolation(void) {
    reset_observer();
    XrCompileResources *owner = NULL; OK(xr_compile_resources_new(&ordinary, &owner));
    CancelContext context = {owner, 0, true};
    XrCompileRequestOptions options = {cancel_observe, &context, XR_COMPILE_DEADLINE_NONE, 0};
    XrCompileRequestControl first = {0}; OK(xr_compile_request_control_init(&options, &first));
    CHECK(xr_compile_request_control_poll(&first) == XR_COMPILE_RESOURCE_CANCELLED && context.polls == 1);
    CHECK(xr_compile_request_control_poll(&first) == XR_COMPILE_RESOURCE_CANCELLED && context.polls == 1);
    CHECK(xr_compile_request_control_init(&options, &first) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    context.cancel = false;
    XrCompileRequestControl second = {0}; OK(xr_compile_request_control_init(&options, &second));
    OK(xr_compile_request_control_poll(&second)); CHECK(context.polls == 2);
    XrCompileRequestOptions deadline = {0}; deadline.deadline_mode = XR_COMPILE_DEADLINE_AFTER;
    XrCompileRequestControl immediate = {0}; OK(xr_compile_request_control_init(&deadline, &immediate));
    CHECK(xr_compile_request_control_poll(&immediate) == XR_COMPILE_RESOURCE_DEADLINE);
    deadline.timeout_ns = UINT64_C(10000000000);
    XrCompileRequestControl running = {0}; OK(xr_compile_request_control_init(&deadline, &running));
    OK(xr_compile_request_control_poll(&running));
    xr_compile_resources_release(owner); CHECK(!held);
}
int main(void) {
    memory_policy(); extent_and_transfer(); allocation_failure_and_profile(); request_isolation();
    puts("request memory ownership and isolation: PASS"); return 0;
}
