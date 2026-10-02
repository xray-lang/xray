/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_compile_resources_concurrency.c - Real contention and physical release
 */
#include "base/xcompile_resources.h"
#include "base/xchecks.h"
#include "base/xmalloc.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#ifdef XR_OS_WINDOWS
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <time.h>
#endif

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define OK(c) CHECK((c) == XR_COMPILE_RESOURCE_OK)
#define WORKERS 8
#define ITERATIONS 200

static void yield_thread(void) {
#ifdef XR_OS_WINDOWS
    SwitchToThread();
#else
    sched_yield();
#endif
}
static void wait_flag(atomic_bool *flag) {
    while (!atomic_load_explicit(flag, memory_order_acquire)) yield_thread();
}
static void lock_flag(atomic_bool *flag) {
    while (atomic_exchange_explicit(flag, true, memory_order_acquire)) yield_thread();
}
static void unlock_flag(atomic_bool *flag) {
    atomic_store_explicit(flag, false, memory_order_release);
}

typedef struct Record { void *pointer; size_t bytes; } Record;
typedef struct Observation {
    size_t attempts, total, live, peak, count, owner_frees;
} Observation;
static Record records[128];
static Observation observed;
static atomic_bool observer_lock;
static size_t fail_at = SIZE_MAX;
static void *owner_pointer, *pause_free;
static atomic_bool free_entered, free_allowed;

static void *observed_allocate(size_t bytes) {
    lock_flag(&observer_lock);
    size_t attempt = observed.attempts++;
    if (attempt == fail_at) { unlock_flag(&observer_lock); return NULL; }
    void *pointer = xr_malloc(bytes);
    CHECK(pointer);
    size_t index = 0;
    while (index < 128 && records[index].pointer) ++index;
    CHECK(index < 128);
    records[index] = (Record) {pointer, bytes};
    observed.total += bytes; observed.live += bytes; ++observed.count;
    if (observed.live > observed.peak) observed.peak = observed.live;
    if (!attempt) owner_pointer = pointer;
    unlock_flag(&observer_lock);
    return pointer;
}
static void observed_free(void *pointer) {
    if (pointer == pause_free) {
        atomic_store_explicit(&free_entered, true, memory_order_release);
        wait_flag(&free_allowed);
    }
    lock_flag(&observer_lock);
    size_t index = 0;
    while (index < 128 && records[index].pointer != pointer) ++index;
    CHECK(index < 128);
    size_t bytes = records[index].bytes;
    if (pointer == owner_pointer) ++observed.owner_frees;
    xr_free(pointer);
    records[index] = (Record) {0};
    observed.live -= bytes; --observed.count;
    unlock_flag(&observer_lock);
}

#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) observed_allocate(bytes)
#define xr_free(pointer) observed_free(pointer)
#include "base/xcompile_resources.c"

static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
static Observation observation(void) {
    lock_flag(&observer_lock);
    Observation result = observed;
    unlock_flag(&observer_lock);
    return result;
}
static void reset_observer(void) {
    CHECK(!observed.live && !observed.count);
    memset(&observed, 0, sizeof(observed));
    owner_pointer = pause_free = NULL; fail_at = SIZE_MAX;
    atomic_store(&free_entered, false); atomic_store(&free_allowed, false);
}
static XrCompileResources *new_owner(XrCompileResourceLimits limits) {
    XrCompileResources *owner = NULL;
    OK(xr_compile_resources_new(&limits, &owner));
    return owner;
}
static XrCompileResourceStats stats(XrCompileResources *owner) {
    XrCompileResourceStats result;
    OK(xr_compile_resources_stats(owner, &result));
    return result;
}
static void check_physical(XrCompileResources *owner) {
    XrCompileResourceStats s = stats(owner);
    Observation o = observation();
    CHECK(s.live_bytes == o.live && s.allocated_bytes == o.total && s.peak_bytes == o.peak);
}
static void check_zero(void) {
    Observation o = observation();
    CHECK(!o.live && !o.count && o.owner_frees == 1);
}

typedef enum Operation { WORK, ALLOC, CALLOC, STRESS, OBSERVE, TRANSFER, FREE } Operation;
typedef struct Worker {
    XrCompileResources *owner;
    Operation operation;
    atomic_bool go, started, done;
    XrCompileResourceStatus status;
    void *memory;
    uint64_t snapshots;
} Worker;

