/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_compile_resources.c - Independently observe allocation and refund bounds
 */
#include "base/xcompile_resources.h"
#include "base/xchecks.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_COMPILE_RESOURCE_OK)

typedef struct AllocationRecord { void *pointer; size_t bytes; } AllocationRecord;
static AllocationRecord records[32];
static size_t attempts, fail_at = SIZE_MAX, live_count, physical_bytes, physical_peak, physical_total, last_bytes;

static void *observed_allocate(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes);
    CHECK(memory != NULL);
    size_t index = 0;
    while (index < 32 && records[index].pointer) ++index;
    CHECK(index < 32);
    records[index] = (AllocationRecord) {memory, bytes};
    ++live_count;
    physical_bytes += bytes;
    physical_total += bytes;
    last_bytes = bytes;
    if (physical_bytes > physical_peak) physical_peak = physical_bytes;
    return memory;
}

static void observed_free(void *memory) {
    if (!memory) return;
    size_t index = 0;
    while (index < 32 && records[index].pointer != memory) ++index;
    CHECK(index < 32);
    size_t bytes = records[index].bytes;
    xr_free(memory);
    records[index] = (AllocationRecord) {0};
    --live_count;
    physical_bytes -= bytes;
}

#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) observed_allocate(bytes)
#define xr_free(memory) observed_free(memory)
#include "base/xcompile_resources.c"

static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};

static void reset_observer(void) {
    CHECK(live_count == 0 && physical_bytes == 0);
    attempts = physical_peak = physical_total = last_bytes = 0;
    fail_at = SIZE_MAX;
}

static XrCompileResourceStats snapshot(XrCompileResources *resources) {
    XrCompileResourceStats stats;
    OK(xr_compile_resources_stats(resources, &stats));
    CHECK(stats.live_bytes == physical_bytes);
    CHECK(stats.peak_bytes == physical_peak);
    CHECK(stats.allocated_bytes == physical_total);
    return stats;
}

