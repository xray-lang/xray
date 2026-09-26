/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_string_builder_storage.c - Owned text, snapshots and rejected allocation
 */
#include "base/xmalloc.h"
#include "runtime/core/xr_string_builder_storage.h"
#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "requirement failed at line %d: %s\n", __LINE__, #condition); \
    abort(); } } while (0)

typedef struct Probe {
    size_t calls, fail_at, live;
} Probe;

static void *allocate(void *context, size_t size) {
    Probe *probe = context;
    if (++probe->calls == probe->fail_at)
        return NULL;
    void *result = xr_malloc(size);
    if (result) ++probe->live;
    return result;
}

static void release(void *context, void *pointer) {
    Probe *probe = context;
    REQUIRE(pointer && probe->live);
    --probe->live;
    xr_free(pointer);
}

static void test_failure_and_aliasing(void) {
    Probe probe = {0};
    XrStringBuilderAllocator allocator = {&probe, allocate, release};
    XrStringBuilderStorage storage = {0};
    const uint8_t text[] = {'A', 0, 0xc3, 0xa9, 0xf0, 0x9f, 0x98, 0x80};
    probe.fail_at = 1u;
    REQUIRE(xr_string_builder_append(&storage, text, sizeof(text), 1024u, &allocator) ==
            XR_STRING_BUILDER_RESOURCE_LIMIT);
    REQUIRE(storage.bytes == NULL && probe.live == 0u);
    probe.fail_at = 0u;
    REQUIRE(xr_string_builder_append(&storage, text, sizeof(text), 1024u, &allocator) ==
            XR_STRING_BUILDER_OK);
    REQUIRE(storage.size == 8u && storage.scalar_count == 4u);
    uint8_t *snapshot = NULL;
    probe.fail_at = probe.calls + 1u;
    REQUIRE(xr_string_builder_snapshot(&storage, 1024u, &allocator, &snapshot) ==
            XR_STRING_BUILDER_RESOURCE_LIMIT && snapshot == NULL);
    probe.fail_at = 0u;
    REQUIRE(xr_string_builder_snapshot(&storage, 1024u, &allocator, &snapshot) ==
            XR_STRING_BUILDER_OK);
    REQUIRE(snapshot != storage.bytes && memcmp(snapshot, text, sizeof(text)) == 0);
    while (storage.size < storage.capacity)
        REQUIRE(xr_string_builder_append(&storage, storage.bytes, storage.size, 1024u,
                                          &allocator) == XR_STRING_BUILDER_OK);
    XrStringBuilderStorage before = storage;
    probe.fail_at = probe.calls + 1u;
    REQUIRE(xr_string_builder_append(&storage, storage.bytes, storage.size, 1024u, &allocator) ==
            XR_STRING_BUILDER_RESOURCE_LIMIT);
    REQUIRE(storage.bytes == before.bytes && storage.capacity == before.capacity &&
            storage.size == before.size && storage.scalar_count == before.scalar_count);
    REQUIRE(memcmp(storage.bytes, text, sizeof(text)) == 0);
    probe.fail_at = 0u;
    REQUIRE(xr_string_builder_append(&storage, storage.bytes, storage.size, 1024u, &allocator) ==
            XR_STRING_BUILDER_OK);
    REQUIRE(storage.bytes != before.bytes && storage.size == 128u && storage.scalar_count == 64u);
    REQUIRE(memcmp(storage.bytes, storage.bytes + 64u, 64u) == 0);
    REQUIRE(xr_string_builder_clear(&storage));
    REQUIRE(storage.size == 0u && storage.scalar_count == 0u && storage.capacity == 128u);
    REQUIRE(memcmp(snapshot, text, sizeof(text)) == 0);
    xr_string_builder_dispose(&storage, &allocator);
    REQUIRE(memcmp(snapshot, text, sizeof(text)) == 0);
    release(&probe, snapshot);
    xr_string_builder_dispose(&storage, &allocator);
    REQUIRE(probe.live == 0u);
    probe.fail_at = probe.calls + 1u;
    REQUIRE(xr_string_builder_snapshot(&storage, 1u, &allocator, &snapshot) ==
            XR_STRING_BUILDER_RESOURCE_LIMIT);
    probe.fail_at = 0u;
    REQUIRE(xr_string_builder_snapshot(&storage, 1u, &allocator, &snapshot) == XR_STRING_BUILDER_OK);
    REQUIRE(snapshot != NULL);
    release(&probe, snapshot);
    REQUIRE(probe.live == 0u);
}

static void test_exact_values_and_rejections(void) {
    Probe probe = {0};
    XrStringBuilderAllocator allocator = {&probe, allocate, release};
    XrStringBuilderStorage storage = {0};
    XrTextDisplayOperand values[] = {
        {.kind = XR_TEXT_DISPLAY_I64, .i64 = INT64_MIN},
        {.kind = XR_TEXT_DISPLAY_BOOL, .boolean = 1},
        {.kind = XR_TEXT_DISPLAY_F64, .f64 = 1.5},
        {.kind = XR_TEXT_DISPLAY_RUNE, .rune = 0x1f600u},
    };
    for (size_t i = 0u; i < sizeof(values) / sizeof(values[0]); ++i)
        REQUIRE(xr_string_builder_append_value(&storage, &values[i], 1024u, &allocator) ==
                XR_STRING_BUILDER_OK);
    REQUIRE(xr_string_builder_append_value(&storage, NULL, 1024u, &allocator) == XR_STRING_BUILDER_OK);
    const uint8_t expected[] = "-9223372036854775808true1.5\xf0\x9f\x98\x80null";
    REQUIRE(storage.size == sizeof(expected) - 1u && storage.scalar_count == 32u);
    REQUIRE(memcmp(storage.bytes, expected, storage.size) == 0);
    size_t calls = probe.calls;
    const uint8_t invalid[] = {0xc0, 0x80};
    REQUIRE(xr_string_builder_append(&storage, invalid, sizeof(invalid), 1024u, &allocator) ==
            XR_STRING_BUILDER_INVALID);
    REQUIRE(xr_string_builder_append(&storage, storage.bytes + storage.size, 1u, 1024u,
                                      &allocator) == XR_STRING_BUILDER_INVALID);
    REQUIRE(xr_string_builder_append(&storage, expected, SIZE_MAX, SIZE_MAX, &allocator) ==
            XR_STRING_BUILDER_RESOURCE_LIMIT);
    XrTextDisplayOperand rejected = {.kind = XR_TEXT_DISPLAY_U64, .u64 = 1u};
    REQUIRE(xr_string_builder_append_value(&storage, &rejected, 1024u, &allocator) ==
            XR_STRING_BUILDER_INVALID);
    rejected = (XrTextDisplayOperand) {.kind = XR_TEXT_DISPLAY_RUNE, .rune = 0xd800u};
    REQUIRE(xr_string_builder_append_value(&storage, &rejected, 1024u, &allocator) ==
            XR_STRING_BUILDER_INVALID);
    REQUIRE(probe.calls == calls && storage.size == sizeof(expected) - 1u);
    REQUIRE(memcmp(storage.bytes, expected, storage.size) == 0);
    uint8_t *snapshot = NULL;
    REQUIRE(xr_string_builder_snapshot(&storage, storage.size - 1u, &allocator, &snapshot) ==
            XR_STRING_BUILDER_RESOURCE_LIMIT && snapshot == NULL);
    xr_string_builder_dispose(&storage, &allocator);
    REQUIRE(probe.live == 0u);
}

int main(void) {
    test_failure_and_aliasing();
    test_exact_values_and_rejections();
    puts("StringBuilder owned storage: PASS");
    return 0;
}