static void worker_run(Worker *worker) {
    wait_flag(&worker->go);
    atomic_store_explicit(&worker->started, true, memory_order_release);
    if (worker->operation == WORK) worker->status = xr_compile_resources_work(worker->owner, 7);
    else if (worker->operation == ALLOC) worker->status = xr_compile_resources_alloc(worker->owner, 32, &worker->memory);
    else if (worker->operation == CALLOC) worker->status = xr_compile_resources_calloc(worker->owner, 4, 8, &worker->memory);
    else if (worker->operation == FREE) xr_compile_resources_free(worker->memory);
    else if (worker->operation == OBSERVE) {
        XrCompileResourceStats previous = stats(worker->owner);
        for (unsigned i = 0; i < ITERATIONS * 10; ++i) {
            XrCompileResourceStats next = stats(worker->owner);
            CHECK(next.work >= previous.work && next.allocated_bytes >= previous.allocated_bytes);
            CHECK(next.allocation_count >= previous.allocation_count && next.peak_bytes >= previous.peak_bytes);
            CHECK(next.live_bytes <= next.peak_bytes && next.peak_bytes <= next.allocated_bytes);
            previous = next; ++worker->snapshots; yield_thread();
        }
    } else if (worker->operation == TRANSFER) {
        /* The block is the worker's only initial reference after publication. */
        OK(xr_compile_resources_retain(worker->owner));
        OK(xr_compile_resources_work(worker->owner, 7));
        xr_compile_resources_release(worker->owner);
        OK(xr_compile_resources_resize(worker->owner, &worker->memory, 64));
        CHECK(stats(worker->owner).live_bytes <= stats(worker->owner).peak_bytes);
        xr_compile_resources_free(worker->memory);
        worker->memory = NULL;
    } else {
        for (unsigned i = 0; i < ITERATIONS; ++i) {
            OK(xr_compile_resources_retain(worker->owner));
            void *memory = NULL;
            OK(xr_compile_resources_calloc(worker->owner, 4, 4, &memory));
            for (size_t j = 0; j < 16; ++j) CHECK(!((unsigned char *) memory)[j]);
            memset(memory, 0x5A, 16);
            OK(xr_compile_resources_resize(worker->owner, &memory, 48));
            OK(xr_compile_resources_resize(worker->owner, &memory, 8));
            for (size_t j = 0; j < 8; ++j) CHECK(((unsigned char *) memory)[j] == 0x5A);
            OK(xr_compile_resources_work(worker->owner, 3));
            xr_compile_resources_free(memory);
            xr_compile_resources_release(worker->owner);
        }
    }
    if (worker->operation != TRANSFER && worker->operation != FREE)
        xr_compile_resources_release(worker->owner);
    atomic_store_explicit(&worker->done, true, memory_order_release);
}
#ifdef XR_OS_WINDOWS
typedef HANDLE TestThread;
static DWORD WINAPI worker_entry(void *argument) { worker_run(argument); return 0; }
static TestThread thread_start(Worker *worker) {
    HANDLE thread = CreateThread(NULL, 0, worker_entry, worker, 0, NULL);
    CHECK(thread); return thread;
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
static void worker_init(Worker *worker, XrCompileResources *owner, Operation operation) {
    memset(worker, 0, sizeof(*worker));
    worker->owner = owner; worker->operation = operation;
    atomic_init(&worker->go, false); atomic_init(&worker->started, false); atomic_init(&worker->done, false);
    if (operation != TRANSFER && operation != FREE) OK(xr_compile_resources_retain(owner));
}
static void start_all(Worker *workers, TestThread *threads) {
    for (unsigned i = 0; i < WORKERS; ++i) threads[i] = thread_start(&workers[i]);
    for (unsigned i = 0; i < WORKERS; ++i) atomic_store_explicit(&workers[i].go, true, memory_order_release);
}
static void join_all(TestThread *threads) {
    for (unsigned i = 0; i < WORKERS; ++i) thread_join(threads[i]);
}

static void competing_limits(void) {
    reset_observer();
    XrCompileResources *owner = new_owner(unlimited);
    size_t owner_bytes = observation().live;
    void *sample = NULL; OK(xr_compile_resources_alloc(owner, 32, &sample));
    size_t block_bytes = observation().live - owner_bytes;
    xr_compile_resources_free(sample); xr_compile_resources_release(owner); check_zero();
    for (unsigned field = 0; field < 4; ++field) for (unsigned minus = 0; minus < 2; ++minus) {
        reset_observer();
        XrCompileResourceLimits limits = unlimited;
        if (field == 0) limits.allocated_bytes = owner_bytes + 3 * block_bytes - minus;
        if (field == 1) limits.live_bytes = owner_bytes + 3 * block_bytes - minus;
        if (field == 2) limits.work = 1 + 3 * 7 - minus;
        if (field == 3) limits.work = 1 + 3 * 33 - minus;
        owner = new_owner(limits);
        Worker workers[WORKERS]; TestThread threads[WORKERS];
        Operation operation = field == 2 ? WORK : field == 3 ? CALLOC : ALLOC;
        for (unsigned i = 0; i < WORKERS; ++i) worker_init(&workers[i], owner, operation);
        start_all(workers, threads); join_all(threads);
        unsigned successes = 0;
        for (unsigned i = 0; i < WORKERS; ++i) {
            if (workers[i].status == XR_COMPILE_RESOURCE_OK) ++successes;
            else CHECK(workers[i].status == XR_COMPILE_RESOURCE_BUDGET && !workers[i].memory);
        }
        CHECK(successes == 3 - minus); check_physical(owner);
        CHECK(stats(owner).work == 1 + successes * (field == 2 ? 7u : field == 3 ? 33u : 1u));
        for (unsigned i = 0; i < WORKERS; ++i) xr_compile_resources_free(workers[i].memory);
        xr_compile_resources_release(owner); check_zero();
    }
}

static void concurrent_oom(void) {
    for (size_t fault = 1; fault <= WORKERS; ++fault) {
        reset_observer(); XrCompileResources *owner = new_owner(unlimited); fail_at = fault;
        Worker workers[WORKERS]; TestThread threads[WORKERS];
        for (unsigned i = 0; i < WORKERS; ++i) worker_init(&workers[i], owner, CALLOC);
        start_all(workers, threads); join_all(threads);
        unsigned failures = 0;
        for (unsigned i = 0; i < WORKERS; ++i) {
            if (workers[i].status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY) { ++failures; CHECK(!workers[i].memory); }
            else CHECK(workers[i].status == XR_COMPILE_RESOURCE_OK);
        }
        CHECK(failures == 1 && observation().attempts == 1 + WORKERS);
        CHECK(stats(owner).allocation_count == WORKERS);
        CHECK(stats(owner).work == 1 + WORKERS + (WORKERS - 1) * 32); check_physical(owner);
        for (unsigned i = 0; i < WORKERS; ++i) xr_compile_resources_free(workers[i].memory);
        xr_compile_resources_release(owner); check_zero();
    }
}

static void mixed_operations(void) {
    reset_observer(); XrCompileResources *owner = new_owner(unlimited);
    Worker workers[WORKERS]; TestThread threads[WORKERS];
    for (unsigned i = 0; i < WORKERS; ++i) worker_init(&workers[i], owner, i ? STRESS : OBSERVE);
    start_all(workers, threads); join_all(threads);
    CHECK(workers[0].snapshots == ITERATIONS * 10);
    CHECK(stats(owner).work == 1 + (WORKERS - 1) * ITERATIONS * 46);
    CHECK(stats(owner).allocation_count == 1 + (WORKERS - 1) * ITERATIONS * 3);
    check_physical(owner); CHECK(observation().count == 1);
    xr_compile_resources_release(owner); check_zero();
}

static void delayed_last_release(void) {
    for (unsigned repeat = 0; repeat < 20; ++repeat) {
        reset_observer(); XrCompileResources *owner = new_owner(unlimited);
        Worker workers[WORKERS]; TestThread threads[WORKERS];
        for (unsigned i = 0; i < WORKERS; ++i) {
            worker_init(&workers[i], owner, TRANSFER);
            OK(xr_compile_resources_alloc(owner, 32, &workers[i].memory));
            threads[i] = thread_start(&workers[i]);
        }
        xr_compile_resources_release(owner);
        for (unsigned i = 0; i < WORKERS; ++i) atomic_store_explicit(&workers[i].go, true, memory_order_release);
        join_all(threads); check_zero();
    }
}

static void physical_free_barrier(void) {
    reset_observer(); XrCompileResources *owner = new_owner(unlimited);
    void *memory = NULL; OK(xr_compile_resources_alloc(owner, 32, &memory));
    size_t live_limit = observation().live;
    xr_compile_resources_free(memory); xr_compile_resources_release(owner); check_zero();
    reset_observer();
    XrCompileResourceLimits limits = unlimited; limits.live_bytes = live_limit;
    owner = new_owner(limits); memory = NULL;
    OK(xr_compile_resources_alloc(owner, 32, &memory));
    XrCompileResourceStats before = stats(owner);
    pause_free = (CompileAllocation *) memory - 1;
    Worker releasing, allocating;
    worker_init(&releasing, owner, FREE); releasing.memory = memory;
    worker_init(&allocating, owner, ALLOC);
    TestThread release_thread = thread_start(&releasing);
    atomic_store_explicit(&releasing.go, true, memory_order_release); wait_flag(&free_entered);
    TestThread allocate_thread = thread_start(&allocating);
    atomic_store_explicit(&allocating.go, true, memory_order_release); wait_flag(&allocating.started);
    for (unsigned i = 0; i < 10000; ++i) yield_thread();
    CHECK(!atomic_load_explicit(&allocating.done, memory_order_acquire));
    CHECK(observation().live == before.live_bytes && observation().attempts == 2);
    atomic_store_explicit(&free_allowed, true, memory_order_release);
    thread_join(release_thread); thread_join(allocate_thread);
    CHECK(allocating.status == XR_COMPILE_RESOURCE_OK && allocating.memory);
    CHECK(stats(owner).peak_bytes == before.peak_bytes); check_physical(owner);
    pause_free = NULL; xr_compile_resources_free(allocating.memory);
    xr_compile_resources_release(owner); check_zero();
}

int main(void) {
    competing_limits(); concurrent_oom(); mixed_operations(); delayed_last_release(); physical_free_barrier();
    puts("compile resources concurrency: exact/minus1, OOM, snapshots, delayed last release and physical free barrier passed");
    return 0;
}
