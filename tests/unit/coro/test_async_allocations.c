/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_async_allocations.c - Async submission allocation failure atomicity
 */

#include "../program/xr_program_allocation_probe.h"
#include "../test_framework.h"
#include "coro/xasync.h"
#include "coro/xcoroutine.h"

static size_t attempt, fail_at, freed;

void *xr_program_test_malloc(size_t size) {
    return xr_program_test_system_malloc(size);
}

void *xr_program_test_calloc(size_t count, size_t size) {
    if (++attempt == fail_at)
        return NULL;
    return xr_program_test_system_calloc(count, size);
}

void *xr_program_test_realloc(void *pointer, size_t size) {
    return xr_program_test_system_realloc(pointer, size);
}

void xr_program_test_free(void *pointer) {
    if (pointer)
        ++freed;
    xr_program_test_system_free(pointer);
}

static void noop_invoke(void *data) {
    (void) data;
}

static void count_destroy(void *data) {
    (*(int *) data)++;
}

TEST(async_job_allocation_failure_keeps_caller_data) {
    int destroyed = 0;
    attempt = freed = 0;
    fail_at = 1;
    XrAsyncJob *job = xr_async_job_create(NULL, 0, noop_invoke, &destroyed);
    ASSERT_TRUE(job == NULL);
    ASSERT_EQ_INT(attempt, 1);
    ASSERT_EQ_INT(freed, 0);
    ASSERT_EQ_INT(destroyed, 0);
}

static void check_submit_failure(bool thread_array) {
    XrRuntime runtime = {0};
    XrAsyncPool pool;
    XrCoroutine coro = {0};
    int destroyed = 0;
    uint32_t flags = XR_CORO_FLG_READY | XR_CORO_FLG_CANCEL_REQUESTED;
    atomic_store(&coro.flags, flags);
    xr_async_pool_init(&pool, &runtime, 1, 2);
    if (thread_array)
        runtime.async_pool = &pool;
    attempt = freed = 0;
    fail_at = 2;
    XrAsyncJob *job = xr_async_job_create(&coro, 0, noop_invoke, &destroyed);
    ASSERT_NOT_NULL(job);
    job->destroy_data = count_destroy;
    ASSERT_FALSE(xr_async_submit(&pool, job));
    ASSERT_EQ_INT(attempt, 2);
    ASSERT_TRUE(coro.ext == NULL);
    ASSERT_TRUE(job->pool == NULL);
    ASSERT_EQ_INT(xr_coro_flags_load(&coro), flags);
    ASSERT_EQ_INT(atomic_load(&pool.queue_depth), 0);
    ASSERT_EQ_INT(atomic_load(&pool.submit_count), 0);
    ASSERT_EQ_INT(atomic_load(&pool.reject_count), 1);
    ASSERT_EQ_INT(destroyed, 0);
    xr_async_job_free(job);
    xr_async_pool_destroy(&pool);
    ASSERT_EQ_INT(destroyed, 1);
    ASSERT_EQ_INT(freed, 1);
}

TEST(async_extension_failure_does_not_publish_or_block) {
    check_submit_failure(false);
}

TEST(async_thread_array_failure_does_not_publish_or_block) {
    check_submit_failure(true);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Async Allocation Failure");
RUN_TEST(async_job_allocation_failure_keeps_caller_data);
RUN_TEST(async_extension_failure_does_not_publish_or_block);
RUN_TEST(async_thread_array_failure_does_not_publish_or_block);
TEST_MAIN_END()
