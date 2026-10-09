/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_sysheap_allocations.c - Heap startup failures release exact owners
 */
#include "xr_sysheap_allocation_probe.h"
#include "runtime/mem/xsystem_heap.h"
#include "coro/xcoro_pool.h"
#include "coro/xcoroutine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

typedef struct Allocation {
    void *memory;
    size_t bytes;
    size_t ordinal;
} Allocation;

/* Independent layout model for the physical segment header. */
typedef struct SegmentModel {
    void *next;
    size_t used;
    size_t capacity;
    uint64_t serial;
} SegmentModel;

static Allocation allocations[8];
static xr_mutex_t *mutexes[4];
static size_t attempts, fail_at, blocks, live_bytes;
static size_t released[8], release_count, mutex_inits, mutex_destroys;
static const XrSysHeapConfig config = {2, 16, false};

static const char *source_name(const char *source) {
    const char *name = source;
    for (; *source; ++source) if (*source == '/' || *source == '\\') name = source + 1;
    return name;
}

static bool allocation_admitted(size_t count, size_t bytes, bool zeroed, const char *source) {
    ++attempts;
    CHECK(count && bytes && count <= SIZE_MAX / bytes);
    const char *name = source_name(source);
    switch (attempts) {
    case 1:
        CHECK(!zeroed && count == 1 && bytes == sizeof(XrCoroStructPool));
        CHECK(!strcmp(name, "xsystem_heap.c"));
        break;
    case 2:
        CHECK(!zeroed && count == 1 && bytes == sizeof(XrCoroPoolBlock));
        CHECK(!strcmp(name, "xcoro_pool.c"));
        break;
    case 3:
        CHECK(zeroed && count == config.coro_pool_init_size && bytes == sizeof(XrCoroutine));
        CHECK(!strcmp(name, "xcoro_pool.c"));
        break;
    case 4:
        CHECK(!zeroed && count == 1 && bytes == sizeof(SegmentModel) + config.class_arena_init_size);
        CHECK(!strcmp(name, "xarena_backing.c"));
        break;
    case 5:
        CHECK(!zeroed && count == 1 && bytes == sizeof(SegmentModel) + XR_ARENA_SEGMENT_SIZE);
        CHECK(!strcmp(name, "xarena_backing.c"));
        break;
    default:
        CHECK(attempts <= 5);
        break;
    }
    return attempts != fail_at;
}

static void *allocation_record(void *memory, size_t bytes) {
    CHECK(memory != NULL);
    size_t index = 0;
    while (index < 8 && allocations[index].memory) ++index;
    CHECK(index < 8 && live_bytes <= SIZE_MAX - bytes);
    allocations[index] = (Allocation) {memory, bytes, attempts};
    ++blocks;
    live_bytes += bytes;
    return memory;
}

XR_FUNC void *xr_sysheap_test_malloc(size_t bytes, const char *source) {
    if (!allocation_admitted(1, bytes, false, source)) return NULL;
    return allocation_record(xr_malloc(bytes), bytes);
}

XR_FUNC void *xr_sysheap_test_calloc(size_t count, size_t bytes, const char *source) {
    if (!allocation_admitted(count, bytes, true, source)) return NULL;
    return allocation_record(xr_calloc(count, bytes), count * bytes);
}

XR_FUNC void xr_sysheap_test_free(void *memory) {
    if (!memory) return;
    size_t index = 0;
    while (index < 8 && allocations[index].memory != memory) ++index;
    CHECK(index < 8 && release_count < 8 && blocks && live_bytes >= allocations[index].bytes);
    released[release_count++] = allocations[index].ordinal;
    live_bytes -= allocations[index].bytes;
    --blocks;
    allocations[index] = (Allocation) {0};
    xr_free(memory);
}

XR_FUNC void xr_sysheap_test_mutex_init(xr_mutex_t *mutex) {
    size_t empty = 4;
    for (size_t i = 0; i < 4; ++i) {
        CHECK(mutexes[i] != mutex);
        if (!mutexes[i] && empty == 4) empty = i;
    }
    CHECK(empty < 4);
    xr_mutex_init(mutex);
    mutexes[empty] = mutex;
    ++mutex_inits;
}

XR_FUNC void xr_sysheap_test_mutex_destroy(xr_mutex_t *mutex) {
    size_t index = 0;
    while (index < 4 && mutexes[index] != mutex) ++index;
    CHECK(index < 4);
    xr_mutex_destroy(mutex);
    mutexes[index] = NULL;
    ++mutex_destroys;
}

static void physically_empty(void) {
    CHECK(!blocks && !live_bytes && mutex_inits == mutex_destroys);
    for (size_t i = 0; i < 8; ++i) CHECK(!allocations[i].memory);
    for (size_t i = 0; i < 4; ++i) CHECK(!mutexes[i]);
}

static void reset_observer(size_t failure) {
    physically_empty();
    attempts = release_count = mutex_inits = mutex_destroys = 0;
    memset(released, 0, sizeof(released));
    fail_at = failure;
}

static void releases_equal(const size_t *expected, size_t count) {
    CHECK(release_count == count);
    for (size_t i = 0; i < count; ++i) CHECK(released[i] == expected[i]);
}

