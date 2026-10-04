/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_compile_resources_scan.c - Bounded scans and shared ledger limits
 *
 * KEY CONCEPT:
 *   Independent read/advance arithmetic checks every failure prefix and
 *   actual allocator observation checks physical release under contention.
 */
#include "base/xcompile_resources.h"
#include "base/xmalloc.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#ifdef XR_OS_WINDOWS
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define OK(c) CHECK((c) == XR_COMPILE_RESOURCE_OK)
typedef XrCompileResourceStatus (*ScanPrototype)(XrCompileResources *, const char *, size_t,
    unsigned char, size_t *, bool *);
_Static_assert(_Generic(&xr_compile_resources_scan_delimiter, ScanPrototype: 1, default: 0),
    "Bounded scan preserves typed status and six parameters");
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[16];
static size_t physical, live, calls, fail_at = SIZE_MAX;
/* Callbacks run under the production ledger lock. Main reads the observer
 * before creating threads or after every worker has joined. */
static void *counted_malloc(size_t bytes) {
    if (calls++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes);
    if (!memory) return NULL;
    for (size_t i = 0; i < 16; ++i) if (!allocations[i].pointer) {
        CHECK(bytes <= SIZE_MAX - physical);
        allocations[i] = (Allocation){memory, bytes};
        physical += bytes; ++live; return memory;
    }
    CHECK(false); return NULL;
}
static void counted_free(void *memory) {
    if (!memory) return;
    for (size_t i = 0; i < 16; ++i) if (allocations[i].pointer == memory) {
        CHECK(live && physical >= allocations[i].bytes);
        physical -= allocations[i].bytes; --live; allocations[i] = (Allocation){0};
        xr_free(memory); return;
    }
    CHECK(false);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) counted_malloc(bytes)
#define xr_free(memory) counted_free(memory)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

