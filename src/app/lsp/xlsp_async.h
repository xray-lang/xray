/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_async.h - Background task system for LSP
 *
 * KEY CONCEPT:
 *   Single background worker thread for non-blocking operations.
 *   Uses pipe/eventfd to notify main thread when tasks complete.
 *   Main thread polls for completions in the event loop.
 */

#ifndef XLSP_ASYNC_H
#define XLSP_ASYNC_H

#include "../../os/os_thread.h"
#include <stdbool.h>
#include <stdatomic.h>
#include "../../base/xdefs.h"

// Task function types
typedef void (*XrLspTaskFn)(void *data);
typedef void (*XrLspCompleteFn)(void *data, void *result);

// Task priority (currently unused: worker uses FIFO order, reserved for future priority queue)
typedef enum {
    XLSP_TASK_LOW = 0,
    XLSP_TASK_NORMAL = 1,
    XLSP_TASK_HIGH = 2
} XrLspTaskPriority;

// Task state
typedef enum {
    XLSP_TASK_PENDING,
    XLSP_TASK_RUNNING,
    XLSP_TASK_COMPLETED,
    XLSP_TASK_CANCELLED
} XrLspTaskState;

// Task type for filtering
typedef enum {
    XLSP_TASK_TYPE_GENERIC = 0,
    XLSP_TASK_TYPE_INDEX,       // Workspace indexing
    XLSP_TASK_TYPE_DIAGNOSTIC,  // Diagnostic computation
    XLSP_TASK_TYPE_COMPLETION,  // Completion computation
    XLSP_TASK_TYPE_REFERENCES,  // References search
    XLSP_TASK_TYPE_RENAME,      // Rename preparation
} XrLspTaskType;

// Background task
typedef struct XrLspTask {
    XrLspTaskFn execute;       // Run in background thread
    XrLspCompleteFn complete;  // Run in main thread (optional)
    void *data;                // Task data
    void *result;              // Task result
    XrLspTaskPriority priority;
    XrLspTaskType type;  // Task type for filtering
    uint64_t task_id;    // Unique task ID
    int64_t request_id;  // Associated LSP request ID (0 if none)
    _Atomic int state;
    _Atomic bool cancel_requested;  // Cooperative cancellation flag
    struct XrLspTask *next;
} XrLspTask;

// Cancellation token (for cooperative cancellation in tasks)
typedef struct XrLspCancelToken {
    _Atomic bool cancelled;
} XrLspCancelToken;

// Task queue with cancellation support
typedef struct XrLspTaskQueue {
    _Atomic(XrLspTask *) head;
    XrLspTask *tail;
    xr_mutex_t consumer_lock;
    xr_rwlock_t traverse_lock;  // For safe traversal during cancellation
} XrLspTaskQueue;

// Async worker pool
typedef struct XrLspAsync {
    // Worker thread
    xr_thread_t worker;
    _Atomic bool running;

    // Pending task queue (main -> worker)
    XrLspTaskQueue pending;
    xr_mutex_t pending_mutex;
    xr_cond_t pending_cond;

    // Completed task queue (worker -> main)
    XrLspTaskQueue completed;

    // Currently running task (for cancellation)
    // Protected by current_task_mutex to prevent TOCTOU race conditions
    XrLspTask *current_task;
    xr_mutex_t current_task_mutex;

    // Notification pipe (worker -> main)
    int notify_fd[2];  // [0]=read, [1]=write

    // Task ID generator
    _Atomic uint64_t next_task_id;

    // Statistics
    _Atomic int pending_count;
    _Atomic int completed_count;
    _Atomic uint64_t total_tasks;
    _Atomic uint64_t cancelled_tasks;

    // Optional per-thread init hook, run once inside the worker thread
    // right after it starts. xlsp_server uses this to install the
    // thread-local logging server pointer so tasks dispatched on the worker
    // can write into the same log file as the main loop. The field is
    // written exactly once (before pthread_create) and read exactly once
    // (at the top of worker_thread), so no synchronisation is required.
    void (*thread_init)(void *ctx);
    void *thread_init_ctx;
} XrLspAsync;

// Initialize/destroy async system.
// thread_init/ctx are called once at worker startup (before any tasks).
// Pass NULL if no per-thread init is needed.
XR_FUNC XrLspAsync *xlsp_async_new(void (*thread_init)(void *), void *thread_init_ctx);
XR_FUNC void xlsp_async_free(XrLspAsync *async);

// Submit task (called from main thread)
// Returns task ID for tracking
XR_FUNC uint64_t xlsp_async_submit(XrLspAsync *async, XrLspTask *task);

// Create task with extended options
XR_FUNC XrLspTask *xlsp_task_new(XrLspTaskFn execute, XrLspCompleteFn complete, void *data,
                                 XrLspTaskPriority priority);
XR_FUNC XrLspTask *xlsp_task_new_ex(XrLspTaskFn execute, XrLspCompleteFn complete, void *data,
                                    XrLspTaskPriority priority, XrLspTaskType type,
                                    int64_t request_id);
XR_FUNC void xlsp_task_free(XrLspTask *task);

// Check for completed tasks (called from main thread)
// Returns number of completions processed
XR_FUNC int xlsp_async_poll(XrLspAsync *async);

// Get notification fd for select/poll
XR_FUNC int xlsp_async_get_notify_fd(XrLspAsync *async);

// Cancel tasks by various criteria
// Returns number of tasks cancelled

// Cancel by task ID
XR_FUNC bool xlsp_async_cancel_task(XrLspAsync *async, uint64_t task_id);

// Cancel by LSP request ID
XR_FUNC int xlsp_async_cancel_request(XrLspAsync *async, int64_t request_id);

// Cancel by task type
XR_FUNC int xlsp_async_cancel_type(XrLspAsync *async, XrLspTaskType type);

// Cancel with custom filter
XR_FUNC int xlsp_async_cancel(XrLspAsync *async, bool (*filter)(XrLspTask *task, void *ctx),
                              void *ctx);

// Cancel all pending tasks
XR_FUNC int xlsp_async_cancel_all(XrLspAsync *async);

// Check if task was cancelled (for cooperative cancellation in task execute)
// Call this periodically in long-running tasks
static inline bool xlsp_task_is_cancelled(XrLspTask *task) {
    return task && atomic_load(&task->cancel_requested);
}

// Check cancellation and return early if cancelled
// Usage: XLSP_CHECK_CANCEL(task, cleanup_code);
#define XLSP_CHECK_CANCEL(task, cleanup)                                                           \
    do {                                                                                           \
        if (xlsp_task_is_cancelled(task)) {                                                        \
            cleanup;                                                                               \
            return;                                                                                \
        }                                                                                          \
    } while (0)

// Get pending task count
static inline int xlsp_async_pending_count(XrLspAsync *async) {
    return atomic_load(&async->pending_count);
}

// Get task statistics
static inline uint64_t xlsp_async_cancelled_count(XrLspAsync *async) {
    return atomic_load(&async->cancelled_tasks);
}

#endif  // XLSP_ASYNC_H
