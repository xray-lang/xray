/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xasync.h - Async thread pool definitions
 *
 * KEY CONCEPT:
 *   Fixed async threads for blocking ops, per-Worker completion queues.
 *   Use cases: File I/O, DNS lookup, blocking syscalls.
 */

#ifndef XASYNC_H
#define XASYNC_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "../os/os_thread.h"
#include "xworker.h"

// ========== Configuration ==========

// Async thread count (default 10)
#ifndef XR_ASYNC_THREAD_COUNT
#define XR_ASYNC_THREAD_COUNT 10
#endif

// Async thread stack size (128KB — must exceed TLS segment + guard page;
// on Linux/x86_64 with XR_THREAD_LOCAL the TLS block alone can be ~65KB)
#ifndef XR_ASYNC_STACK_SIZE
#define XR_ASYNC_STACK_SIZE (128 * 1024)
#endif

// ========== Async Task ==========

typedef struct XrAsyncJob {
    struct XrAsyncJob *next;

    // Task ownership
    struct XrCoroutine *coro;
    int worker_id;

    // Task function
    void (*invoke)(void *data);  // Execute in async thread
    void *data;

    // Execution result
    XrValue result;
    int error;  // 0 = success
} XrAsyncJob;

// ========== Completion Queue (one per Worker) ==========
//
// Lock-free MPSC (multi async-thread producers, single owner
// worker consumer). Implemented as a Treiber stack with O(1) bulk drain
// via atomic_exchange. Order-of-completion is not preserved across
// pushes, which is fine since the consumer already iterates the whole
// list in one go via xr_async_check_ready.
typedef struct XrAsyncReadyQueue {
    _Atomic(XrAsyncJob *) head;  // Top of stack (newest pushed)
} XrAsyncReadyQueue;

// ========== Async Thread Pool ==========

typedef struct XrAsyncPool {
    // Thread management
    int thread_count;
    xr_thread_t *threads;
    // Shutdown flag. Plain bool; every read and write happens with
    // queue_mutex held, so the mutex's acquire/release fences carry the
    // happens-before edge. Using _Atomic here surfaces as MSan
    // use-of-uninitialized-value at xasync.c:97 on Clang/MSan because
    // Clang does not always propagate shadow bytes through
    // __atomic_load builtins for _Atomic bool.
    bool running;

    // Task queue
    XrAsyncJob *queue_head;
    XrAsyncJob *queue_tail;
    xr_mutex_t queue_mutex;
    xr_cond_t queue_cond;

    // Completion queues (one per Worker)
    XrAsyncReadyQueue ready_queues[XR_MAX_WORKERS];

    // Owner Runtime
    struct XrRuntime *runtime;
} XrAsyncPool;

// ========== API ==========

// Initialize async thread pool (data structures only, no threads)
XR_FUNC void xr_async_pool_init(XrAsyncPool *pool, struct XrRuntime *runtime, int thread_count);

// Create async threads (called lazily on first spawn)
XR_FUNC void xr_async_pool_start_threads(XrAsyncPool *pool);

// Destroy async thread pool
XR_FUNC void xr_async_pool_destroy(XrAsyncPool *pool);

// Submit async task (coroutine suspended until completion)
XR_FUNC void xr_async_submit(XrAsyncPool *pool, XrAsyncJob *job);

// Check completion queue, wake coroutines (called in Worker loop)
XR_FUNC int xr_async_check_ready(XrAsyncPool *pool, int worker_id);

// Create async task
XR_FUNC XrAsyncJob *xr_async_job_create(struct XrCoroutine *coro, int worker_id,
                                        void (*invoke)(void *), void *data);

// Free async task
XR_FUNC void xr_async_job_free(XrAsyncJob *job);

#endif  // XASYNC_H
