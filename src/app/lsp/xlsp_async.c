/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_async.c - Background task system implementation
 */

#include "xlsp_async.h"
#include "../../base/xmalloc.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>

// ============================================================================
// Task Queue (Lock-free MPSC with cancellation support)
// ============================================================================

static void queue_init(XrLspTaskQueue *q) {
    // Stub node for simpler implementation
    XrLspTask *stub = xr_calloc(1, sizeof(XrLspTask));
    stub->next = NULL;
    atomic_store(&q->head, stub);
    q->tail = stub;
    xr_mutex_init(&q->consumer_lock);
    xr_rwlock_init(&q->traverse_lock);
}

static void queue_destroy(XrLspTaskQueue *q) {
    // Free stub and any remaining tasks
    XrLspTask *node = q->tail;
    while (node) {
        XrLspTask *next = node->next;
        xr_free(node);
        node = next;
    }
    xr_mutex_destroy(&q->consumer_lock);
    xr_rwlock_destroy(&q->traverse_lock);
}

// Push (thread-safe, lock-free)
static void queue_push(XrLspTaskQueue *q, XrLspTask *task) {
    task->next = NULL;
    XrLspTask *prev = atomic_exchange(&q->head, task);
    prev->next = task;
}

// Pop (single consumer, needs lock if multiple consumers)
static XrLspTask *queue_pop(XrLspTaskQueue *q) {
    xr_mutex_lock(&q->consumer_lock);

    XrLspTask *tail = q->tail;
    XrLspTask *next = tail->next;

    if (next) {
        // Advance tail: old stub (tail) is freed, next becomes new stub
        // but first we return a copy of next's data
        // Use tail's memory to hold the result (reuse allocation)
        XrLspTask *result = tail;
        *result = *next;
        result->next = NULL;

        // next becomes the new stub (clear its task fields)
        next->execute = NULL;
        next->complete = NULL;
        next->data = NULL;
        next->result = NULL;
        next->task_id = 0;
        next->request_id = 0;
        q->tail = next;

        xr_mutex_unlock(&q->consumer_lock);
        return result;
    }

    xr_mutex_unlock(&q->consumer_lock);
    return NULL;
}

// Mark tasks as cancelled by filter (traverses queue safely)
// Returns count of tasks marked for cancellation
static int queue_cancel_if(XrLspTaskQueue *q, bool (*filter)(XrLspTask *task, void *ctx),
                           void *ctx) {
    int cancelled = 0;

    xr_rwlock_wrlock(&q->traverse_lock);
    xr_mutex_lock(&q->consumer_lock);

    // Traverse from tail (oldest) to head
    XrLspTask *node = q->tail;
    while (node) {
        XrLspTask *next = node->next;
        if (next && next->execute) {  // Skip stub nodes
            if (filter(next, ctx)) {
                // Mark for cancellation
                int expected = XLSP_TASK_PENDING;
                if (atomic_compare_exchange_strong(&next->state, &expected, XLSP_TASK_CANCELLED)) {
                    atomic_store(&next->cancel_requested, true);
                    cancelled++;
                }
            }
        }
        node = next;
    }

    xr_mutex_unlock(&q->consumer_lock);
    xr_rwlock_wrunlock(&q->traverse_lock);

    return cancelled;
}

// ============================================================================
// Worker Thread
// ============================================================================

