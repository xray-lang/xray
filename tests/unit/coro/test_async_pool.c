/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_async_pool.c - Unit tests for bounded async pool submission
 */

#include "../test_framework.h"
#include "coro/xasync.h"
#include "coro/xcoroutine.h"
#include "coro/xcoro_pool.h"
#include "os/os_time.h"
#include <stdatomic.h>
#include <string.h>

static void noop_invoke(void *data) {
    (void) data;
}

static void count_destroy(void *data) {
    atomic_int *counter = (atomic_int *) data;
    atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
}

TEST(async_submit_rejects_when_queue_full) {
    XrRuntime runtime;
    XrAsyncPool pool;
    XrCoroutine rejected_coro;
    atomic_int destroy_count;

    memset(&runtime, 0, sizeof(runtime));
    memset(&rejected_coro, 0, sizeof(rejected_coro));
    atomic_store(&rejected_coro.flags, XR_CORO_FLG_READY | XR_CORO_FLG_CANCEL_REQUESTED);
    atomic_init(&destroy_count, 0);

    xr_async_pool_init(&pool, &runtime, 1, 1);

    XrAsyncJob *first = xr_async_job_create(NULL, 0, noop_invoke, &destroy_count);
    ASSERT_NOT_NULL(first);
    first->destroy_data = count_destroy;
    ASSERT_TRUE(xr_async_submit(&pool, first));

    XrAsyncJob *second = xr_async_job_create(&rejected_coro, 0, noop_invoke, &destroy_count);
    ASSERT_NOT_NULL(second);
    second->destroy_data = count_destroy;
    ASSERT_FALSE(xr_async_submit(&pool, second));
    ASSERT_FALSE(xr_coro_flags_has(&rejected_coro, XR_CORO_FLG_BLOCKED));
    ASSERT_TRUE(xr_coro_flags_has(&rejected_coro, XR_CORO_FLG_READY));
    ASSERT_TRUE(xr_coro_flags_has(&rejected_coro, XR_CORO_FLG_CANCEL_REQUESTED));
    ASSERT_EQ_INT(xr_flag_to_state(atomic_load(&rejected_coro.flags)), XR_CORO_STATE_READY);

    ASSERT_EQ_INT(atomic_load_explicit(&pool.queue_depth, memory_order_relaxed), 1);
    ASSERT_EQ_INT(atomic_load_explicit(&pool.max_queue_depth, memory_order_relaxed), 1);
    ASSERT_EQ_INT((int) atomic_load_explicit(&pool.submit_count, memory_order_relaxed), 1);
    ASSERT_EQ_INT((int) atomic_load_explicit(&pool.reject_count, memory_order_relaxed), 1);

    xr_async_job_free(second);
    ASSERT_EQ_INT(atomic_load_explicit(&destroy_count, memory_order_relaxed), 1);

    xr_async_pool_destroy(&pool);
    ASSERT_EQ_INT(atomic_load_explicit(&destroy_count, memory_order_relaxed), 2);
    xr_coro_free(&rejected_coro);
}

TEST(async_submit_rejects_after_shutdown) {
    XrRuntime runtime;
    XrAsyncPool pool;
    atomic_int destroy_count;

    memset(&runtime, 0, sizeof(runtime));
    atomic_init(&destroy_count, 0);
    xr_async_pool_init(&pool, &runtime, 1, 2);

    xr_mutex_lock(&pool.queue_mutex);
    pool.running = false;
    xr_mutex_unlock(&pool.queue_mutex);

    XrAsyncJob *job = xr_async_job_create(NULL, 0, noop_invoke, &destroy_count);
    ASSERT_NOT_NULL(job);
    job->destroy_data = count_destroy;
    ASSERT_FALSE(xr_async_submit(&pool, job));
    ASSERT_EQ_INT((int) atomic_load_explicit(&pool.reject_count, memory_order_relaxed), 1);

    xr_async_job_free(job);
    ASSERT_EQ_INT(atomic_load_explicit(&destroy_count, memory_order_relaxed), 1);

    xr_async_pool_destroy(&pool);
}

TEST(async_coroutine_free_detaches_pending_job) {
    XrRuntime runtime = {0};
    XrAsyncPool pool;
    XrCoroutine coro = {0};
    atomic_int destroy_count;
    atomic_init(&destroy_count, 0);
    xr_async_pool_init(&pool, &runtime, 1, 2);
    XrAsyncJob *job = xr_async_job_create(&coro, 0, noop_invoke, &destroy_count);
    ASSERT_NOT_NULL(job);
    job->destroy_data = count_destroy;
    ASSERT_TRUE(xr_async_submit(&pool, job));
    xr_coro_free(&coro);
    ASSERT_TRUE(job->coro == NULL);
    ASSERT_EQ_INT(atomic_load(&destroy_count), 0);
    xr_async_pool_destroy(&pool);
    ASSERT_EQ_INT(atomic_load(&destroy_count), 1);
}

