/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_values.c - VM and mixed string calls with results outliving artifacts
 *
 * KEY CONCEPT:
 *   Assert independent expected values and explicit ownership at every exit.
 */


#include "xir/xxir_call.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#else
#include <pthread.h>
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void bytes_equal(const XrXirValue *value, const char *expected, size_t count) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length));
    CHECK(length == count && !memcmp(bytes, expected, count));
}
static void unicode_cases(void) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirDomainStats initial = xr_xir_domain_stats(domain);
    static const char valid[] = "A\0\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80";
    XrXirValue value = {0}, copy = {0}, empty = {0};
    CHECK(xr_xir_string_new(domain, valid, sizeof(valid) - 1, &value) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&value, &copy) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, NULL, 0, &empty) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_append(&copy, &empty) == XR_XIR_VALUE_OK);
    size_t runes = 0;
    CHECK(xr_xir_string_runes(&copy, &runes) && runes == 5);
    bytes_equal(&copy, valid, sizeof(valid) - 1);
    CHECK(xr_xir_value_copy(&value, &copy) == XR_XIR_VALUE_BAD_ARGUMENT);
    const char *invalid[] = {"\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\x80", "\xE4\xB8", "\xFF"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        XrXirValue rejected = {0};
        CHECK(xr_xir_string_new(domain, invalid[i], strlen(invalid[i]), &rejected) == XR_XIR_VALUE_BAD_UTF8);
        CHECK(!rejected.type);
    }
    xr_xir_value_drop(&value); xr_xir_value_drop(&copy); xr_xir_value_drop(&empty);
    CHECK(xr_xir_domain_stats(domain).live_bytes == initial.live_bytes);
    xr_xir_domain_drop(domain);
}
static void cow_cases(void) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirValue left = {0}, suffix = {0}, copy = {0};
    CHECK(xr_xir_string_new(domain, "abc", 3, &left) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "!", 1, &suffix) == XR_XIR_VALUE_OK);
    const char *before = NULL, *after = NULL; size_t size = 0;
    CHECK(xr_xir_string_view(&left, &before, &size));
    XrXirDomainStats stats = xr_xir_domain_stats(domain);
    CHECK(xr_xir_string_append(&left, &suffix) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_view(&left, &after, &size) && before == after);
    CHECK(xr_xir_domain_stats(domain).allocations == stats.allocations);
    CHECK(xr_xir_value_copy(&left, &copy) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_append(&left, &left) == XR_XIR_VALUE_OK);
    bytes_equal(&left, "abc!abc!", 8); bytes_equal(&copy, "abc!", 4);
    CHECK(xr_xir_string_append(&left, &left) == XR_XIR_VALUE_OK);
    bytes_equal(&left, "abc!abc!abc!abc!", 16);
    CHECK(xr_xir_domain_stats(domain).reallocations == 1);
    xr_xir_value_drop(&copy); xr_xir_value_drop(&suffix);
    xr_xir_domain_drop(domain);
    bytes_equal(&left, "abc!abc!abc!abc!", 16);
    xr_xir_value_drop(&left);
}
static void copy_worker(XrXirValue *source) {
    for (uint32_t i = 0; i < 2000; ++i) {
        XrXirValue copy = {0};
        CHECK(xr_xir_value_copy(source, &copy) == XR_XIR_VALUE_OK);
        if (source->type == XR_XIR_ATOMIC_I64) {
            int64_t previous = 0;
            CHECK(xr_xir_atomic_i64_fetch_add(&copy, 1, &previous));
            CHECK(previous >= 0 && previous < 8000);
        } else {
            CHECK(xr_xir_string_append(&copy, source) == XR_XIR_VALUE_OK);
            bytes_equal(&copy, "threadthread", 12);
        }
        xr_xir_value_drop(&copy);
    }
    xr_xir_value_drop(source);
}
#if defined(XR_OS_WINDOWS)
static DWORD WINAPI thread_entry(void *pointer) { copy_worker(pointer); return 0; }
#else
static void *thread_entry(void *pointer) { copy_worker(pointer); return NULL; }
#endif
static void concurrent_copies(bool atomic) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirValue value = {0}, copies[4] = {{0}, {0}, {0}, {0}};
    CHECK((atomic ? xr_xir_atomic_i64_new(domain, 0, &value) :
        xr_xir_string_new(domain, "thread", 6, &value)) == XR_XIR_VALUE_OK);
#if defined(XR_OS_WINDOWS)
    HANDLE threads[4];
#else
    pthread_t threads[4];
