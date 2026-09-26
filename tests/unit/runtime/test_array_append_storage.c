/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_array_append_storage.c - Append rollback, aliases and physical backing release
 */
#include "base/xmalloc.h"
#include "runtime/core/xr_array_append_storage.h"
#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "requirement failed at line %d: %s\n", __LINE__, #condition); \
    abort(); } } while (0)

typedef struct Probe { size_t calls, fail_at, live; } Probe;
static void *allocate(void *context, size_t bytes) {
    Probe *probe = context;
    if (++probe->calls == probe->fail_at) return NULL;
    void *result = xr_malloc(bytes);
    if (result) ++probe->live;
    return result;
}
static void release(void *context, void *pointer) {
    Probe *probe = context;
    REQUIRE(pointer && probe->live);
    --probe->live;
    xr_free(pointer);
}

static void growth_and_failure(void) {
    Probe probe = {0};
    XrArrayAppendAllocator allocator = {&probe, allocate, release};
    XrArrayAppendStorage storage = {0};
    for (uint32_t i = 0u; i < 257u; ++i) {
        uint64_t value = (uint64_t) i * 19u;
        XrArrayAppendStorage before = storage;
        size_t old_calls = probe.calls;
        if (storage.length == storage.capacity) {
            probe.fail_at = probe.calls + 1u;
            REQUIRE(xr_array_append_storage(&storage, sizeof(value), &value, 512u,
                SIZE_MAX, &allocator) == XR_ARRAY_APPEND_RESOURCE_LIMIT);
            REQUIRE(storage.data == before.data && storage.length == before.length &&
                    storage.capacity == before.capacity);
            REQUIRE(probe.live == (before.data ? 1u : 0u));
            probe.fail_at = 0u;
        }
        REQUIRE(xr_array_append_storage(&storage, sizeof(value), &value, 512u,
            SIZE_MAX, &allocator) == XR_ARRAY_APPEND_OK);
        REQUIRE(storage.length == i + 1u && storage.capacity >= storage.length);
        REQUIRE(probe.live == 1u);
        if (before.length < before.capacity) REQUIRE(probe.calls == old_calls);
        for (uint32_t j = 0u; j <= i; ++j)
            REQUIRE(((uint64_t *) storage.data)[j] == (uint64_t) j * 19u);
    }
    REQUIRE(probe.calls == 16u); /* Eight rejected and eight successful growths. */
    release(&probe, storage.data);
    REQUIRE(probe.live == 0u);
}

static void aliases_and_boundaries(void) {
    Probe probe = {0};
    XrArrayAppendAllocator allocator = {&probe, allocate, release};
    XrArrayAppendStorage storage = {0};
    uint64_t first = 42u;
    REQUIRE(xr_array_append_storage(&storage, sizeof(first), &first, 8u, 64u,
        &allocator) == XR_ARRAY_APPEND_OK);
    for (uint32_t i = 1u; i < 8u; ++i) {
        if (storage.length == storage.capacity) {
            void *previous = storage.data;
            probe.fail_at = probe.calls + 1u;
            REQUIRE(xr_array_append_storage(&storage, sizeof(first), storage.data, 8u, 64u,
                &allocator) == XR_ARRAY_APPEND_RESOURCE_LIMIT);
            REQUIRE(storage.data == previous && storage.length == i && probe.live == 1u);
            REQUIRE(((uint64_t *) storage.data)[0] == 42u);
            probe.fail_at = 0u;
        }
        REQUIRE(xr_array_append_storage(&storage, sizeof(first), storage.data, 8u, 64u,
            &allocator) == XR_ARRAY_APPEND_OK);
    }
    for (uint32_t i = 0u; i < 8u; ++i) REQUIRE(((uint64_t *) storage.data)[i] == 42u);
    size_t calls = probe.calls;
    REQUIRE(xr_array_append_storage(&storage, sizeof(first), &first, 9u, 64u,
        &allocator) == XR_ARRAY_APPEND_RESOURCE_LIMIT);
    REQUIRE(probe.calls == calls && storage.length == 8u && probe.live == 1u);
    REQUIRE(xr_array_append_storage(&storage, sizeof(first), (uint8_t *) storage.data + 1u,
        16u, 128u, &allocator) == XR_ARRAY_APPEND_INVALID);
    REQUIRE(xr_array_append_storage(&storage, sizeof(first), (uint8_t *) storage.data + 64u,
        16u, 128u, &allocator) == XR_ARRAY_APPEND_INVALID);
    release(&probe, storage.data);
    REQUIRE(probe.live == 0u);
    XrArrayAppendStorage empty = {0};
    REQUIRE(xr_array_append_storage(&empty, SIZE_MAX, &first, 1u, SIZE_MAX - 1u,
        &allocator) == XR_ARRAY_APPEND_RESOURCE_LIMIT);
    REQUIRE(probe.calls == calls);
    XrArrayAppendStorage unit = {NULL, UINT32_MAX - 1u, UINT32_MAX};
    REQUIRE(xr_array_append_storage(&unit, 0u, NULL, UINT32_MAX, 0u,
        &allocator) == XR_ARRAY_APPEND_OK);
    REQUIRE(unit.length == UINT32_MAX && !unit.data);
    REQUIRE(xr_array_append_storage(&unit, 0u, NULL, UINT32_MAX, 0u,
        &allocator) == XR_ARRAY_APPEND_RESOURCE_LIMIT);
    REQUIRE(probe.calls == calls);
}

int main(void) {
    growth_and_failure();
    aliases_and_boundaries();
    return 0;
}
