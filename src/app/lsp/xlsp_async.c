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
#include "../../base/xchecks.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#ifndef XR_OS_WINDOWS
#include <unistd.h>
#include <fcntl.h>
#define xr_pipe(fds) pipe(fds)
#define xr_pipe_read(fd, buf, n) read((fd), (buf), (n))
#define xr_pipe_write(fd, buf, n) write((fd), (buf), (n))
#define xr_pipe_close(fd) close(fd)
static inline void xr_pipe_set_nonblocking(int fd) {
    int flags=fcntl(fd,F_GETFL,0);
    fcntl(fd,F_SETFL,flags|O_NONBLOCK);
}
#endif

// ============================================================================
// Task queues preserve task identity until the completion callback returns.
// ============================================================================

static void queue_init(XrLspTaskQueue *q) {
    q->head=NULL;q->tail=NULL;xr_mutex_init(&q->consumer_lock);
}
static void queue_destroy(XrLspTaskQueue *q) {
    XR_CHECK(!q->head&&!q->tail,"Task owners must be drained before queue destruction");
    xr_mutex_destroy(&q->consumer_lock);
}
static void queue_push(XrLspTaskQueue *q,XrLspTask *task) {
    task->next=NULL;xr_mutex_lock(&q->consumer_lock);
    if(q->tail)q->tail->next=task;else q->head=task;
    q->tail=task;xr_mutex_unlock(&q->consumer_lock);
}
static XrLspTask *queue_pop(XrLspTaskQueue *q) {
    xr_mutex_lock(&q->consumer_lock);XrLspTask *task=q->head;
    if(task){q->head=task->next;if(!q->head)q->tail=NULL;task->next=NULL;}
    xr_mutex_unlock(&q->consumer_lock);return task;
}
static int queue_cancel_if(XrLspTaskQueue *q,bool (*filter)(XrLspTask *,void *),void *ctx) {
    int cancelled=0;xr_mutex_lock(&q->consumer_lock);
    for(XrLspTask *task=q->head;task;task=task->next) {
        int expected=XLSP_TASK_PENDING;
        if(filter(task,ctx)&&atomic_compare_exchange_strong(&task->state,&expected,XLSP_TASK_CANCELLED)) {
            atomic_store(&task->cancel_requested,true);++cancelled;
        }
    }
    xr_mutex_unlock(&q->consumer_lock);return cancelled;
}
static void task_completed(XrLspAsync *async,XrLspTask *task) {
    XrLspTaskQueue *q=&async->completed;task->next=NULL;
    xr_mutex_lock(&q->consumer_lock);
    int previous=atomic_fetch_add(&async->completed_count,1);
    if(q->tail)q->tail->next=task;else q->head=task;
    q->tail=task;xr_mutex_unlock(&q->consumer_lock);
#ifndef XR_OS_WINDOWS
    if(!previous){char byte=1;(void)xr_pipe_write(async->notify_fd[1],&byte,1);}
#else
    (void)previous;
#endif
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
        // Cancellation holds this lock across both current and pending owners.
        xr_mutex_lock(&async->current_task_mutex);
        XrLspTask *task=queue_pop(&async->pending);
        async->current_task=task;
        xr_mutex_unlock(&async->current_task_mutex);
        if(!task)continue;

        // Check if already cancelled before starting
        if (atomic_load(&task->cancel_requested)) {
            atomic_store(&task->state, XLSP_TASK_CANCELLED);
            atomic_fetch_sub(&async->pending_count, 1);
            atomic_fetch_add(&async->cancelled_tasks, 1);
            // Push to completed queue so complete handler can free task->data
            xr_mutex_lock(&async->current_task_mutex);
            async->current_task=NULL;
            xr_mutex_unlock(&async->current_task_mutex);
            task_completed(async,task);
            continue;
        }

        // Try to transition to running state
        int expected = XLSP_TASK_PENDING;
        if (!atomic_compare_exchange_strong(&task->state, &expected, XLSP_TASK_RUNNING)) {
            // Task was cancelled
            atomic_fetch_sub(&async->pending_count, 1);
            atomic_fetch_add(&async->cancelled_tasks, 1);
            // Push to completed queue so complete handler can free task->data
            xr_mutex_lock(&async->current_task_mutex);
            async->current_task=NULL;
            xr_mutex_unlock(&async->current_task_mutex);
            task_completed(async,task);
            continue;
        }

        atomic_fetch_sub(&async->pending_count, 1);

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
        task_completed(async,task);
    }

    return NULL;
}

