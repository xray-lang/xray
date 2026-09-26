/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_value_allocations.c - VM and mixed string calls with results outliving artifacts
 *
 * KEY CONCEPT:
 *   Assert independent expected values and explicit ownership at every exit.
 */


#include "base/xmalloc.h"
#include "xir/xxir_call.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static size_t calls, live, fail_at = SIZE_MAX;

static void *counted_malloc(size_t size) {
    if (calls++ == fail_at)
        return NULL;
    void *pointer = xr_malloc(size);
    if (pointer)
        ++live;
    return pointer;
}

static void *counted_calloc(size_t count, size_t size) {
    if (calls++ == fail_at)
        return NULL;
    void *pointer = xr_calloc(count, size);
    if (pointer)
        ++live;
    return pointer;
}

static void counted_free(void *pointer) {
    if (pointer) {
        CHECK(live > 0);
        --live;
    }
    xr_free(pointer);
}

static void *counted_realloc(void *pointer, size_t size) {
    if (calls++ == fail_at)
        return NULL;
    bool was_null = pointer == NULL;
    void *replacement = xr_realloc(pointer, size);
    if (replacement && was_null)
        ++live;
    return replacement;
}

#undef xr_malloc
#undef xr_calloc
#undef xr_free
#undef xr_realloc
#define xr_malloc(size) counted_malloc(size)
#define xr_calloc(count, size) counted_calloc(count, size)
#define xr_free(pointer) counted_free(pointer)
#define xr_realloc(pointer, size) counted_realloc(pointer, size)


