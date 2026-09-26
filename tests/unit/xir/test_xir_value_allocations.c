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
int main(void) {
    fail_at = SIZE_MAX; calls = 0; fail_sequence();
    size_t count = calls;
    for (size_t i = 0; i < count; ++i) { fail_at = i; calls = 0; fail_sequence(); }
    fail_at = SIZE_MAX; saturation();
    printf("Managed allocation failures: %zu; every domain, string and activation physically released\n", count);
    return 0;
}
