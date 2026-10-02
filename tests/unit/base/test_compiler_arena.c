/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_compiler_arena.c - Observe exact physical ownership and sticky failures
 */
#include "base/xarena.h"
#include "base/xcompile_resources.h"
#include "toolchain/xcompiler_arena_backing.h"
#include "base/xmalloc.h"
#include "base/xchecks.h"
#include <stdio.h>
#include <string.h>
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#endif

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_ARENA_OK)
#define RESOURCE_OK(c) CHECK((c) == XR_COMPILE_RESOURCE_OK)

typedef struct AllocationRecord { void *pointer; size_t bytes; } AllocationRecord;
static AllocationRecord records[32];
static size_t attempts, fail_at = SIZE_MAX, physical_live, physical_total, physical_peak, blocks;

static void *observed_allocate(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes);
    CHECK(memory != NULL);
    size_t i = 0;
    while (i < 32 && records[i].pointer) ++i;
    CHECK(i < 32);
    records[i] = (AllocationRecord) {memory, bytes};
    ++blocks;
    physical_live += bytes;
    physical_total += bytes;
    if (physical_live > physical_peak) physical_peak = physical_live;
    return memory;
}

static void observed_free(void *memory) {
    if (!memory) return;
    size_t i = 0;
    while (i < 32 && records[i].pointer != memory) ++i;
    CHECK(i < 32);
    physical_live -= records[i].bytes;
    --blocks;
    records[i] = (AllocationRecord) {0};
    xr_free(memory);
}

/* Inject at the actual production allocator call, below both adapters. */
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) observed_allocate(bytes)
#define xr_free(memory) observed_free(memory)
#include "base/xcompile_resources.c"
#include "base/xarena_backing.c"

static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};

static void observer_reset(void) {
    CHECK(!blocks && !physical_live);
    attempts = physical_total = physical_peak = 0;
    fail_at = SIZE_MAX;
}

static XrCompileResourceStats snapshot(XrCompileResources *owner) {
    XrCompileResourceStats result;
    RESOURCE_OK(xr_compile_resources_stats(owner, &result));
    CHECK(result.live_bytes == physical_live && result.allocated_bytes == physical_total);
    CHECK(result.peak_bytes == physical_peak);
    return result;
}

static XrArenaStatus compiler_open(XrArena *arena, size_t size, XrCompileResources *owner) {
    XrArenaBacking local;
    OK(xr_compiler_arena_backing(owner, &local));
    XrArenaStatus status = xr_arena_open(arena, size, &local);
    memset(&local, 0xA5, sizeof(local));
    return status;
}

/* Independent work: five allocator calls (ledger + four segments), zero 3+6,
 * scan 4, copy 4+2, terminator 1, restore inspection 3: exactly 28. */
static XrArenaStatus pipeline(const XrCompileResourceLimits *limits, XrCompileResourceStats *result) {
    XrCompileResources *owner = NULL;
    XrArena arena = {0};
    XrCompileResourceStatus created = xr_compile_resources_new(limits, &owner);
    if (created != XR_COMPILE_RESOURCE_OK)
        return created == XR_COMPILE_RESOURCE_BUDGET ? XR_ARENA_BUDGET : XR_ARENA_OUT_OF_MEMORY;
    XrArenaStatus status = compiler_open(&arena, 32, owner);
    if (status == XR_ARENA_OK) {
        unsigned char *bytes = xr_arena_alloc(&arena, 3);
        if (bytes) CHECK(bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0);
        char *a = xr_arena_strdup(&arena, "abc");
        if (a) CHECK(!strcmp(a, "abc"));
        char *b = xr_arena_strndup(&arena, "XYignored", 2);
        if (b) CHECK(!strcmp(b, "XY"));
        XrArenaState mark = xr_arena_save(&arena);
        void *raw = xr_arena_alloc_raw(&arena, 16);
        if (raw) CHECK((uintptr_t) raw % XR_ARENA_ALIGNMENT == 0);
        (void) xr_arena_alloc_raw(&arena, 70000);
        if (xr_arena_status(&arena) == XR_ARENA_OK) {
            CHECK(arena.segment_count == 3 && arena.total_capacity == 32 + 65536 + 70000);
            CHECK(arena.total_allocated == 24 + 16 + 70000);
        }
        xr_arena_restore(&arena, mark);
        if (xr_arena_status(&arena) == XR_ARENA_OK) {
            CHECK(arena.segment_count == 1 && arena.total_capacity == 32);
            CHECK(arena.total_allocated == 24 && blocks == 2);
        }
        xr_arena_reset(&arena);
        (void) xr_arena_alloc_array(&arena, 2, 3);
        status = xr_arena_status(&arena);
    }
    if (status == XR_ARENA_OK) {
        xr_arena_destroy(&arena);
        CHECK(blocks == 1);
        status = compiler_open(&arena, 16, owner);
        if (status == XR_ARENA_OK) CHECK(xr_arena_alloc_raw(&arena, 1) != NULL);
    }
    *result = snapshot(owner);
    xr_arena_destroy(&arena);
    XrCompileResourceStats after = snapshot(owner);
    CHECK(after.work == result->work && after.allocated_bytes == result->allocated_bytes);
    CHECK(blocks == 1);
    xr_compile_resources_release(owner);
    CHECK(!blocks && !physical_live);
    return status;
}