typedef struct AsyncProbe {
    atomic_bool entered;
    atomic_bool release;
    atomic_int invoked;
    atomic_int destroyed;
} AsyncProbe;

static void probe_invoke(void *data) {
    AsyncProbe *probe = data;
    atomic_fetch_add(&probe->invoked, 1);
    atomic_store_explicit(&probe->entered, true, memory_order_release);
    while (!atomic_load_explicit(&probe->release, memory_order_acquire))
        xr_thread_yield();
}

static void probe_destroy(void *data) {
    AsyncProbe *probe = data;
    atomic_fetch_add(&probe->destroyed, 1);
}

static bool wait_for_probe(AsyncProbe *probe) {
    uint64_t deadline = xr_time_monotonic_ms() + 5000;
    while (!atomic_load_explicit(&probe->entered, memory_order_acquire)) {
        if (xr_time_monotonic_ms() >= deadline)
            return false;
        xr_thread_yield();
    }
    return true;
}

static bool wait_for_completion(XrAsyncPool *pool) {
    uint64_t deadline = xr_time_monotonic_ms() + 5000;
    while (!atomic_load_explicit(&pool->ready_queues[0].head, memory_order_acquire)) {
        if (xr_time_monotonic_ms() >= deadline)
            return false;
        xr_thread_yield();
    }
    return true;
}

TEST(async_repeated_submit_preserves_active_wait) {
    XrRuntime runtime = {0};
    XrAsyncPool pool;
    XrCoroutine coro = {0};
    xr_async_pool_init(&pool, &runtime, 1, 2);
    XrAsyncJob *first = xr_async_job_create(&coro, 0, noop_invoke, NULL);
    XrAsyncJob *second = xr_async_job_create(&coro, 0, noop_invoke, NULL);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);
    ASSERT_TRUE(xr_async_submit(&pool, first));
    uint32_t flags = xr_coro_flags_load(&coro);
    ASSERT_FALSE(xr_async_submit(&pool, second));
    ASSERT_EQ_INT(xr_coro_flags_load(&coro), flags);
    ASSERT_TRUE(coro.ext->async_job == first);
    xr_async_job_free(second);
    xr_async_pool_destroy(&pool);
    ASSERT_TRUE(coro.ext->async_job == NULL);
    ASSERT_TRUE(atomic_load(&coro.ext->async_pool) == NULL);
    xr_coro_free(&coro);
}

TEST(async_inflight_job_outlives_destroyed_coroutine) {
    XrRuntime runtime = {0};
    XrAsyncPool pool;
    AsyncProbe probe = {0};
    XrCoroutine *coro = xr_calloc(1, sizeof(*coro));
    ASSERT_NOT_NULL(coro);
    coro->gc_flags = XR_CORO_GC_LIGHTWEIGHT;
    xr_async_pool_init(&pool, &runtime, 1, 2);
    XrAsyncJob *job = xr_async_job_create(coro, 0, probe_invoke, &probe);
    ASSERT_NOT_NULL(job);
    job->destroy_data = probe_destroy;
    ASSERT_TRUE(xr_async_submit(&pool, job));
    ASSERT_TRUE(xr_async_pool_start_threads(&pool));
    bool entered = wait_for_probe(&probe);
    xr_coro_destroy(coro);
    bool detached = job->coro == NULL;
    int early_destroy = atomic_load(&probe.destroyed);
    atomic_store_explicit(&probe.release, true, memory_order_release);
    bool completed = wait_for_completion(&pool);
    int drained = xr_async_check_ready(&pool, 0);
    xr_async_pool_destroy(&pool);
    ASSERT_TRUE(entered);
    ASSERT_TRUE(detached);
    ASSERT_EQ_INT(early_destroy, 0);
    ASSERT_TRUE(completed);
    ASSERT_EQ_INT(drained, 1);
    ASSERT_EQ_INT(atomic_load(&probe.invoked), 1);
    ASSERT_EQ_INT(atomic_load(&probe.destroyed), 1);
}

