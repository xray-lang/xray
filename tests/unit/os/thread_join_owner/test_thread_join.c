/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_thread_join.c - Real Windows thread ownership across failed joins
 *
 * KEY CONCEPT:
 *   Faults replace only the selected wait or close result. The thread,
 *   trampoline, context and successful retry use the production implementation.
 */
#include "os/os_thread.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)

#if defined(THREAD_JOIN_INJECTED)
static HANDLE watched_handle;
static DWORD injected_wait;
static unsigned wait_failures, close_failures, wait_calls, close_calls;
static size_t allocations, frees, live_bytes;
static void *owned_context;
static void **watched_output;
static void *expected_output;

static void *join_alloc(size_t size) {
    CHECK(!owned_context);
    void *memory = xr_malloc(size);
    CHECK(memory != NULL);
    owned_context = memory;
    live_bytes = size;
    ++allocations;
    return memory;
}

static void join_free(void *memory) {
    CHECK(memory && memory == owned_context && live_bytes);
    CHECK(close_calls > 0);
    owned_context = NULL;
    live_bytes = 0;
    ++frees;
    xr_free(memory);
}

static DWORD WINAPI join_wait(HANDLE handle, DWORD milliseconds) {
    CHECK(handle == watched_handle && milliseconds == INFINITE);
    ++wait_calls;
    if (wait_failures) {
        --wait_failures;
        SetLastError(ERROR_INVALID_HANDLE);
        return injected_wait;
    }
    return WaitForSingleObject(handle, milliseconds);
}

static BOOL WINAPI join_close(HANDLE handle) {
    CHECK(handle == watched_handle);
    CHECK(!watched_output || *watched_output == expected_output);
    ++close_calls;
    if (close_failures) {
        --close_failures;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return CloseHandle(handle);
}

#undef xr_malloc
#undef xr_free
#define xr_malloc join_alloc
#define xr_free join_free
#define WaitForSingleObject join_wait
#define CloseHandle join_close
#include "../../../../src/os/win/thread_win.c"
#undef CloseHandle
#undef WaitForSingleObject
#undef xr_free
#undef xr_malloc
#endif

typedef struct Worker {
    HANDLE started, proceed;
    LONG completed;
    int returned;
} Worker;

static void *worker_entry(void *context) {
    Worker *worker = context;
    CHECK(SetEvent(worker->started));
    CHECK(WaitForSingleObject(worker->proceed, 5000) == WAIT_OBJECT_0);
    InterlockedExchange(&worker->completed, 1);
    return &worker->returned;
}

static DWORD handles(void) {
    DWORD count = 0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &count));
    return count;
}

static void run_case(DWORD wait_failure, unsigned close_failure, bool no_output) {
    DWORD baseline = handles();
    Worker worker = {0};
    worker.started = CreateEventW(NULL, TRUE, FALSE, NULL);
    worker.proceed = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(worker.started && worker.proceed);
    xr_thread_t thread = {0};
    int sentinel = 0;
    void *output = &sentinel;
#if defined(THREAD_JOIN_INJECTED)
    size_t old_allocations = allocations, old_frees = frees;
    wait_calls = close_calls = 0;
    watched_output = no_output ? NULL : &output;
    expected_output = &sentinel;
#else
    (void)wait_failure;
    (void)close_failure;
#endif
    CHECK(xr_thread_create_ex(&thread, worker_entry, &worker, 0));
    CHECK(WaitForSingleObject(worker.started, 5000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(thread.handle, 0) == WAIT_TIMEOUT);
    CHECK(handles() == baseline + 3);
#if defined(THREAD_JOIN_INJECTED)
    watched_handle = thread.handle;
    CHECK(thread.ctx == owned_context && live_bytes == sizeof(*thread.ctx));
    CHECK(allocations == old_allocations + 1 && frees == old_frees);
    if (wait_failure) {
        injected_wait = wait_failure;
        wait_failures = 2;
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            CHECK(xr_thread_join(thread, &output) != 0);
            CHECK(output == &sentinel && !close_calls && frees == old_frees);
            CHECK(owned_context == thread.ctx && live_bytes == sizeof(*thread.ctx));
            CHECK(InterlockedCompareExchange(&worker.completed, 0, 0) == 0);
            CHECK(WaitForSingleObject(thread.handle, 0) == WAIT_TIMEOUT);
            CHECK(handles() == baseline + 3);
        }
        CHECK(wait_calls == 2);
    }
#endif
    CHECK(SetEvent(worker.proceed));
    CHECK(WaitForSingleObject(thread.handle, 5000) == WAIT_OBJECT_0);
    CHECK(InterlockedCompareExchange(&worker.completed, 0, 0) == 1);
#if defined(THREAD_JOIN_INJECTED)
    if (close_failure) {
        unsigned old_waits = wait_calls;
        close_failures = close_failure;
        for (unsigned attempt = 0; attempt < close_failure; ++attempt) {
            CHECK(xr_thread_join(thread, &output) != 0);
            CHECK(output == &sentinel && frees == old_frees);
            CHECK(owned_context == thread.ctx && live_bytes == sizeof(*thread.ctx));
            CHECK(WaitForSingleObject(thread.handle, 0) == WAIT_OBJECT_0);
            CHECK(handles() == baseline + 3);
        }
        CHECK(wait_calls == old_waits + close_failure && close_calls == close_failure);
    }
#endif
    CHECK(xr_thread_join(thread, no_output ? NULL : &output) == 0);
    CHECK(output == (no_output ? (void *)&sentinel : (void *)&worker.returned));
#if defined(THREAD_JOIN_INJECTED)
    CHECK(!owned_context && !live_bytes && frees == old_frees + 1);
    CHECK(allocations == old_allocations + 1 && close_calls == close_failure + 1);
    watched_output = NULL;
#endif
    CHECK(handles() == baseline + 2);
    CHECK(CloseHandle(worker.proceed));
    CHECK(CloseHandle(worker.started));
    CHECK(handles() == baseline);
}

int main(void) {
    DWORD baseline = handles();
    int sentinel = 0;
    void *output = &sentinel;
    CHECK(xr_thread_join((xr_thread_t){0}, &output) != 0 && output == &sentinel);
#if defined(THREAD_JOIN_INJECTED)
    CHECK(!allocations && !frees && !wait_calls && !close_calls);
    run_case(WAIT_FAILED, 0, false);
    run_case(WAIT_TIMEOUT, 0, false);
    run_case(WAIT_ABANDONED, 0, false);
    run_case(0, 2, false);
    run_case(WAIT_FAILED, 2, false);
#endif
    run_case(0, 0, false);
    run_case(0, 0, true);
#if defined(THREAD_JOIN_INJECTED)
    CHECK(allocations == 7 && frees == 7 && !owned_context && !live_bytes);
    printf("wait/close retry: allocations=%zu frees=%zu live=%zu\n", allocations, frees, live_bytes);
#endif
    CHECK(handles() == baseline);
    printf("real thread joins: handles=%lu->%lu PASS\n", (unsigned long)baseline,
        (unsigned long)handles());
    return 0;
}