static void *worker_thread(void *arg) {
    XrLspAsync *async = (XrLspAsync *) arg;

    // Run the optional per-thread init (e.g. to install tls_server so
    // lsp_log inside task execution writes to the same file as the main
    // loop instead of silently falling through to stderr-only).
    if (async->thread_init) {
        async->thread_init(async->thread_init_ctx);
    }

    while (atomic_load(&async->running)) {
        // Wait for tasks
        xr_mutex_lock(&async->pending_mutex);
        while (atomic_load(&async->pending_count) == 0 && atomic_load(&async->running)) {
            xr_cond_wait(&async->pending_cond, &async->pending_mutex);
        }
        xr_mutex_unlock(&async->pending_mutex);

        if (!atomic_load(&async->running))
            break;

        // Pop task from pending queue
        XrLspTask *task = queue_pop(&async->pending);
        if (!task)
            continue;

        // Check if already cancelled before starting
        if (atomic_load(&task->cancel_requested)) {
            atomic_store(&task->state, XLSP_TASK_CANCELLED);
            atomic_fetch_sub(&async->pending_count, 1);
            atomic_fetch_add(&async->cancelled_tasks, 1);
            // Push to completed queue so complete handler can free task->data
            queue_push(&async->completed, task);
            atomic_fetch_add(&async->completed_count, 1);
            char byte = 1;
            ssize_t n = write(async->notify_fd[1], &byte, 1);
            (void) n;
            continue;
        }

        // Try to transition to running state
        int expected = XLSP_TASK_PENDING;
        if (!atomic_compare_exchange_strong(&task->state, &expected, XLSP_TASK_RUNNING)) {
            // Task was cancelled
            atomic_fetch_sub(&async->pending_count, 1);
            atomic_fetch_add(&async->cancelled_tasks, 1);
            // Push to completed queue so complete handler can free task->data
            queue_push(&async->completed, task);
            atomic_fetch_add(&async->completed_count, 1);
            char byte = 1;
            ssize_t n = write(async->notify_fd[1], &byte, 1);
            (void) n;
            continue;
        }

        atomic_fetch_sub(&async->pending_count, 1);

        // Set as current task (for cancellation while running)
        // Protected by mutex to prevent TOCTOU race in cancel functions
        xr_mutex_lock(&async->current_task_mutex);
        async->current_task = task;
        xr_mutex_unlock(&async->current_task_mutex);

        // Execute task
        if (task->execute) {
            task->execute(task->data);
        }

        // Clear current task before pushing to completed queue
        // This ensures cancel functions won't access a task being freed
        xr_mutex_lock(&async->current_task_mutex);
        async->current_task = NULL;
        xr_mutex_unlock(&async->current_task_mutex);

        // Check if cancelled during execution
        if (atomic_load(&task->cancel_requested)) {
            atomic_store(&task->state, XLSP_TASK_CANCELLED);
            atomic_fetch_add(&async->cancelled_tasks, 1);
            // Still push to completed queue so cleanup can happen
        } else {
            // Mark completed
            atomic_store(&task->state, XLSP_TASK_COMPLETED);
        }

        // Push to completed queue
        queue_push(&async->completed, task);
        atomic_fetch_add(&async->completed_count, 1);

        // Notify main thread
        char byte = 1;
        ssize_t n = write(async->notify_fd[1], &byte, 1);
        (void) n;  // Ignore write errors
    }

    return NULL;
}

// ============================================================================
// Public API
// ============================================================================

XrLspAsync *xlsp_async_new(void) {
    XrLspAsync *async = xr_calloc(1, sizeof(XrLspAsync));
    if (!async)
        return NULL;

    // Create notification pipe
    if (pipe(async->notify_fd) < 0) {
        xr_free(async);
        return NULL;
    }

    // Set read end to non-blocking
    int flags = fcntl(async->notify_fd[0], F_GETFL, 0);
    fcntl(async->notify_fd[0], F_SETFL, flags | O_NONBLOCK);

    // Initialize queues
    queue_init(&async->pending);
    queue_init(&async->completed);

    // Initialize synchronization
    xr_mutex_init(&async->pending_mutex);
    xr_cond_init(&async->pending_cond);
    xr_mutex_init(&async->current_task_mutex);

    // Initialize task ID generator (start from 1)
    atomic_store(&async->next_task_id, 1);
    async->current_task = NULL;

    // Start worker thread
    atomic_store(&async->running, true);
    if (!xr_thread_create(&async->worker, worker_thread, async)) {
        close(async->notify_fd[0]);
        close(async->notify_fd[1]);
        queue_destroy(&async->pending);
        queue_destroy(&async->completed);
        xr_mutex_destroy(&async->pending_mutex);
        xr_cond_destroy(&async->pending_cond);
        xr_free(async);
        return NULL;
    }

    return async;
}