static void unpublished(const XrSystemHeap *heap) {
    CHECK(!heap->initialized && !heap->coro_pool);
    CHECK(!heap->class_arena.head && !heap->class_arena.retained);
    CHECK(!heap->class_arena.total_capacity && !heap->class_arena.total_allocated);
}

static void successful_lifetime(XrSystemHeap *heap, bool grow) {
    static const size_t plain[] = {3, 2, 1, 4};
    static const size_t grown[] = {3, 2, 1, 5, 4};
    reset_observer(SIZE_MAX);
    CHECK(xr_sysheap_init(heap, &config));
    CHECK(attempts == 4 && blocks == 4 && mutex_inits == 3 && !mutex_destroys);
    CHECK(heap->initialized && heap->coro_pool && heap->class_arena.retained);
    size_t expected = sizeof(XrCoroStructPool) + sizeof(XrCoroPoolBlock) +
        config.coro_pool_init_size * sizeof(XrCoroutine) + sizeof(SegmentModel) + 16;
    CHECK(live_bytes == expected);
    uint64_t *first = xr_sysheap_alloc_class(heap, sizeof(*first));
    uint64_t *second = xr_sysheap_alloc_module(heap, sizeof(*second));
    CHECK(first && second && !*first && !*second && attempts == 4);
    *first = UINT64_C(37); *second = UINT64_C(91);
    if (grow) {
        uint64_t *third = xr_sysheap_alloc_class(heap, sizeof(*third));
        CHECK(third && !*third && attempts == 5 && blocks == 5);
        CHECK(live_bytes == expected + sizeof(SegmentModel) + XR_ARENA_SEGMENT_SIZE);
        *third = UINT64_C(73);
        CHECK(*third == UINT64_C(73));
    }
    CHECK(*first == UINT64_C(37) && *second == UINT64_C(91));
    xr_sysheap_destroy_coro_storage(heap);
    CHECK(heap->initialized && !heap->coro_pool && heap->class_arena.retained);
    CHECK(*first == UINT64_C(37) && *second == UINT64_C(91));
    CHECK(release_count == 3 && mutex_destroys == 1);
    xr_sysheap_destroy(heap);
    unpublished(heap);
    physically_empty();
    CHECK(mutex_inits == 3 && mutex_destroys == 3);
    releases_equal(grow ? grown : plain, grow ? 5 : 4);
    size_t frees = release_count;
    xr_sysheap_destroy(heap);
    CHECK(release_count == frees);
}

static void init_failures(void) {
    static const size_t expected[4][3] = {{0, 0, 0}, {1, 0, 0}, {2, 1, 0}, {3, 2, 1}};
    XrSystemHeap heap;
    for (size_t failure = 1; failure <= 4; ++failure) {
        reset_observer(failure);
        memset(&heap, 0xa5, sizeof(heap));
        CHECK(!xr_sysheap_init(&heap, &config));
        CHECK(attempts == failure);
        unpublished(&heap);
        physically_empty();
        releases_equal(expected[failure - 1], failure - 1);
        CHECK(mutex_inits == (failure == 4 ? 1u : 0u));
        xr_sysheap_destroy_coro_storage(&heap);
        xr_sysheap_destroy(&heap);
        xr_sysheap_destroy(&heap);
        releases_equal(expected[failure - 1], failure - 1);
        physically_empty();
        successful_lifetime(&heap, false);
    }
}

static void size_failure(void) {
    static const size_t expected[] = {3, 2, 1};
    reset_observer(SIZE_MAX);
    XrSystemHeap heap;
    XrSysHeapConfig oversized = config;
    oversized.class_arena_init_size = SIZE_MAX;
    CHECK(!xr_sysheap_init(&heap, &oversized));
    CHECK(attempts == 3 && mutex_inits == 1);
    unpublished(&heap);
    physically_empty();
    releases_equal(expected, 3);
    xr_sysheap_destroy(&heap);
    releases_equal(expected, 3);
    successful_lifetime(&heap, false);
}

static void growth_failure(void) {
    static const size_t expected[] = {3, 2, 1, 4};
    reset_observer(5);
    XrSystemHeap heap;
    CHECK(xr_sysheap_init(&heap, &config));
    uint64_t *saved = xr_sysheap_alloc_class(&heap, 16);
    CHECK(saved != NULL);
    *saved = UINT64_C(73);
    size_t prior_bytes = live_bytes;
    CHECK(!xr_sysheap_alloc_module(&heap, 8));
    CHECK(attempts == 5 && blocks == 4 && live_bytes == prior_bytes && !release_count);
    CHECK(xr_arena_status(&heap.class_arena) == XR_ARENA_OUT_OF_MEMORY);
    CHECK(!xr_sysheap_alloc_class(&heap, 8) && attempts == 5);
    CHECK(*saved == UINT64_C(73));
    xr_sysheap_destroy(&heap);
    unpublished(&heap);
    physically_empty();
    releases_equal(expected, 4);
}

int main(void) {
    reset_observer(SIZE_MAX);
    CHECK(!xr_sysheap_init(NULL, &config) && !attempts);
    XrSystemHeap heap;
    successful_lifetime(&heap, false);
    successful_lifetime(&heap, true);
    init_failures();
    size_failure();
    growth_failure();
    puts("system heap: four real init failures, growth failure and physical ownership PASS");
    return 0;
}
