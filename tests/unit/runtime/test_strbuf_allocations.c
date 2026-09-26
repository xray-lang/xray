/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_strbuf_allocations.c - Real buffer implementation under rejected allocation
 */
#include "base/xmalloc.h"
#include "runtime/xstrbuf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t allocation_calls, fail_at, live_allocations;
static void *probe_malloc(size_t bytes) {
    if (++allocation_calls == fail_at)
        return NULL;
    void *pointer = malloc(bytes);
    if (pointer)
        ++live_allocations;
    return pointer;
}
static void probe_free(void *pointer) {
    if (pointer) {
        if (!live_allocations)
            abort();
        --live_allocations;
    }
    free(pointer);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) probe_malloc(bytes)
#define xr_free(pointer) probe_free(pointer)

static bool reject_reallocation;
static bool force_move;
static size_t old_capacity;
static void *probe_realloc(void *pointer, size_t bytes) {
    if (reject_reallocation)
        return NULL;
    if (!force_move)
        return realloc(pointer, bytes);
    void *moved = malloc(bytes);
    if (!moved)
        return NULL;
    if (pointer)
        memcpy(moved, pointer, old_capacity < bytes ? old_capacity : bytes);
    free(pointer);
    old_capacity = bytes;
    return moved;
}
#undef xr_realloc
#define xr_realloc(pointer, bytes) probe_realloc(pointer, bytes)
#include "../../../src/runtime/xstrbuf.c"

#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "requirement failed at line %d: %s\n", __LINE__, #condition); \
    abort(); } } while (0)

int main(void) {
    for (fail_at = 1u; fail_at <= 2u; ++fail_at) {
        allocation_calls = 0u;
        REQUIRE(xr_strbuf_new((XrVMRuntime *) (uintptr_t) 1u, 64u) == NULL);
        REQUIRE(live_allocations == 0u);
    }
    fail_at = 0u;
    XrStrBuf *buffer = xr_strbuf_new((XrVMRuntime *) (uintptr_t) 1u, 64u);
    REQUIRE(buffer != NULL);
    char initial[64];
    memset(initial, 'a', sizeof(initial));
    xr_strbuf_append_cstr(buffer, initial, sizeof(initial));
    REQUIRE(buffer->length == sizeof(initial));
    char *original = buffer->data;
    reject_reallocation = true;
    REQUIRE(!xr_strbuf_append_char(buffer, 'b'));
    REQUIRE(buffer->data == original && buffer->capacity == 64u && buffer->length == 64u);
    REQUIRE(memcmp(buffer->data, initial, sizeof(initial)) == 0);
    REQUIRE(!xr_strbuf_append_cstr(buffer, "more", 4u));
    REQUIRE(!xr_strbuf_append_int(buffer, INT64_MIN));
    REQUIRE(!xr_strbuf_append_float(buffer, 1.25));
    REQUIRE(!xr_strbuf_reserve(buffer, 128u));
    REQUIRE(!xr_strbuf_ensure(buffer, SIZE_MAX));
    REQUIRE(buffer->data == original && buffer->length == 64u && buffer->capacity == 64u);
    REQUIRE(memcmp(buffer->data, initial, sizeof(initial)) == 0);
    reject_reallocation = false;
    force_move = true;
    old_capacity = buffer->capacity;
    REQUIRE(xr_strbuf_append_cstr(buffer, buffer->data, buffer->length));
    REQUIRE(buffer->data != original && buffer->length == 128u);
    REQUIRE(memcmp(buffer->data, initial, 64u) == 0);
    REQUIRE(memcmp(buffer->data + 64u, initial, 64u) == 0);
    REQUIRE(!xr_strbuf_append_cstr(buffer, buffer->data + 127u, 2u));
    REQUIRE(xr_strbuf_append_char(buffer, 'b'));
    REQUIRE(buffer->length == 129u && buffer->data[128] == 'b');
    xr_strbuf_reset(buffer);
    REQUIRE(xr_strbuf_append_cstr(buffer, NULL, 0u));
    REQUIRE(xr_strbuf_append_cstr(buffer, "x\0y", 3u));
    REQUIRE(buffer->length == 3u && memcmp(buffer->data, "x\0y", 3u) == 0);
    xr_strbuf_free(buffer);
    REQUIRE(live_allocations == 0u);
    REQUIRE(xr_strbuf_new((XrVMRuntime *) (uintptr_t) 1u, SIZE_MAX) == NULL);

    size_t capacity = 0u;
    REQUIRE(!xr_buffer_capacity_plan(SIZE_MAX, SIZE_MAX, 1u, 64u, SIZE_MAX, &capacity));
    REQUIRE(capacity == 0u);
    REQUIRE(xr_buffer_capacity_plan(SIZE_MAX - 1u, SIZE_MAX - 1u, 1u, 64u, SIZE_MAX, &capacity));
    REQUIRE(capacity == SIZE_MAX);
    REQUIRE(!xr_buffer_capacity_plan(2u, 1u, 0u, 64u, SIZE_MAX, &capacity));
    for (size_t maximum = 0u; maximum < 20u; ++maximum)
        for (size_t current = 0u; current < 22u; ++current)
            for (size_t length = 0u; length < 22u; ++length)
                for (size_t additional = 0u; additional < 22u; ++additional) {
                    bool expected = length <= current && current <= maximum &&
                                    length + additional <= maximum;
                    bool ok = xr_buffer_capacity_plan(length, current, additional, 4u,
                                                            maximum, &capacity);
                    REQUIRE(ok == expected);
                    if (ok)
                        REQUIRE(capacity >= current && capacity >= length + additional &&
                                capacity <= maximum);
                    else
                        REQUIRE(capacity == 0u);
                }
    return 0;
}
