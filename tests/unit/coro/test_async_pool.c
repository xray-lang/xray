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
    atomic_store(&rejected_coro.coro_state, XR_CORO_STATE_READY);
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
    ASSERT_EQ_INT(atomic_load(&rejected_coro.coro_state), XR_CORO_STATE_READY);

    ASSERT_EQ_INT(atomic_load_explicit(&pool.queue_depth, memory_order_relaxed), 1);
    ASSERT_EQ_INT(atomic_load_explicit(&pool.max_queue_depth, memory_order_relaxed), 1);
    ASSERT_EQ_INT((int) atomic_load_explicit(&pool.submit_count, memory_order_relaxed), 1);
    ASSERT_EQ_INT((int) atomic_load_explicit(&pool.reject_count, memory_order_relaxed), 1);

    xr_async_job_free(second);
    ASSERT_EQ_INT(atomic_load_explicit(&destroy_count, memory_order_relaxed), 1);

    xr_async_pool_destroy(&pool);
    ASSERT_EQ_INT(atomic_load_explicit(&destroy_count, memory_order_relaxed), 2);
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

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Async Pool");
RUN_TEST(async_submit_rejects_when_queue_full);
RUN_TEST(async_submit_rejects_after_shutdown);

TEST_MAIN_END()