#include "xir/xxir_value.c"
#include "xir/xxir_call.c"
#include "xir/xxir_output.c"
static bool counted_sink(void *context, XrXirOutputStream stream, const char *bytes, size_t length) {
    size_t *published = context;
    CHECK(stream == XR_XIR_STDOUT && length == 12 && !memcmp(bytes, "false 0 123\n", 12));
    ++*published;
    return true;
}
static void output_allocation(void) {
    XrXirValue values[] = {{XR_XIR_BOOL, 0, 0}, {XR_XIR_I64, 0, 0}, {XR_XIR_I64, 0, 123}};
    XrXirOutputGroup group = {XR_XIR_STDOUT, values, 3, true};
    size_t published = 0;
    XrXirOutputSink sink = {counted_sink, &published, 12};
    fail_at = calls;
    CHECK(!xr_xir_output_render(&sink, &group) && !published && !live);
    fail_at = SIZE_MAX;
    CHECK(xr_xir_output_render(&sink, &group) && published == 1 && !live);
    size_t before = calls;
    sink.byte_limit = 10;
    CHECK(!xr_xir_output_render(&sink, &group));
    sink.byte_limit = 12;
    values[0].payload = 2;
    CHECK(!xr_xir_output_render(&sink, &group));
    values[0].payload = 0; values[1].reserved = 1;
    CHECK(!xr_xir_output_render(&sink, &group));
    values[1].reserved = 0; values[1].type = XR_XIR_UNIT;
    CHECK(!xr_xir_output_render(&sink, &group));
    values[1].type = XR_XIR_I64;
    group.stream = XR_XIR_STDERR;
    CHECK(!xr_xir_output_render(&sink, &group));
    group.stream = XR_XIR_STDOUT; group.line = false;
    CHECK(!xr_xir_output_render(&sink, &group));
    group.line = true; group.count = 65537;
    CHECK(!xr_xir_output_render(&sink, &group));
    group.count = 3; group.values = NULL;
    CHECK(!xr_xir_output_render(&sink, &group));
    CHECK(calls == before && published == 1 && !live);
}
static XrXirAction identity_resume(XrXirCallView *view) {
    return (XrXirAction) {XR_XIR_ACTION_RETURN, 0, NULL, 0, view->arguments[0]};
}
static void fail_sequence(void) {
    XrXirDomain *domain = NULL;
    XrXirValue left = {0}, right = {0}, copy = {0};
    XrXirValueStatus status = xr_xir_domain_new(65536, &domain);
    if (status != XR_XIR_VALUE_OK) goto done;
    status = xr_xir_string_new(domain, "123456789012345", 15, &left);
    if (status != XR_XIR_VALUE_OK) goto done;
    status = xr_xir_string_new(domain, "123456789012345", 15, &right);
    if (status != XR_XIR_VALUE_OK) goto done;
    size_t baseline = live;
    uint64_t bytes = xr_xir_domain_stats(domain).live_bytes;
    status = xr_xir_string_append(&left, &right);
    if (status != XR_XIR_VALUE_OK) {
        CHECK(live == baseline && xr_xir_domain_stats(domain).live_bytes == bytes);
        CHECK(string_pointer(&left)->length == 15);
        goto done;
    }
    CHECK(xr_xir_value_copy(&left, &copy) == XR_XIR_VALUE_OK);
    baseline = live; bytes = xr_xir_domain_stats(domain).live_bytes;
    status = xr_xir_string_append(&left, &right);
    if (status != XR_XIR_VALUE_OK) {
        CHECK(live == baseline && xr_xir_domain_stats(domain).live_bytes == bytes);
        CHECK(string_pointer(&left)->length == 30 && left.payload == copy.payload);
        goto done;
    }
    XrXirType type = XR_XIR_STRING;
    XrXirCallEntry entry = {XR_XIR_CALL_ABI_VERSION, &type, 1, XR_XIR_STRING, 0, identity_resume, NULL, NULL};
    XrXirCallAccounting accounting = {0};
    XrXirCallConfig config = {&entry, 1, NULL, 65536, 10, 10, &accounting, {NULL, NULL}};
    XrXirCall *call = NULL;
    XrXirCallStatus admitted = xr_xir_call_new(&config, 0, &left, 1, &call);
    if (admitted == XR_XIR_CALL_READY) {
        xr_xir_value_drop(&left);
        CHECK(xr_xir_call_poll(call).status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_call_take_result(call, &left) == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
    } else CHECK(admitted == XR_XIR_CALL_OOM && !call);
    CHECK(!accounting.live_bytes && accounting.allocations == accounting.frees);
 done:
    CHECK(status == XR_XIR_VALUE_OK || status == XR_XIR_VALUE_OOM);
    xr_xir_domain_drop(domain);
    xr_xir_value_drop(&left); xr_xir_value_drop(&right); xr_xir_value_drop(&copy);
    CHECK(!live);
}
static void saturation(void) {
    XrXirDomain *domain = NULL; XrXirValue value = {0}, copy = {0};
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "x", 1, &value) == XR_XIR_VALUE_OK);
    XirString *string = string_pointer(&value);
    atomic_store(&string->object.references, UINT32_MAX);
    CHECK(xr_xir_value_copy(&value, &copy) == XR_XIR_VALUE_REFCOUNT_LIMIT && !copy.type);
    XrXirValue first = {0};
    CHECK(xr_xir_string_new(domain, "first", 5, &first) == XR_XIR_VALUE_OK);
    XrXirValue arguments[] = {first, value};
    XrXirType parameters[] = {XR_XIR_STRING, XR_XIR_STRING};
    XrXirCallEntry entry = {XR_XIR_CALL_ABI_VERSION, parameters, 2, XR_XIR_STRING,
        0, identity_resume, NULL, NULL};
    XrXirCallAccounting accounting = {0};
    XrXirCallConfig config = {&entry, 1, NULL, 65536, 10, 10, &accounting, {NULL, NULL}};
    XrXirCall *call = NULL;
    size_t baseline = live;
    CHECK(xr_xir_call_new(&config, 0, arguments, 2, &call) == XR_XIR_CALL_LIMIT && !call);
    CHECK(live == baseline && !accounting.live_bytes && accounting.allocations == accounting.frees);
    CHECK(atomic_load(&string_pointer(&first)->object.references) == 1);
    xr_xir_value_drop(&first);
    atomic_store(&string->object.references, 1);
    atomic_store(&domain->references, UINT32_MAX);
    CHECK(xr_xir_string_new(domain, "y", 1, &copy) == XR_XIR_VALUE_REFCOUNT_LIMIT && !copy.type);
    atomic_store(&domain->references, 2);
    xr_xir_value_drop(&value); xr_xir_domain_drop(domain);
    CHECK(!live);
}
static void capture_release(void *owner) { ++*(size_t *) owner; }
static void capture_ownership(void) {
    XrXirDomain *domain = NULL; XrXirValue values[2] = {{0}}, output = {0};
    CHECK(xr_xir_domain_new(65536,&domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"first",5,&values[0]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"second",6,&values[1]) == XR_XIR_VALUE_OK);
    size_t releases = 0, baseline = live;
    uint64_t bytes = xr_xir_domain_stats(domain).live_bytes;
    XrXirFunctionBinding binding = {&releases,capture_release,7,values,2};
    atomic_store(&object_pointer(values+1)->references,UINT32_MAX);
    CHECK(xr_xir_function_new(domain,(XrXirType)256,&binding,&output) == XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(!output.type && !releases && live == baseline && xr_xir_domain_stats(domain).live_bytes == bytes);
    CHECK(atomic_load(&object_pointer(values)->references) == 1);
    atomic_store(&object_pointer(values+1)->references,1);
    fail_at = calls;
    CHECK(xr_xir_function_new(domain,(XrXirType)256,&binding,&output) == XR_XIR_VALUE_OOM);
    CHECK(!output.type && !releases && live == baseline && xr_xir_domain_stats(domain).live_bytes == bytes);
    fail_at = SIZE_MAX;
    domain->limit = bytes;
    CHECK(xr_xir_function_new(domain,(XrXirType)256,&binding,&output) == XR_XIR_VALUE_LIMIT);
    domain->limit = 65536;
    CHECK(xr_xir_function_new(domain,(XrXirType)256,&binding,&output) == XR_XIR_VALUE_OK);
    XrXirValue copy = {0}; CHECK(xr_xir_value_copy(&output,&copy) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_append(values,values+1) == XR_XIR_VALUE_OK);
    const XrXirFunctionBinding *owned = xr_xir_function_binding(&output);
    const char *text; size_t length;
    CHECK(owned->captures != values && owned->capture_count == 2);
    CHECK(xr_xir_string_view(owned->captures,&text,&length) && length == 5 && !memcmp(text,"first",5));
    xr_xir_domain_drop(domain); xr_xir_value_drop(values); xr_xir_value_drop(values+1);
    xr_xir_value_drop(&output); CHECK(!releases);
    xr_xir_value_drop(&copy); CHECK(releases == 1 && !live);
}
static void deep_capture_release(void) {
    XrXirDomain *domain = NULL; XrXirValue previous = {XR_XIR_I64,0,17};
    CHECK(xr_xir_domain_new(32u*1024u*1024u,&domain) == XR_XIR_VALUE_OK);
    size_t releases = 0;
    for (uint32_t depth = 0; depth < 100000; ++depth) {
        XrXirValue next = {0};
        XrXirFunctionBinding binding = {&releases,capture_release,0,&previous,1};
        CHECK(xr_xir_function_new(domain,(XrXirType)256,&binding,&next) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&previous); previous = next;
    }
    xr_xir_domain_drop(domain);
    size_t allocations = calls;
    fail_at = calls;
    xr_xir_value_drop(&previous);
    CHECK(releases == 100000 && !live && calls == allocations);
    fail_at = SIZE_MAX;
    puts("Capture cleanup: 100000 nested environments; zero cleanup allocations; zero live blocks");
}
int main(void) {
    fail_at = SIZE_MAX; calls = 0; fail_sequence();
    size_t count = calls;
    for (size_t i = 0; i < count; ++i) { fail_at = i; calls = 0; fail_sequence(); }
    fail_at = SIZE_MAX; saturation(); output_allocation(); capture_ownership(); deep_capture_release();
    printf("Managed allocation failures: %zu; every domain, string and activation physically released\n", count);
    return 0;
}