TEST(async_late_completion_preserves_new_wait) {
    XrRuntime runtime = {0};
    XrAsyncPool pool;
    XrCoroutine coro = {0};
    AsyncProbe old_probe = {0};
    AsyncProbe new_probe = {0};
    atomic_store(&old_probe.release, true);
    xr_async_pool_init(&pool, &runtime, 1, 2);
    XrAsyncJob *first = xr_async_job_create(&coro, 0, probe_invoke, &old_probe);
    XrAsyncJob *second = xr_async_job_create(&coro, 0, probe_invoke, &new_probe);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);
    first->destroy_data = probe_destroy;
    second->destroy_data = probe_destroy;
    ASSERT_TRUE(xr_async_submit(&pool, first));
    xr_async_detach_coro(&coro);
    ASSERT_TRUE(xr_async_submit(&pool, second));
    ASSERT_TRUE(xr_async_pool_start_threads(&pool));
    bool entered = wait_for_probe(&new_probe);
    int old_drained = xr_async_check_ready(&pool, 0);
    bool new_link_preserved = coro.ext->async_job == second;
    atomic_store_explicit(&new_probe.release, true, memory_order_release);
    bool completed = wait_for_completion(&pool);
    int new_drained = xr_async_check_ready(&pool, 0);
    xr_async_pool_destroy(&pool);
    ASSERT_TRUE(entered);
    ASSERT_EQ_INT(old_drained, 1);
    ASSERT_TRUE(new_link_preserved);
    ASSERT_TRUE(completed);
    ASSERT_EQ_INT(new_drained, 1);
    ASSERT_TRUE(coro.ext->async_job == NULL);
    ASSERT_TRUE(atomic_load(&coro.ext->async_pool) == NULL);
    ASSERT_EQ_INT(atomic_load(&old_probe.destroyed), 1);
    ASSERT_EQ_INT(atomic_load(&new_probe.destroyed), 1);
    xr_coro_free(&coro);
}

typedef struct AsyncDrain {
    XrAsyncPool *pool;
    atomic_bool stop;
    int drained;
} AsyncDrain;

static void *drain_completions(void *data) {
    AsyncDrain *drain = data;
    while (!atomic_load_explicit(&drain->stop, memory_order_acquire)) {
        drain->drained += xr_async_check_ready(drain->pool, 0);
        xr_thread_yield();
    }
    drain->drained += xr_async_check_ready(drain->pool, 0);
    return NULL;
}

TEST(async_completion_races_coroutine_destruction) {
    XrRuntime runtime = {0};
    XrAsyncPool pool;
    AsyncProbe probe = {0};
    AsyncDrain drain = {0};
    xr_thread_t consumer;
    atomic_store(&probe.release, true);
    xr_async_pool_init(&pool, &runtime, 4, 512);
    drain.pool = &pool;
    ASSERT_TRUE(xr_async_pool_start_threads(&pool));
    ASSERT_TRUE(xr_thread_create(&consumer, drain_completions, &drain));
    int submitted = 0;
    for (int i = 0; i < 256; i++) {
        XrCoroutine *coro = xr_calloc(1, sizeof(*coro));
        if (!coro)
            break;
        coro->gc_flags = XR_CORO_GC_LIGHTWEIGHT;
        XrAsyncJob *job = xr_async_job_create(coro, 0, probe_invoke, &probe);
        if (!job) {
            xr_coro_destroy(coro);
            break;
        }
        job->destroy_data = probe_destroy;
        bool accepted = xr_async_submit(&pool, job);
        if (!accepted)
            xr_async_job_free(job);
        xr_coro_destroy(coro);
        if (!accepted)
            break;
        submitted++;
    }
    uint64_t deadline = xr_time_monotonic_ms() + 5000;
    while (atomic_load(&probe.destroyed) < submitted && xr_time_monotonic_ms() < deadline)
        xr_thread_yield();
    atomic_store_explicit(&drain.stop, true, memory_order_release);
    int joined = xr_thread_join(consumer, NULL);
    xr_async_pool_destroy(&pool);
    ASSERT_EQ_INT(joined, 0);
    ASSERT_EQ_INT(submitted, 256);
    ASSERT_EQ_INT(drain.drained, 256);
    ASSERT_EQ_INT(atomic_load(&probe.invoked), 256);
    ASSERT_EQ_INT(atomic_load(&probe.destroyed), 256);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Async Pool");
RUN_TEST(async_submit_rejects_when_queue_full);
RUN_TEST(async_submit_rejects_after_shutdown);
RUN_TEST(async_coroutine_free_detaches_pending_job);
RUN_TEST(async_repeated_submit_preserves_active_wait);
RUN_TEST(async_inflight_job_outlives_destroyed_coroutine);
RUN_TEST(async_late_completion_preserves_new_wait);
RUN_TEST(async_completion_races_coroutine_destruction);

TEST_MAIN_END()