static void exact_limits_and_oom(void) {
    observer_reset();
    XrCompileResourceStats measured = {0}, result = {0};
    OK(pipeline(&unlimited, &measured));
    CHECK(attempts == 5 && measured.allocation_count == 5 && measured.work == 28);
    CHECK(measured.allocated_bytes > measured.peak_bytes);
    XrCompileResourceLimits exact = {measured.allocated_bytes, measured.peak_bytes, 28};
    observer_reset();
    OK(pipeline(&exact, &result));
    CHECK(!memcmp(&result, &measured, sizeof(result)));
    for (unsigned field = 0; field < 3; ++field) {
        observer_reset();
        XrCompileResourceLimits smaller = exact;
        if (field == 0) --smaller.allocated_bytes;
        if (field == 1) --smaller.live_bytes;
        if (field == 2) --smaller.work;
        CHECK(pipeline(&smaller, &result) == XR_ARENA_BUDGET);
    }
    const uint64_t oom_work[] = {0, 2, 17, 18, 28};
    for (size_t point = 0; point < 5; ++point) {
        observer_reset();
        fail_at = point;
        result = (XrCompileResourceStats) {0};
        CHECK(pipeline(&unlimited, &result) == XR_ARENA_OUT_OF_MEMORY);
        CHECK(attempts == point + 1 && result.work == oom_work[point]);
        CHECK(result.allocation_count == point && !blocks);
    }
}

static void sticky_failure_and_owner(void) {
    observer_reset();
    XrCompileResources *owner = NULL;
    RESOURCE_OK(xr_compile_resources_new(&unlimited, &owner));
    XrArena arena = {0};
    OK(compiler_open(&arena, 8, owner));
    CHECK(xr_arena_alloc_raw(&arena, 8) != NULL);
    XrArenaState mark = xr_arena_save(&arena);
    fail_at = attempts;
    CHECK(!xr_arena_alloc_raw(&arena, 9));
    CHECK(xr_arena_status(&arena) == XR_ARENA_OUT_OF_MEMORY);
    XrCompileResourceStats before = snapshot(owner);
    size_t calls = attempts;
    const char *inaccessible = (const char *) (uintptr_t) 1;
    CHECK(!xr_arena_strdup(&arena, inaccessible));
    CHECK(!xr_arena_strndup(&arena, inaccessible, 3));
    CHECK(!xr_arena_alloc(&arena, SIZE_MAX));
    CHECK(!xr_arena_alloc_array(&arena, 1, 1));
    CHECK(!xr_arena_save(&arena).owner);
    xr_arena_reset(&arena);
    xr_arena_restore(&arena, mark);
    CHECK(arena.total_allocated == 8 && xr_arena_status(&arena) == XR_ARENA_OUT_OF_MEMORY);
    CHECK(compiler_open(&arena, 8, owner) == XR_ARENA_OUT_OF_MEMORY);
    XrCompileResourceStats after = snapshot(owner);
    CHECK(!memcmp(&before, &after, sizeof(before)) && attempts == calls);
    xr_arena_destroy(&arena);
    fail_at = SIZE_MAX;
    OK(compiler_open(&arena, 8, owner));
    CHECK(snapshot(owner).work == before.work + 1);
    xr_compile_resources_release(owner);
    owner = NULL;
    CHECK(xr_arena_strdup(&arena, "alive") != NULL);
    xr_arena_destroy(&arena);
    CHECK(!blocks);
    xr_arena_destroy(&arena);

    observer_reset();
    RESOURCE_OK(xr_compile_resources_new(&unlimited, &owner));
    fail_at = attempts;
    CHECK(compiler_open(&arena, 8, owner) == XR_ARENA_OUT_OF_MEMORY);
    CHECK(!arena.head && arena.retained && blocks == 1);
    xr_compile_resources_release(owner);
    CHECK(blocks == 1);
    xr_arena_destroy(&arena);
    CHECK(!blocks && !physical_live);
}

