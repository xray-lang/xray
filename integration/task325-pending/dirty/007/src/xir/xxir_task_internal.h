/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task_internal.h - Charged FIFO task ownership and executor leases
 *
 * KEY CONCEPT:
 *   Terminal tasks keep value storage but no execution or code authority.
 */
#ifndef XXIR_TASK_INTERNAL_H
#define XXIR_TASK_INTERNAL_H
#include "xxir_task.h"
#include "xxir_program_internal.h"
#include "xxir_value_internal.h"
#include "xxir_call_internal.h"
typedef struct XrXirTaskExecutor XrXirTaskExecutor;
typedef enum XirTaskState {
    XIR_TASK_PREPARING, XIR_TASK_PENDING, XIR_TASK_RUNNING, XIR_TASK_TERMINAL
} XirTaskState;
typedef struct XirTaskActivation {
    XrXirCall *call;
    struct XirTask *task;
    struct XirTaskActivation *ready_next;
    uint64_t ticket;
    bool queued;
} XirTaskActivation;
typedef struct XirTask {
    XirObject object;
    XrXirCallResult outcome;
    XrXirCallAccounting accounting;
    XrXirCall *call;
    XrXirTaskExecutor *executor;
    struct XirTask *active_next, *active_previous;
    XrXirCall *waiter_head, *waiter_tail;
    XirTaskActivation activation;
    uint64_t generation, identity;
    _Atomic(XirTaskState) state;
} XirTask;
typedef struct XrXirTaskExecutorConfig {
    XrXirProgram *program;
    XrXirDomain *domain;
    XrXirTypeArena *task_arena;
    uint64_t call_limit, poll_limit, requested_value_limit, requested_call_limit, work_limit;
    uint32_t depth_limit;
    XrXirCallBudget *budget;
    XrXirInstance *instance;
    const XrXirCallEntry *entries;
    uint32_t entry_count;
    XrXirValueAdmission admission;
    XrXirOutputProvider output;
} XrXirTaskExecutorConfig;
XR_FUNC XrXirCallStatus xr_xir_task_executor_new(const XrXirTaskExecutorConfig *config,
    XrXirTaskExecutor **output);
XR_FUNC XrXirCallStatus xr_xir_task_executor_spawn(XrXirTaskExecutor *executor, XrXirType type,
    const XrXirCallRequest *request, XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_task_executor_spawn_from_view(XrXirTaskExecutor *executor,
    XrXirCallView *view, XrXirType type, const XrXirCallRequest *request, XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_task_executor_poll(XrXirTaskExecutor *executor, uint64_t quantum);
/* The instance owns its root Call; the executor only schedules the activation. */
XR_FUNC bool xr_xir_task_executor_root_idle(const XrXirTaskExecutor *executor);
/* A finished root may still carry a fault from child completion or cleanup. */
XR_FUNC XrXirCallStatus xr_xir_task_executor_completion_status(const XrXirTaskExecutor *executor);
XR_FUNC XrXirCallStatus xr_xir_task_executor_attach_root(XrXirTaskExecutor *executor, XrXirCall *call, uint64_t epoch);
XR_FUNC XrXirCallResult xr_xir_task_executor_poll_root(XrXirTaskExecutor *executor, uint64_t quantum);
XR_FUNC XrXirCallStatus xr_xir_task_executor_resume_root(XrXirTaskExecutor *executor, uint64_t epoch, uint64_t wake);
XR_FUNC XrXirCallStatus xr_xir_task_executor_root_wait(const XrXirTaskExecutor *executor, uint64_t epoch, uint64_t wake,
    XrXirWaitRequest *output);
XR_FUNC bool xr_xir_task_executor_view_member(const XrXirTaskExecutor *executor,
    const XrXirCallView *view, bool *child);
XR_FUNC bool xr_xir_task_executor_cleanup_active(const XrXirTaskExecutor *executor);
XR_FUNC XrXirCallStatus xr_xir_task_executor_cancel_root(XrXirTaskExecutor *executor);
XR_FUNC XrXirCallStatus xr_xir_task_executor_stop(XrXirTaskExecutor *executor);
XR_FUNC XrXirCallStatus xr_xir_task_executor_free(XrXirTaskExecutor *executor, XrXirCallBudget *accounting);
XR_FUNC bool xr_xir_task_storage_valid(const XirTask *task);
#endif // XXIR_TASK_INTERNAL_H