static XrCompileResources *owner(uint64_t work) {
    CHECK(!live && !physical);
    XrCompileResources *resources = NULL;
    const XrCompileResourceLimits limits = {4096, 4096, work};
    OK(xr_compile_resources_new(&limits, &resources));
    CHECK(live == 1 && physical <= 4096); return resources;
}
static XrCompileResourceStats stats(XrCompileResources *resources) {
    XrCompileResourceStats result = {0};
    OK(xr_compile_resources_stats(resources, &result)); return result;
}
static void release(XrCompileResources *resources) {
    xr_compile_resources_release(resources); CHECK(!live && !physical);
}
static void one_span(const char *source, size_t length, unsigned char delimiter,
    size_t expected_advance, bool expected_found) {
    /* Fixed delimiter positions supply the oracle: read1+advance1 for each
     * preceding byte, then only read1 for the delimiter itself. */
    uint64_t required = 2 * (uint64_t)expected_advance + (expected_found ? 1u : 0u);
    for (uint64_t budget = 0; budget <= required + 1; ++budget) {
        XrCompileResources *resources = owner(1 + budget);
        size_t advanced = SIZE_MAX, before = calls; bool found = true;
        XrCompileResourceStatus status = xr_compile_resources_scan_delimiter(
            resources, source, length, delimiter, &advanced, &found);
        CHECK(status == (budget < required ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OK));
        if (budget < required) CHECK(advanced == SIZE_MAX && found);
        else CHECK(advanced == expected_advance && found == expected_found);
        XrCompileResourceStats result = stats(resources);
        CHECK(result.work == 1 + (budget < required ? budget : required));
        CHECK(result.allocation_count == 1 && calls == before);
        CHECK(result.allocated_bytes == physical && result.live_bytes == physical && result.peak_bytes == physical);
        release(resources);
    }
}
static void all_prefixes(void) {
    /* Exact spans have no terminator. Embedded NUL is just another byte. */
    char source[64]; memset(source, 'x', sizeof(source));
    one_span(source, sizeof(source), '\n', 64, false);
    one_span(NULL, 0, '\n', 0, false);
    static const size_t positions[] = {0, 1, 31, 32, 62, 63};
    for (size_t i = 0; i < sizeof(positions)/sizeof(positions[0]); ++i) {
        memset(source, 'x', sizeof(source)); source[positions[i]] = '\n';
        one_span(source, sizeof(source), '\n', positions[i], true);
    }
    memset(source, 'x', sizeof(source)); source[17] = 0;
    one_span(source, sizeof(source), 0, 17, true);
    memset(source + 17, 0xFF, 1);
    one_span(source, sizeof(source), 0xFF, 17, true);
    one_span(source, 1, 'x', 0, true);
}
static void invalid_and_allocation_failures(void) {
    XrCompileResources *resources = owner(1000);
    const char source[64] = {0}; size_t advanced = SIZE_MAX; bool found = true;
    XrCompileResourceStats before = stats(resources);
    CHECK(xr_compile_resources_scan_delimiter(NULL, source, 1, 0, &advanced, &found) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_scan_delimiter(resources, NULL, 1, 0, &advanced, &found) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_scan_delimiter(resources, source, 65, 0, &advanced, &found) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_scan_delimiter(resources, source, SIZE_MAX, 0, &advanced, &found) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_scan_delimiter(resources, source, 1, 0, NULL, &found) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_scan_delimiter(resources, source, 1, 0, &advanced, NULL) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(advanced == SIZE_MAX && found && stats(resources).work == before.work);
    /* No physical scan allocation is attempted, even when the next actual
     * allocator call would fail. The root allocation remains an OOM site. */
    fail_at = calls;
    OK(xr_compile_resources_scan_delimiter(resources, source, sizeof(source), 0, &advanced, &found));
    CHECK(advanced == 0 && found && calls == fail_at);
    fail_at = SIZE_MAX; release(resources);
    XrCompileResources *empty = NULL;
    const XrCompileResourceLimits limits = {4096, 4096, 1000};
    fail_at = calls;
    CHECK(xr_compile_resources_new(&limits, &empty) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !empty);
    CHECK(!physical && !live); fail_at = SIZE_MAX;
}
static void chunks(size_t total, size_t delimiter_position) {
    char source[129]; CHECK(total <= sizeof(source)); memset(source, 'x', sizeof(source));
    bool expected_found = delimiter_position < total;
    if (expected_found) source[delimiter_position] = '\n';
    uint64_t required = expected_found ? 2 * (uint64_t)delimiter_position + 1 : 2 * (uint64_t)total;
    for (uint64_t budget = 0; budget <= required + 1; ++budget) {
        XrCompileResources *resources = owner(1 + budget); size_t position = 0;
        bool complete = false; XrCompileResourceStatus status = XR_COMPILE_RESOURCE_OK;
        while (position < total) {
            size_t length = total - position; if (length > 64) length = 64;
            size_t advanced = SIZE_MAX; bool found = true;
            status = xr_compile_resources_scan_delimiter(resources, source + position, length, '\n', &advanced, &found);
            if (status != XR_COMPILE_RESOURCE_OK) { CHECK(advanced == SIZE_MAX && found); break; }
            CHECK(advanced <= length); position += advanced;
            if (found) { complete = true; break; }
            CHECK(advanced == length);
        }
        CHECK(status == (budget < required ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OK));
        if (status == XR_COMPILE_RESOURCE_OK)
            CHECK(complete == expected_found && position == (expected_found ? delimiter_position : total));
        CHECK(stats(resources).work == 1 + (budget < required ? budget : required));
        CHECK(stats(resources).allocation_count == 1); release(resources);
    }
}

static void yield_thread(void) {
#ifdef XR_OS_WINDOWS
    SwitchToThread();
#else
    sched_yield();
#endif
}
typedef struct Shared {
    XrCompileResources *resources;
    atomic_bool go, done[3];
    char source[64];
} Shared;
typedef struct Worker {
    Shared *shared;
    unsigned operation;
    XrCompileResourceStatus status;
    size_t advanced;
    bool found;
    XrCompileResourceStats final;
} Worker;
static void worker_run(Worker *worker) {
    Shared *shared = worker->shared;
    while (!atomic_load_explicit(&shared->go, memory_order_acquire)) yield_thread();
    if (worker->operation == 0) {
        worker->advanced = SIZE_MAX; worker->found = true;
        worker->status = xr_compile_resources_scan_delimiter(shared->resources,
            shared->source, sizeof(shared->source), '\n', &worker->advanced, &worker->found);
    } else if (worker->operation == 1) {
        worker->status = xr_compile_resources_work(shared->resources, 7);
    } else if (worker->operation == 2) {
        void *memory = NULL;
        worker->status = xr_compile_resources_alloc(shared->resources, 16, &memory);
        if (worker->status == XR_COMPILE_RESOURCE_OK) CHECK(memory);
        else CHECK(!memory);
        xr_compile_resources_free(memory);
    } else {
        XrCompileResourceStats previous = stats(shared->resources);
        for (;;) {
            worker->final = stats(shared->resources);
            CHECK(worker->final.work >= previous.work && worker->final.allocation_count >= previous.allocation_count);
            CHECK(worker->final.live_bytes <= worker->final.peak_bytes && worker->final.peak_bytes <= worker->final.allocated_bytes);
            CHECK(worker->final.work <= 137);
            previous = worker->final;
            bool all = true;
            for (size_t i = 0; i < 3; ++i) all = all && atomic_load_explicit(&shared->done[i], memory_order_acquire);
            if (all) { worker->final = stats(shared->resources); break; }
            yield_thread();
        }
    }
    xr_compile_resources_release(shared->resources);
    if (worker->operation < 3) atomic_store_explicit(&shared->done[worker->operation], true, memory_order_release);
}
#ifdef XR_OS_WINDOWS
typedef HANDLE TestThread;
static DWORD WINAPI worker_entry(void *argument) { worker_run(argument); return 0; }
static TestThread thread_start(Worker *worker) {
    HANDLE thread = CreateThread(NULL, 0, worker_entry, worker, 0, NULL); CHECK(thread); return thread;
}
static void thread_join(TestThread thread) {
    CHECK(WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0); CHECK(CloseHandle(thread));
}
#else
typedef pthread_t TestThread;
static void *worker_entry(void *argument) { worker_run(argument); return NULL; }
static TestThread thread_start(Worker *worker) {
    pthread_t thread; CHECK(!pthread_create(&thread, NULL, worker_entry, worker)); return thread;
}
static void thread_join(TestThread thread) { CHECK(!pthread_join(thread, NULL)); }
#endif
static void concurrency(bool producer_first, unsigned minus) {
    Shared shared = {0}; shared.resources = owner(137 - minus);
    size_t baseline = physical; memset(shared.source, 'x', sizeof(shared.source));
    atomic_init(&shared.go, false);
    for (size_t i = 0; i < 3; ++i) atomic_init(&shared.done[i], false);
    Worker workers[4] = {0}; TestThread threads[4];
    for (unsigned i = 0; i < 4; ++i) {
        workers[i].shared = &shared; workers[i].operation = i;
        OK(xr_compile_resources_retain(shared.resources)); threads[i] = thread_start(&workers[i]);
    }
    if (producer_first) xr_compile_resources_release(shared.resources);
    atomic_store_explicit(&shared.go, true, memory_order_release);
    for (unsigned i = 0; i < 4; ++i) thread_join(threads[i]);
    unsigned failures = 0;
    for (size_t i = 0; i < 3; ++i) {
        CHECK(workers[i].status == XR_COMPILE_RESOURCE_OK || workers[i].status == XR_COMPILE_RESOURCE_BUDGET);
        failures += workers[i].status == XR_COMPILE_RESOURCE_BUDGET;
    }
    CHECK(minus ? failures >= 1 : failures == 0);
    if (workers[0].status == XR_COMPILE_RESOURCE_OK) {
        CHECK(workers[0].advanced == 64 && !workers[0].found);
        uint64_t required = 1 + 128 + (workers[1].status == XR_COMPILE_RESOURCE_OK ? 7u : 0u) +
            (workers[2].status == XR_COMPILE_RESOURCE_OK ? 1u : 0u);
        CHECK(workers[3].final.work == required);
    } else {
        CHECK(workers[0].advanced == SIZE_MAX && workers[0].found);
        CHECK(workers[3].final.work == 137 - minus);
    }
    CHECK(workers[3].final.allocation_count == 1 + (workers[2].status == XR_COMPILE_RESOURCE_OK ? 1u : 0u));
    CHECK(workers[3].final.live_bytes == baseline);
    if (!producer_first) { CHECK(physical == baseline && live == 1); release(shared.resources); }
    CHECK(!live && !physical);
}
int main(void) {
    all_prefixes(); invalid_and_allocation_failures();
    chunks(63, SIZE_MAX); chunks(64, SIZE_MAX); chunks(65, SIZE_MAX); chunks(129, SIZE_MAX);
    chunks(65, 62); chunks(65, 63); chunks(65, 64); chunks(129, 128);
    for (unsigned minus = 0; minus < 2; ++minus) {
        concurrency(false, minus); concurrency(true, minus);
    }
    puts("bounded delimiter scan: every work prefix, preserved failure outputs, shared ledger and physical0 passed");
    return 0;
}
