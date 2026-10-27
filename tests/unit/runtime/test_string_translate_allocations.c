/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_string_translate_allocations.c - Actual translate cleanup at failed boundaries
 */
#include "base/xmalloc.h"
#include "base/xutf8.h"
#include "runtime/xstrbuf.h"
#include "runtime/object/xstring.h"
#include "runtime/object/xmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "requirement failed at line %d: %s\n", __LINE__, #condition); \
    abort(); } } while (0)

static size_t buffer_calls, fail_buffer_at, live_buffers;
static size_t key_calls, fail_key_at, live_keys;
static bool reject_growth, reject_intern;
static XrString *replacement;
static void *probe_malloc(size_t bytes) {
    if (++buffer_calls == fail_buffer_at) return NULL;
    void *pointer = malloc(bytes);
    if (pointer) ++live_buffers;
    return pointer;
}
static void probe_free(void *pointer) {
    if (pointer) { REQUIRE(live_buffers != 0u); --live_buffers; }
    free(pointer);
}
static void *probe_realloc(void *pointer, size_t bytes) {
    return reject_growth ? NULL : realloc(pointer, bytes);
}
#undef xr_malloc
#undef xr_free
#undef xr_realloc
#define xr_malloc(bytes) probe_malloc(bytes)
#define xr_free(pointer) probe_free(pointer)
#define xr_realloc(pointer, bytes) probe_realloc(pointer, bytes)
#include "../../../src/runtime/xstrbuf.c"

/* The function and private buffer are real. String allocation, map lookup,
 * key release and interning are explicit boundary doubles, not ledger proof. */
static XrString *make_string(const char *bytes, size_t count) {
    XrString *value = calloc(1u, sizeof(*value) + count + 1u);
    REQUIRE(value != NULL);
    value->length = (uint32_t) count;
    if (count) memcpy(value->data, bytes, count);
    return value;
}
static XrString *probe_key(XrVMRuntime *owner, const char *bytes, size_t count) {
    (void) owner;
    if (++key_calls == fail_key_at) return NULL;
    XrString *key = make_string(bytes, count);
    ++live_keys;
    return key;
}
static XrValue probe_value(XrString *value) {
    return xr_make_typed_ptr_val(value, XR_TSTRING);
}
static XrValue probe_lookup(XrMap *table, XrValue key, bool *found) {
    REQUIRE(table != NULL);
    XrString *text = XR_TO_STRING(key);
    *found = text->length == 1u && text->data[0] == 'a';
    return *found ? probe_value(replacement) : xr_null();
}
static void probe_release(XrValue key) {
    REQUIRE(live_keys != 0u);
    --live_keys;
    free(XR_TO_STRING(key));
}
static XrString *probe_intern(XrStrBuf *buffer) {
    return reject_intern ? NULL : make_string(buffer->data, buffer->length);
}
#define xr_string_new(owner, bytes, count) probe_key(owner, bytes, count)
#define xr_string_value(value) probe_value(value)
#define xr_map_get(table, key, found) probe_lookup(table, key, found)
#define xr_rc_release_value(heap, key) probe_release(key)
#define xr_strbuf_to_string(buffer) probe_intern(buffer)
#define xr_string_translate tested_string_translate
#include "../../../src/runtime/object/xstring_translate.inc.c"

int main(void) {
    char expanded[70]; memset(expanded, 'x', sizeof(expanded));
    replacement = make_string(expanded, sizeof(expanded));
    XrString *input = make_string("ab", 2u);
    XrVMRuntime *owner = (XrVMRuntime *) (uintptr_t) 1u;
    XrMap *table = (XrMap *) (uintptr_t) 1u;
    for (unsigned mode = 0; mode < 7u; ++mode) {
        buffer_calls = key_calls = 0u;
        fail_buffer_at = mode == 1u ? 1u : mode == 2u ? 2u : 0u;
        fail_key_at = mode == 3u ? 1u : mode == 4u ? 2u : 0u;
        reject_growth = mode == 5u;
        reject_intern = mode == 6u;
        XrString *result = tested_string_translate(owner, input, table);
        if (mode == 0u) {
            REQUIRE(result != NULL && result->length == sizeof(expanded) + 1u);
            REQUIRE(memcmp(result->data, expanded, sizeof(expanded)) == 0);
            REQUIRE(result->data[sizeof(expanded)] == 'b');
            REQUIRE(result->data != input->data && result->data != replacement->data);
            free(result);
        } else {
            REQUIRE(result == NULL);
        }
        REQUIRE(live_buffers == 0u && live_keys == 0u);
        REQUIRE(input->length == 2u && memcmp(input->data, "ab", 2u) == 0);
        REQUIRE(replacement->length == sizeof(expanded));
    }
    free(replacement);
    free(input);
    return 0;
}