void xlsp_async_free(XrLspAsync *async) {
    if (!async)
        return;

    // Signal shutdown
    atomic_store(&async->running, false);

    // Wake worker thread
    xr_mutex_lock(&async->pending_mutex);
    xr_cond_signal(&async->pending_cond);
    xr_mutex_unlock(&async->pending_mutex);

    // Wait for worker to exit
    xr_thread_join(async->worker, NULL);

    // Cleanup
    close(async->notify_fd[0]);
    close(async->notify_fd[1]);

    // Drain remaining tasks, calling complete handlers to free task->data
    XrLspTask *task;
    while ((task = queue_pop(&async->pending)) != NULL) {
        if (task->complete) {
            task->complete(task->data, NULL);
        }
        xlsp_task_free(task);
    }

    while ((task = queue_pop(&async->completed)) != NULL) {
        if (task->complete) {
            task->complete(task->data, NULL);
        }
        xlsp_task_free(task);
    }

    queue_destroy(&async->pending);
    queue_destroy(&async->completed);
    xr_mutex_destroy(&async->pending_mutex);
    xr_cond_destroy(&async->pending_cond);
    xr_mutex_destroy(&async->current_task_mutex);

    xr_free(async);
}

uint64_t xlsp_async_submit(XrLspAsync *async, XrLspTask *task) {
    if (!async || !task)
        return 0;

    // Assign task ID
    task->task_id = atomic_fetch_add(&async->next_task_id, 1);
    atomic_store(&task->state, XLSP_TASK_PENDING);
    atomic_store(&task->cancel_requested, false);

    // Push to pending queue
    queue_push(&async->pending, task);
    atomic_fetch_add(&async->pending_count, 1);
    atomic_fetch_add(&async->total_tasks, 1);

    // Wake worker thread
    xr_mutex_lock(&async->pending_mutex);
    xr_cond_signal(&async->pending_cond);
    xr_mutex_unlock(&async->pending_mutex);

    return task->task_id;
}

XrLspTask *xlsp_task_new(XrLspTaskFn execute, XrLspCompleteFn complete, void *data,
                         XrLspTaskPriority priority) {
    return xlsp_task_new_ex(execute, complete, data, priority, XLSP_TASK_TYPE_GENERIC, 0);
}

XrLspTask *xlsp_task_new_ex(XrLspTaskFn execute, XrLspCompleteFn complete, void *data,
                            XrLspTaskPriority priority, XrLspTaskType type, int64_t request_id) {
    XrLspTask *task = xr_calloc(1, sizeof(XrLspTask));
    if (!task)
        return NULL;

    task->execute = execute;
    task->complete = complete;
    task->data = data;
    task->priority = priority;
    task->type = type;
    task->request_id = request_id;
    task->task_id = 0;  // Will be assigned on submit
    atomic_store(&task->state, XLSP_TASK_PENDING);
    atomic_store(&task->cancel_requested, false);

    return task;
}

void xlsp_task_free(XrLspTask *task) {
    if (task) {
        xr_free(task);
    }
}

int xlsp_async_poll(XrLspAsync *async) {
    if (!async)
        return 0;

    // Drain notification pipe
    char buf[64];
    while (read(async->notify_fd[0], buf, sizeof(buf)) > 0) {
        // Consume all notifications
    }

    int processed = 0;
    XrLspTask *task;

    while ((task = queue_pop(&async->completed)) != NULL) {
        atomic_fetch_sub(&async->completed_count, 1);

        // Check task state
        int state = atomic_load(&task->state);

        // Call completion handler in main thread
        // For cancelled tasks, complete handler can check task state
        if (task->complete) {
            // Pass NULL result for cancelled tasks
            void *result = (state == XLSP_TASK_CANCELLED) ? NULL : task->result;
            task->complete(task->data, result);
        }

        xlsp_task_free(task);
        processed++;
    }

    return processed;
}

int xlsp_async_get_notify_fd(XrLspAsync *async) {
    return async ? async->notify_fd[0] : -1;
}

// Filter context for various cancel operations
typedef struct {
    uint64_t task_id;
    int64_t request_id;
    XrLspTaskType type;
} CancelFilterCtx;

// Filter: match by task ID
static bool filter_by_task_id(XrLspTask *task, void *ctx) {
    CancelFilterCtx *c = (CancelFilterCtx *) ctx;
    return task->task_id == c->task_id;
}