static void failure_operation_boundaries(void) {
    const uint64_t limits[] = {2, 5, 6, 9};
    const uint64_t expected[] = {2, 5, 6, 6};
    for (size_t i = 0; i < 4; ++i) {
        observer_reset();
        XrCompileResourceLimits bounded = unlimited;
        bounded.work = limits[i];
        XrCompileResources *owner = NULL;
        RESOURCE_OK(xr_compile_resources_new(&bounded, &owner));
        XrArena arena = {0};
        OK(compiler_open(&arena, 32, owner));
        CHECK(!xr_arena_strdup(&arena, "abc"));
        CHECK(xr_arena_status(&arena) == XR_ARENA_BUDGET);
        CHECK(snapshot(owner).work == expected[i] && attempts == 2);
        xr_arena_destroy(&arena);
        xr_compile_resources_release(owner);
    }
    observer_reset();
    XrCompileResourceLimits bounded = unlimited;
    bounded.work = 4;
    XrCompileResources *owner = NULL;
    RESOURCE_OK(xr_compile_resources_new(&bounded, &owner));
    XrArena arena = {0};
    OK(compiler_open(&arena, 8, owner));
    char *destination = arena.position;
    memset(destination, 0x5A, 8);
    CHECK(!xr_arena_strndup(&arena, "XY", 2));
    CHECK(destination[0] == 'X' && destination[1] == 'Y' && destination[2] == 0x5A);
    CHECK(snapshot(owner).work == 4);
    xr_arena_destroy(&arena);
    xr_compile_resources_release(owner);

    observer_reset();
    bounded.work = 2;
    owner = NULL;
    RESOURCE_OK(xr_compile_resources_new(&bounded, &owner));
    OK(compiler_open(&arena, 8, owner));
    destination = arena.position;
    memset(destination, 0x6B, 8);
    CHECK(!xr_arena_alloc(&arena, 3));
    CHECK(destination[0] == 0x6B && destination[2] == 0x6B && snapshot(owner).work == 2);
    xr_arena_destroy(&arena);
    xr_compile_resources_release(owner);
    CHECK(!blocks);
}

static void restore_and_reset(void) {
    observer_reset();
    XrCompileResources *owner = NULL;
    RESOURCE_OK(xr_compile_resources_new(&unlimited, &owner));
    XrArena arena = {0};
    OK(compiler_open(&arena, 8, owner));
    XrArenaState outer = xr_arena_save(&arena);
    CHECK(xr_arena_alloc_raw(&arena, 8));
    XrArenaState inner = xr_arena_save(&arena);
    CHECK(xr_arena_alloc_raw(&arena, 9));
    xr_arena_restore(&arena, inner);
    OK(xr_arena_status(&arena));
    CHECK(arena.total_allocated == 8 && blocks == 2);
    xr_arena_restore(&arena, outer);
    CHECK(arena.total_allocated == 0 && snapshot(owner).work == 6);
    CHECK(xr_arena_alloc_raw(&arena, 9));
    XrCompileResourceStats before = snapshot(owner);
    XrArenaState stale = xr_arena_save(&arena);
    xr_arena_reset(&arena);
    CHECK(arena.segment_count == 1 && arena.total_capacity == 65536 && !arena.total_allocated);
    CHECK(blocks == 2 && snapshot(owner).work == before.work);
    xr_arena_restore(&arena, stale);
    CHECK(xr_arena_status(&arena) == XR_ARENA_BAD_ARGUMENT && blocks == 2);
    xr_arena_destroy(&arena);
    xr_compile_resources_release(owner);

    observer_reset();
    XrCompileResourceLimits bounded = unlimited;
    bounded.work = 4;
    owner = NULL;
    RESOURCE_OK(xr_compile_resources_new(&bounded, &owner));
    OK(compiler_open(&arena, 8, owner));
    outer = xr_arena_save(&arena);
    CHECK(xr_arena_alloc_raw(&arena, 9));
    size_t live = physical_live;
    xr_arena_restore(&arena, outer);
    CHECK(xr_arena_status(&arena) == XR_ARENA_BUDGET);
    CHECK(arena.segment_count == 2 && physical_live == live && snapshot(owner).work == 4);
    xr_arena_destroy(&arena);
    xr_compile_resources_release(owner);
    CHECK(!blocks);
}