// ============================================================================
// Public API
// ============================================================================

XrLspAsync *xlsp_async_new(void (*thread_init)(void *), void *thread_init_ctx) {
    XrLspAsync *async = xr_calloc(1, sizeof(XrLspAsync));
    if (!async)
        return NULL;

    async->notify_fd[0]=-1;async->notify_fd[1]=-1;
#ifndef XR_OS_WINDOWS
    // Windows polls the completion queue; CRT anonymous pipe reads can block.
    if (xr_pipe(async->notify_fd) < 0) {
        xr_free(async);
        return NULL;
    }

    // Set read end to non-blocking
    xr_pipe_set_nonblocking(async->notify_fd[0]);
    xr_pipe_set_nonblocking(async->notify_fd[1]);
#endif

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

    // Set thread-init hook before starting worker (avoids data race)
    async->thread_init = thread_init;
    async->thread_init_ctx = thread_init_ctx;

    // Start worker thread
    atomic_store(&async->running, true);
    if (!xr_thread_create(&async->worker, worker_thread, async)) {
#ifndef XR_OS_WINDOWS
        xr_pipe_close(async->notify_fd[0]);xr_pipe_close(async->notify_fd[1]);
#endif
        queue_destroy(&async->pending);
        queue_destroy(&async->completed);
        xr_mutex_destroy(&async->pending_mutex);
        xr_cond_destroy(&async->pending_cond);
        xr_mutex_destroy(&async->current_task_mutex);
        xr_free(async);
        return NULL;
    }

    return async;
}

void xlsp_async_free(XrLspAsync *async) {
    if (!async)
        return;

    (void)xlsp_async_cancel_all(async);
    // Signal shutdown
    atomic_store(&async->running, false);

    // Wake worker thread
    xr_mutex_lock(&async->pending_mutex);
    xr_cond_signal(&async->pending_cond);
    xr_mutex_unlock(&async->pending_mutex);

    // Wait for worker to exit
    XR_CHECK(xr_thread_join(async->worker,NULL)==0,"Worker must stop before its task owners are released");

    // Cleanup
#ifndef XR_OS_WINDOWS
    xr_pipe_close(async->notify_fd[0]);xr_pipe_close(async->notify_fd[1]);
#endif

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
    if (!async || !task || !atomic_load(&async->running))
        return 0;

    // Assign task ID
    uint64_t next=atomic_load(&async->next_task_id);
    for(;;) {
        if(!next||next==UINT64_MAX)return 0;
        if(atomic_compare_exchange_weak(&async->next_task_id,&next,next+1))break;
    }
    task->task_id=next;
    atomic_store(&task->state, XLSP_TASK_PENDING);
    atomic_store(&task->cancel_requested, false);

    // Push to pending queue
    xr_mutex_lock(&async->pending_mutex);
    atomic_fetch_add(&async->pending_count,1);
    queue_push(&async->pending,task);
    atomic_fetch_add(&async->total_tasks, 1);

    // Wake worker thread
    xr_cond_signal(&async->pending_cond);
    xr_mutex_unlock(&async->pending_mutex);

    return next;
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

#ifndef XR_OS_WINDOWS
    // Drain notification pipe
    char buf[64];
    while (xr_pipe_read(async->notify_fd[0], buf, sizeof(buf)) > 0) {
        // Consume all notifications
    }

#endif
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
    int cancelled=queue_cancel_if(&async->pending,filter_by_task_id,&ctx);
    xr_mutex_unlock(&async->current_task_mutex);return cancelled>0;
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
    cancelled+=queue_cancel_if(&async->pending,filter_by_request_id, &ctx);
    xr_mutex_unlock(&async->current_task_mutex);

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
    cancelled+=queue_cancel_if(&async->pending,filter_by_type, &ctx);
    xr_mutex_unlock(&async->current_task_mutex);

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
    cancelled+=queue_cancel_if(&async->pending,filter, ctx);
    xr_mutex_unlock(&async->current_task_mutex);

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
    cancelled+=queue_cancel_if(&async->pending,filter_all, NULL);
    xr_mutex_unlock(&async->current_task_mutex);

    return cancelled;
}