// Filter: match by request ID
static bool filter_by_request_id(XrLspTask *task, void *ctx) {
    CancelFilterCtx *c = (CancelFilterCtx *) ctx;
    return task->request_id == c->request_id;
}

// Filter: match by task type
static bool filter_by_type(XrLspTask *task, void *ctx) {
    CancelFilterCtx *c = (CancelFilterCtx *) ctx;
    return task->type == c->type;
}

// Filter: match all
static bool filter_all(XrLspTask *task, void *ctx) {
    (void) task;
    (void) ctx;
    return true;
}

// Cancel by task ID
bool xlsp_async_cancel_task(XrLspAsync *async, uint64_t task_id) {
    if (!async || task_id == 0)
        return false;

    CancelFilterCtx ctx = {.task_id = task_id};

    // First, check if this is the currently running task
    // Use mutex to prevent TOCTOU race with worker thread
    xr_mutex_lock(&async->current_task_mutex);
    XrLspTask *current = async->current_task;
    if (current && current->task_id == task_id) {
        atomic_store(&current->cancel_requested, true);
        xr_mutex_unlock(&async->current_task_mutex);
        return true;
    }
    xr_mutex_unlock(&async->current_task_mutex);

    // Try to cancel in pending queue
    int cancelled = queue_cancel_if(&async->pending, filter_by_task_id, &ctx);
    return cancelled > 0;
}

// Cancel by LSP request ID
int xlsp_async_cancel_request(XrLspAsync *async, int64_t request_id) {
    if (!async || request_id == 0)
        return 0;

    CancelFilterCtx ctx = {.request_id = request_id};
    int cancelled = 0;

    // Check currently running task with mutex protection
    xr_mutex_lock(&async->current_task_mutex);
    XrLspTask *current = async->current_task;
    if (current && current->request_id == request_id) {
        atomic_store(&current->cancel_requested, true);
        cancelled++;
    }
    xr_mutex_unlock(&async->current_task_mutex);

    // Cancel in pending queue
    cancelled += queue_cancel_if(&async->pending, filter_by_request_id, &ctx);
    return cancelled;
}

// Cancel by task type
int xlsp_async_cancel_type(XrLspAsync *async, XrLspTaskType type) {
    if (!async)
        return 0;

    CancelFilterCtx ctx = {.type = type};
    int cancelled = 0;

    // Check currently running task with mutex protection
    xr_mutex_lock(&async->current_task_mutex);
    XrLspTask *current = async->current_task;
    if (current && current->type == type) {
        atomic_store(&current->cancel_requested, true);
        cancelled++;
    }
    xr_mutex_unlock(&async->current_task_mutex);

    // Cancel in pending queue
    cancelled += queue_cancel_if(&async->pending, filter_by_type, &ctx);
    return cancelled;
}

// Cancel with custom filter
int xlsp_async_cancel(XrLspAsync *async, bool (*filter)(XrLspTask *task, void *ctx), void *ctx) {
    if (!async || !filter)
        return 0;

    int cancelled = 0;

    // Check currently running task with mutex protection
    xr_mutex_lock(&async->current_task_mutex);
    XrLspTask *current = async->current_task;
    if (current && filter(current, ctx)) {
        atomic_store(&current->cancel_requested, true);
        cancelled++;
    }
    xr_mutex_unlock(&async->current_task_mutex);

    // Cancel in pending queue
    cancelled += queue_cancel_if(&async->pending, filter, ctx);
    return cancelled;
}

// Cancel all pending tasks
int xlsp_async_cancel_all(XrLspAsync *async) {
    if (!async)
        return 0;

    int cancelled = 0;

    // Mark currently running task for cancellation with mutex protection
    xr_mutex_lock(&async->current_task_mutex);
    XrLspTask *current = async->current_task;
    if (current) {
        atomic_store(&current->cancel_requested, true);
        cancelled++;
    }
    xr_mutex_unlock(&async->current_task_mutex);

    // Cancel all in pending queue
    cancelled += queue_cancel_if(&async->pending, filter_all, NULL);
    return cancelled;
}