static void bad_arguments_and_system(void) {
    observer_reset();
    XrArena arena = {0};
    XrArenaBacking backing = xr_arena_system_backing(), untouched = backing;
    CHECK(xr_compiler_arena_backing(NULL, &backing) == XR_ARENA_BAD_ARGUMENT);
    CHECK(!memcmp(&backing, &untouched, sizeof(backing)));
    CHECK(xr_arena_open(NULL, 8, &backing) == XR_ARENA_BAD_ARGUMENT);
    CHECK(xr_arena_status(NULL) == XR_ARENA_BAD_ARGUMENT);
    CHECK(!xr_arena_alloc_raw(&arena, 1));
    CHECK(xr_arena_status(&arena) == XR_ARENA_BAD_ARGUMENT);
    xr_arena_destroy(&arena);
    for (unsigned point = 0; point < 5; ++point) {
        backing = untouched;
        if (point == 0) backing.alloc = NULL;
        if (point == 1) backing.free = NULL;
        if (point == 2) backing.work = NULL;
        if (point == 3) backing.retain = NULL;
        if (point == 4) backing.release = NULL;
        CHECK(xr_arena_open(&arena, 8, &backing) == XR_ARENA_BAD_ARGUMENT);
        CHECK(!arena.retained && !attempts);
        xr_arena_destroy(&arena);
    }
    backing = untouched;
    CHECK(xr_arena_open(&arena, SIZE_MAX, &backing) == XR_ARENA_BUDGET);
    CHECK(arena.retained && !attempts);
    xr_arena_destroy(&arena);
    for (unsigned point = 0; point < 5; ++point) {
        OK(xr_arena_open(&arena, 8, &backing));
        if (point == 0) CHECK(!xr_arena_alloc(&arena, 0));
        if (point == 1) CHECK(!xr_arena_alloc_array(&arena, 0, 2));
        if (point == 2) CHECK(!xr_arena_strdup(&arena, NULL));
        if (point == 3) CHECK(!xr_arena_strndup(&arena, NULL, 0));
        if (point == 4) CHECK(xr_arena_open(&arena, 8, &backing) == XR_ARENA_BAD_ARGUMENT);
        CHECK(xr_arena_status(&arena) == XR_ARENA_BAD_ARGUMENT && blocks == 1);
        xr_arena_destroy(&arena);
    }
    for (unsigned point = 0; point < 3; ++point) {
        OK(xr_arena_open(&arena, 8, &backing));
        size_t count = attempts;
        if (point == 0) CHECK(!xr_arena_alloc(&arena, SIZE_MAX));
        if (point == 1) CHECK(!xr_arena_alloc_array(&arena, SIZE_MAX, 2));
        if (point == 2) CHECK(!xr_arena_strndup(&arena, "", SIZE_MAX));
        CHECK(xr_arena_status(&arena) == XR_ARENA_BUDGET && attempts == count);
        xr_arena_destroy(&arena);
    }
    fail_at = attempts;
    CHECK(xr_arena_open(&arena, 8, &backing) == XR_ARENA_OUT_OF_MEMORY);
    xr_arena_destroy(&arena);
    fail_at = SIZE_MAX;
    OK(xr_arena_open(&arena, 0, &backing));
    CHECK(arena.total_capacity == XR_ARENA_SEGMENT_SIZE);
    XrArenaState foreign = xr_arena_save(&arena);
    foreign.owner = NULL;
    xr_arena_restore(&arena, foreign);
    CHECK(xr_arena_status(&arena) == XR_ARENA_BAD_ARGUMENT && blocks == 1);
    xr_arena_destroy(&arena);
    CHECK(!blocks);
}