static void creation_and_arguments(void) {
    reset_observer();
    XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(NULL, &owner) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_new(&unlimited, NULL) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    OK(xr_compile_resources_new(&unlimited, &owner));
    XrCompileResources *saved = owner;
    CHECK(xr_compile_resources_new(&unlimited, &owner) == XR_COMPILE_RESOURCE_BAD_ARGUMENT && owner == saved);
    XrCompileResourceStats baseline = snapshot(owner), untouched = baseline;
    CHECK(baseline.allocation_count == 1 && baseline.work == 1 && baseline.live_bytes == last_bytes);
    CHECK(xr_compile_resources_stats(NULL, &untouched) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(!memcmp(&untouched, &baseline, sizeof(baseline)));
    CHECK(xr_compile_resources_stats(owner, NULL) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_retain(NULL) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_work(NULL, 1) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    void *memory = NULL;
    CHECK(xr_compile_resources_alloc(owner, 0, &memory) == XR_COMPILE_RESOURCE_BAD_ARGUMENT && !memory);
    CHECK(xr_compile_resources_alloc(owner, 8, NULL) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_alloc(NULL, 8, &memory) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_calloc(owner, 0, 8, &memory) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_calloc(owner, 8, 0, &memory) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_resize(owner, NULL, 8) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_resize(NULL, &memory, 8) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    OK(xr_compile_resources_resize(owner, &memory, 0));
    memory = (void *) (uintptr_t) 17;
    CHECK(xr_compile_resources_alloc(owner, 8, &memory) == XR_COMPILE_RESOURCE_BAD_ARGUMENT && memory == (void *) (uintptr_t) 17);
    CHECK(xr_compile_resources_calloc(owner, 1, 8, &memory) == XR_COMPILE_RESOURCE_BAD_ARGUMENT && memory == (void *) (uintptr_t) 17);
    CHECK(attempts == 1);
    xr_compile_resources_release(owner);
    CHECK(!live_count && !physical_bytes);
    for (unsigned field = 0; field < 3; ++field) {
        reset_observer();
        XrCompileResourceLimits limits = {baseline.allocated_bytes, baseline.live_bytes, 1};
        if (field == 0) --limits.allocated_bytes;
        if (field == 1) --limits.live_bytes;
        if (field == 2) --limits.work;
        owner = NULL;
        CHECK(xr_compile_resources_new(&limits, &owner) == XR_COMPILE_RESOURCE_BUDGET && !owner && !attempts);
    }
    xr_compile_resources_release(NULL);
    xr_compile_resources_free(NULL);
}

static XrCompileResourceStatus pipeline(const XrCompileResourceLimits *limits, XrCompileResourceStats *result) {
    XrCompileResources *owner = NULL;
    void *memory = NULL;
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &owner);
    if (status == XR_COMPILE_RESOURCE_OK) status = xr_compile_resources_alloc(owner, 8, &memory);
    if (status == XR_COMPILE_RESOURCE_OK) {
        xr_compile_resources_free(memory);
        memory = NULL;
        status = xr_compile_resources_calloc(owner, 3, 4, &memory);
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        for (size_t i = 0; i < 12; ++i) CHECK(((unsigned char *) memory)[i] == 0);
        memset(memory, 0xA5, 12);
        void *saved = memory;
        status = xr_compile_resources_resize(owner, &memory, 20);
        if (status != XR_COMPILE_RESOURCE_OK) CHECK(memory == saved);
        for (size_t i = 0; i < 12; ++i) CHECK(((unsigned char *) memory)[i] == 0xA5);
    }
    if (status == XR_COMPILE_RESOURCE_OK) status = xr_compile_resources_work(owner, 7);
    if (owner) {
        *result = snapshot(owner);
        xr_compile_resources_free(memory);
        XrCompileResourceStats refunded = snapshot(owner);
        CHECK(refunded.allocated_bytes == result->allocated_bytes && refunded.work == result->work);
        CHECK(refunded.allocation_count == result->allocation_count && live_count == 1);
        xr_compile_resources_release(owner);
    }
    CHECK(live_count == 0 && physical_bytes == 0);
    return status;
}

static void exact_and_faults(void) {
    reset_observer();
    XrCompileResourceStats measured = {0};
    OK(pipeline(&unlimited, &measured));
    CHECK(attempts == 4 && measured.allocation_count == 4 && measured.work == 35);
    CHECK(measured.allocated_bytes > measured.peak_bytes);
    XrCompileResourceLimits exact = {measured.allocated_bytes, measured.peak_bytes, 35};
    reset_observer();
    XrCompileResourceStats result;
    OK(pipeline(&exact, &result));
    CHECK(!memcmp(&result, &measured, sizeof(result)));
    for (unsigned field = 0; field < 3; ++field) {
        reset_observer();
        XrCompileResourceLimits smaller = exact;
        if (field == 0) --smaller.allocated_bytes;
        if (field == 1) --smaller.live_bytes;
        if (field == 2) --smaller.work;
        CHECK(pipeline(&smaller, &result) == XR_COMPILE_RESOURCE_BUDGET);
    }
    const uint64_t work_on_oom[] = {0, 2, 3, 16};
    for (size_t point = 0; point < 4; ++point) {
        reset_observer();
        fail_at = point;
        result = (XrCompileResourceStats) {0};
        CHECK(pipeline(&unlimited, &result) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
        CHECK(attempts == point + 1 && result.work == work_on_oom[point]);
        CHECK(result.allocation_count == point);
    }
}

static void resize_and_transfer(void) {
    reset_observer();
    XrCompileResources *producer = NULL;
    OK(xr_compile_resources_new(&unlimited, &producer));
    uint64_t owner_bytes = last_bytes;
    void *memory = NULL;
    OK(xr_compile_resources_resize(producer, &memory, 24));
    CHECK((uintptr_t) memory % _Alignof(long double) == 0);
    CHECK((uintptr_t) memory % _Alignof(uint64_t) == 0);
    CHECK((uintptr_t) memory % _Alignof(void *) == 0);
    memset(memory, 0x5A, 24);
    size_t old_allocation = last_bytes;
    void *saved = memory;
    OK(xr_compile_resources_resize(producer, &memory, 24));
    CHECK(memory == saved && attempts == 2);
    OK(xr_compile_resources_resize(producer, &memory, 4));
    XrCompileResourceStats stats = snapshot(producer);
    CHECK(stats.peak_bytes == owner_bytes + old_allocation + last_bytes);
    CHECK(stats.work == 7 && stats.allocation_count == 3);
    for (size_t i = 0; i < 4; ++i) CHECK(((unsigned char *) memory)[i] == 0x5A);
    XrCompileResources *consumer = producer;
    OK(xr_compile_resources_retain(consumer));
    xr_compile_resources_release(producer);
    producer = NULL;
    OK(xr_compile_resources_work(consumer, 9));
    xr_compile_resources_release(consumer);
    consumer = NULL;
    CHECK(live_count == 2);
    for (size_t i = 0; i < 4; ++i) CHECK(((unsigned char *) memory)[i] == 0x5A);
    xr_compile_resources_free(memory);
    CHECK(!live_count && !physical_bytes);

    reset_observer();
    OK(xr_compile_resources_new(&unlimited, &producer));
    OK(xr_compile_resources_new(&unlimited, &consumer));
    memory = NULL;
    OK(xr_compile_resources_alloc(producer, 4, &memory));
    saved = memory;
    CHECK(xr_compile_resources_resize(consumer, &memory, 8) == XR_COMPILE_RESOURCE_BAD_ARGUMENT && memory == saved);
    CHECK(xr_compile_resources_resize(consumer, &memory, 0) == XR_COMPILE_RESOURCE_BAD_ARGUMENT && memory == saved);
    OK(xr_compile_resources_resize(producer, &memory, 0));
    CHECK(!memory && live_count == 2);
    xr_compile_resources_release(producer);
    xr_compile_resources_release(consumer);
    CHECK(!live_count && !physical_bytes);
}

static void overflow_and_recovery(void) {
    reset_observer();
    XrCompileResources *owner = NULL;
    OK(xr_compile_resources_new(&unlimited, &owner));
    void *memory = NULL;
    CHECK(xr_compile_resources_alloc(owner, SIZE_MAX, &memory) == XR_COMPILE_RESOURCE_BUDGET && !memory);
    CHECK(xr_compile_resources_calloc(owner, SIZE_MAX, 2, &memory) == XR_COMPILE_RESOURCE_BUDGET && !memory);
    CHECK(attempts == 1);
    fail_at = attempts;
    CHECK(xr_compile_resources_alloc(owner, 4, &memory) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !memory);
    XrCompileResourceStats stats = snapshot(owner);
    CHECK(stats.work == 2 && stats.allocation_count == 1);
    fail_at = SIZE_MAX;
    OK(xr_compile_resources_alloc(owner, 4, &memory));
    memset(memory, 0x39, 4);
    void *saved = memory;
    CHECK(xr_compile_resources_resize(owner, &memory, SIZE_MAX) == XR_COMPILE_RESOURCE_BUDGET && memory == saved);
    fail_at = attempts;
    CHECK(xr_compile_resources_resize(owner, &memory, 8) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && memory == saved);
    for (size_t i = 0; i < 4; ++i) CHECK(((unsigned char *) memory)[i] == 0x39);
    CHECK(snapshot(owner).work == 4);
    OK(xr_compile_resources_work(owner, UINT64_MAX - 4));
    CHECK(xr_compile_resources_work(owner, 1) == XR_COMPILE_RESOURCE_BUDGET);
    void *empty = NULL;
    CHECK(xr_compile_resources_alloc(owner, 8, &empty) == XR_COMPILE_RESOURCE_BUDGET && !empty);
    xr_compile_resources_free(memory);
    CHECK(snapshot(owner).work == UINT64_MAX);
    xr_compile_resources_release(owner);
    CHECK(!live_count && !physical_bytes);
}

static void operation_work_limits(void) {
    for (unsigned exact = 0; exact < 2; ++exact) {
        reset_observer();
        XrCompileResourceLimits limits = unlimited;
        limits.work = 5 + exact;
        XrCompileResources *owner = NULL;
        void *memory = NULL;
        OK(xr_compile_resources_new(&limits, &owner));
        XrCompileResourceStatus status = xr_compile_resources_calloc(owner, 2, 2, &memory);
        CHECK(status == (exact ? XR_COMPILE_RESOURCE_OK : XR_COMPILE_RESOURCE_BUDGET));
        CHECK(attempts == (exact ? 2u : 1u));
        CHECK(snapshot(owner).work == (exact ? 6u : 1u));
        xr_compile_resources_free(memory);
        xr_compile_resources_release(owner);
    }
    for (unsigned exact = 0; exact < 2; ++exact) {
        reset_observer();
        XrCompileResourceLimits limits = unlimited;
        limits.work = 6 + exact;
        XrCompileResources *owner = NULL;
        void *memory = NULL;
        OK(xr_compile_resources_new(&limits, &owner));
        OK(xr_compile_resources_alloc(owner, 4, &memory));
        memset(memory, 0x72, 4);
        void *saved = memory;
        XrCompileResourceStatus status = xr_compile_resources_resize(owner, &memory, 9);
        CHECK(status == (exact ? XR_COMPILE_RESOURCE_OK : XR_COMPILE_RESOURCE_BUDGET));
        CHECK(exact || memory == saved);
        for (size_t i = 0; i < 4; ++i) CHECK(((unsigned char *) memory)[i] == 0x72);
        CHECK(attempts == (exact ? 3u : 2u));
        CHECK(snapshot(owner).work == (exact ? 7u : 2u));
        xr_compile_resources_free(memory);
        xr_compile_resources_release(owner);
    }
}

static void counter_overflow_guards(void) {
    reset_observer();
    XrCompileResources *owner = NULL;
    OK(xr_compile_resources_new(&unlimited, &owner));
    XrCompileResourceStats saved = snapshot(owner);
    void *memory = NULL;
    /* Reach otherwise impractical integer boundaries without allocating
     * exabytes. Restore the real counters before physical release. */
    owner->stats.allocated_bytes = UINT64_MAX - 1;
    CHECK(xr_compile_resources_alloc(owner, 4, &memory) == XR_COMPILE_RESOURCE_BUDGET && !memory);
    owner->stats = saved;
    owner->stats.live_bytes = UINT64_MAX - 1;
    CHECK(xr_compile_resources_alloc(owner, 4, &memory) == XR_COMPILE_RESOURCE_BUDGET && !memory);
    owner->stats = saved;
    owner->stats.allocation_count = UINT64_MAX;
    CHECK(xr_compile_resources_alloc(owner, 4, &memory) == XR_COMPILE_RESOURCE_BUDGET && !memory);
    owner->stats = saved;
    owner->references = UINT64_MAX;
    CHECK(xr_compile_resources_alloc(owner, 4, &memory) == XR_COMPILE_RESOURCE_BUDGET && !memory);
    CHECK(xr_compile_resources_retain(owner) == XR_COMPILE_RESOURCE_BUDGET);
    owner->references = 1;
    CHECK(attempts == 1 && snapshot(owner).work == 1);
    xr_compile_resources_release(owner);
    CHECK(!live_count && !physical_bytes);
}

int main(void) {
    creation_and_arguments();
    exact_and_faults();
    resize_and_transfer();
    overflow_and_recovery();
    operation_work_limits();
    counter_overflow_guards();
    puts("compile resources: exact limits, allocation faults, transfer and physical zero passed");
    return 0;
}