#endif
    for (uint32_t i = 0; i < 4; ++i) {
        CHECK(xr_xir_value_copy(&value, &copies[i]) == XR_XIR_VALUE_OK);
#if defined(XR_OS_WINDOWS)
        threads[i] = CreateThread(NULL, 0, thread_entry, &copies[i], 0, NULL);
        CHECK(threads[i] != NULL);
#else
        CHECK(pthread_create(&threads[i], NULL, thread_entry, &copies[i]) == 0);
#endif
    }
    for (uint32_t i = 0; i < 4; ++i) {
#if defined(XR_OS_WINDOWS)
        CHECK(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0);
        CHECK(CloseHandle(threads[i]));
#else
        CHECK(pthread_join(threads[i], NULL) == 0);
#endif
    }
    if (atomic) {
        int64_t count = 0;
        CHECK(xr_xir_atomic_i64_load(&value, &count) && count == 8000);
    } else bytes_equal(&value, "thread", 6);
    xr_xir_value_drop(&value);
    XrXirDomainStats stats = xr_xir_domain_stats(domain);
    CHECK(stats.allocations == stats.frees + 1);
    xr_xir_domain_drop(domain);
}
typedef struct TypedOutput { uint32_t seen; bool reject; } TypedOutput;
static bool typed_write(void *context, XrXirOutputStream stream, const XrXirValue *value) {
    TypedOutput *output = context;
    CHECK(stream == (output->seen ? XR_XIR_STDERR : XR_XIR_STDOUT));
    CHECK(value->type == (uint32_t) (output->seen ? XR_XIR_I64 : XR_XIR_BOOL));
    CHECK(value->payload == (output->seen ? INT64_MIN : 1));
    ++output->seen;
    return !output->reject;
}
static XrXirAction typed_resume(XrXirCallView *view) {
    uint32_t *pc = view->state;
    if ((*pc)++ == 0)
        return (XrXirAction) {XR_XIR_ACTION_OUTPUT, XR_XIR_STDOUT, NULL, 0, {XR_XIR_BOOL, 0, 1}};
    if (*pc == 2)
        return (XrXirAction) {XR_XIR_ACTION_OUTPUT, XR_XIR_STDERR, NULL, 0, {XR_XIR_I64, 0, INT64_MIN}};
    return (XrXirAction) {XR_XIR_ACTION_RETURN, 0, NULL, 0, {0, 0, 0}};
}
static void typed_output(void) {
    XrXirCallEntry entry = {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_UNIT,
        sizeof(uint32_t), typed_resume, NULL, NULL};
    for (uint32_t mode = 0; mode < 3; ++mode) {
        TypedOutput output = {0, mode == 1};
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config = {&entry, 1, NULL, 65536, 10, 10, &accounting,
            {mode == 2 ? NULL : typed_write, &output}};
        XrXirCall *call = NULL;
        CHECK(xr_xir_call_new(&config, 0, NULL, 0, &call) == XR_XIR_CALL_READY);
        CHECK(xr_xir_call_poll(call).status == (mode ? XR_XIR_CALL_OUTPUT_ERROR : XR_XIR_CALL_RETURNED));
        CHECK(output.seen == (mode == 2 ? 0u : mode == 1 ? 1u : 2u));
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(!accounting.live_bytes && accounting.allocations == accounting.frees);
    }
}
static XrXirAction atomic_output_resume(XrXirCallView *view) {
    return (XrXirAction) {XR_XIR_ACTION_OUTPUT, XR_XIR_STDOUT, NULL, 0, view->arguments[0]};
}
static void atomic_boundaries(void) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirValue cell = {0}, copy = {0};
    CHECK(xr_xir_atomic_i64_new(domain, INT64_MAX, &cell) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&cell, &copy) == XR_XIR_VALUE_OK);
    CHECK(cell.payload == copy.payload);
    int64_t old = 0, value = 0;
    CHECK(xr_xir_atomic_i64_fetch_add(&copy, 1, &old) && old == INT64_MAX);
    CHECK(xr_xir_atomic_i64_load(&cell, &value) && value == INT64_MIN);
    CHECK(!xr_xir_value_argument(&cell, XR_XIR_STRING));
    int64_t frame = 0;
    CHECK(xr_xir_owned_slot_copy(&frame, 0, XR_XIR_I64, 42) == XR_XIR_VALUE_BAD_ARGUMENT && !frame);
    const XrXirType type = XR_XIR_ATOMIC_I64;
    XrXirCallEntry entry = {XR_XIR_CALL_ABI_VERSION, &type, 1, XR_XIR_UNIT, 0, atomic_output_resume, NULL, NULL};
    XrXirCallAccounting accounting = {0};
    TypedOutput output = {0};
    XrXirCallConfig config = {&entry, 1, NULL, 65536, 10, 10, &accounting, {typed_write, &output}};
    XrXirCall *call = NULL;
    CHECK(xr_xir_call_new(&config, 0, &cell, 1, &call) == XR_XIR_CALL_READY);
    CHECK(xr_xir_call_poll(call).status == XR_XIR_CALL_BAD_STATE && !output.seen);
    CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
    xr_xir_domain_drop(domain); xr_xir_value_drop(&cell);
    CHECK(xr_xir_atomic_i64_load(&copy, &value) && value == INT64_MIN);
    xr_xir_value_drop(&copy);
}
int main(void) {
    unicode_cases(); cow_cases(); concurrent_copies(false); concurrent_copies(true); typed_output(); atomic_boundaries();
    puts("Strict Unicode, CoW growth, independent lifetime and concurrent owned copies passed");
    return 0;
}