static void unknown_string_guard(void) {
#if defined(XR_OS_WINDOWS)
    observer_reset();
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    size_t page = info.dwPageSize;
    char *region = VirtualAlloc(NULL, page * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(region != NULL);
    DWORD old = 0;
    CHECK(VirtualProtect(region + page, page, PAGE_NOACCESS, &old));
    memcpy(region + page - 3, "abc", 3);
    XrCompileResourceLimits limits = unlimited;
    limits.work = 5;
    XrCompileResources *owner = NULL;
    RESOURCE_OK(xr_compile_resources_new(&limits, &owner));
    XrArena arena = {0};
    OK(compiler_open(&arena, 8, owner));
    CHECK(!xr_arena_strdup(&arena, region + page - 3));
    CHECK(xr_arena_status(&arena) == XR_ARENA_BUDGET && snapshot(owner).work == 5);
    CHECK(!xr_arena_strdup(&arena, region + page));
    xr_arena_destroy(&arena);
    xr_compile_resources_release(owner);
    CHECK(VirtualFree(region, 0, MEM_RELEASE));
    CHECK(!blocks);
#endif
}

static void shared_owner_and_retain_failure(void) {
    observer_reset();
    XrCompileResources *owner = NULL;
    RESOURCE_OK(xr_compile_resources_new(&unlimited, &owner));
    XrArena a = {0}, b = {0};
    OK(compiler_open(&a, 8, owner));
    OK(compiler_open(&b, 16, owner));
    CHECK(a.backing.context == b.backing.context && blocks == 3);
    CHECK(xr_arena_strdup(&a, "a") && xr_arena_strdup(&b, "b"));
    /* Ledger + two segments = 3, two strings each scan/copy 2+2 = 8. */
    CHECK(snapshot(owner).work == 11);
    XrCompileResourceStats before = snapshot(owner);
    xr_arena_destroy(&a);
    CHECK(blocks == 2 && snapshot(owner).allocated_bytes == before.allocated_bytes);
    OK(compiler_open(&a, 8, owner));
    CHECK(snapshot(owner).work == 12 && snapshot(owner).allocated_bytes > before.allocated_bytes);
    xr_compile_resources_release(owner);
    owner = NULL;
    xr_arena_destroy(&b);
    CHECK(blocks == 2 && xr_arena_strdup(&a, "kept"));
    xr_arena_destroy(&a);
    CHECK(!blocks);

    observer_reset();
    RESOURCE_OK(xr_compile_resources_new(&unlimited, &owner));
    /* Inject the unreachable reference limit into the actual production
     * ledger, then restore it before releasing the real single reference. */
    owner->references = UINT64_MAX;
    CHECK(compiler_open(&a, 8, owner) == XR_ARENA_BUDGET);
    CHECK(!a.retained && !a.head && attempts == 1 && owner->references == UINT64_MAX);
    xr_arena_destroy(&a);
    CHECK(owner->references == UINT64_MAX);
    owner->references = 1;
    CHECK(compiler_open(&a, SIZE_MAX, owner) == XR_ARENA_BUDGET);
    CHECK(a.retained && !a.head && attempts == 1 && owner->references == 2);
    xr_compile_resources_release(owner);
    CHECK(blocks == 1);
    xr_arena_destroy(&a);
    CHECK(!blocks);
}

typedef struct EmbeddedArenaOwner {
    size_t references;
    XrArena arena;
} EmbeddedArenaOwner;

static XrArenaStatus embedded_retain(void *context) {
    EmbeddedArenaOwner *owner = context;
    ++owner->references;
    return XR_ARENA_OK;
}

static void embedded_release(void *context) {
    EmbeddedArenaOwner *owner = context;
    if (!--owner->references) observed_free(owner);
}

static void embedded_context_lifetime(void) {
    observer_reset();
    EmbeddedArenaOwner *owner = observed_allocate(sizeof(*owner));
    CHECK(owner != NULL);
    *owner = (EmbeddedArenaOwner) {1, {0}};
    XrArenaBacking backing = xr_arena_system_backing();
    backing.context = owner;
    backing.retain = embedded_retain;
    backing.release = embedded_release;
    OK(xr_arena_open(&owner->arena, 8, &backing));
    memset(&backing, 0xA5, sizeof(backing));
    embedded_release(owner);
    CHECK(owner->references == 1 && blocks == 2);
    CHECK(xr_arena_strdup(&owner->arena, "owned") != NULL);
    xr_arena_destroy(&owner->arena);
    CHECK(!blocks && !physical_live);
}

int main(void) {
    exact_limits_and_oom();
    sticky_failure_and_owner();
    failure_operation_boundaries();
    restore_and_reset();
    bad_arguments_and_system();
    unknown_string_guard();
    shared_owner_and_retain_failure();
    embedded_context_lifetime();
    puts("compiler arena: work=28, exact/minus-one, all five allocation faults, rewind, guard, physical zero passed");
    return 0;
}
