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
#include "xxir_root_callback_internal.h"
typedef struct XrXirTaskExecutor XrXirTaskExecutor;
/* Only physical close retries are pending here; this never drives guest code. */
XR_FUNC bool xir_task_executor_retirement_pending(const XrXirTaskExecutor *executor);
typedef struct XirTaskGroup XirTaskGroup;
typedef struct XirTaskSubscription XirTaskSubscription;
typedef enum XirTaskState {
    XIR_TASK_PREPARING, XIR_TASK_PENDING, XIR_TASK_RUNNING, XIR_TASK_TERMINAL
} XirTaskState;
typedef struct XirTaskActivation {
    XrXirCall *call;
    struct XirTask *task;
    struct XirTaskActivation *ready_next;
    uint64_t ticket;
    uint32_t entry;
    bool queued;
    uint64_t host_token, host_call_wake, host_start_ns, host_last_ns, host_duration_ns;
} XirTaskActivation;
typedef struct XirTask {
    XirObject object;
    XrXirCallResult outcome;
    XrXirCallAccounting accounting;
    XrXirCall *call;
    XrXirTaskExecutor *executor;
    struct XirTask *active_next, *active_previous;
    XrXirCall *waiter_head, *waiter_tail;
    XirTaskGroup *aggregation;
    uint64_t cancel_scope;
    XirTaskSubscription *subscriptions;
    XirTaskActivation activation;
    uint64_t generation, identity, terminal_order;
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
XR_FUNC XrXirCallStatus xir_task_scope_begin(XrXirTaskExecutor *executor,
    XrXirCallView *view,XrXirType type,uint32_t mode,XrXirValue *output);
typedef enum XirTaskAggregateKind { XIR_TASK_AGGREGATE_ALL=1,XIR_TASK_AGGREGATE_FIRST=2 } XirTaskAggregateKind;
/* Begin owns real subscriptions; the ordinary Task await performs the park. */
XR_FUNC XrXirCallStatus xir_task_aggregate_begin(XrXirTaskExecutor *executor,
    XrXirCallView *view,const XrXirValue *tasks,XrXirType result,XirTaskAggregateKind kind,XrXirValue *output);
XR_FUNC XrXirCallStatus xir_task_spawn_scoped(XrXirTaskExecutor *executor,
    XrXirCallView *view,const XrXirValue *scope,XrXirType type,
    const XrXirCallRequest *request,XrXirValue *output);
XR_FUNC XrXirCallStatus xir_task_scope_seal(XrXirTaskExecutor *executor,
    XrXirCallView *view,const XrXirValue *scope,bool abandon);
/* Metadata identity of the exact active frame; it lends no state or permission. */
XR_FUNC XrXirCallStatus xir_call_scope_frame(const XrXirCallView *view,uint64_t *epoch,uint32_t *frontier);
/* A real cleanup frontier drains its own lexical group before its helper runs. */
XR_FUNC XrXirCallStatus xir_task_scope_cleanup(XrXirTaskExecutor *executor,
    XrXirCallView *view,XrXirValue *output);
XR_FUNC XrXirCallStatus xir_task_scope_observe(const XrXirValue *task,
    XrXirCall *call,uint64_t frame,uint64_t *failure_order);
/* A failure-induced cancellation may stop only at its original lexical owner. */
XR_FUNC bool xir_task_scope_cancel_boundary(const XirTaskGroup *group,
    const XrXirCall *call,uint64_t frame,uint32_t frontier);
XR_FUNC XrXirCallStatus xir_call_request_scope_cancel(XrXirCall *call,XirTaskGroup *group);
XR_FUNC XirTaskGroup *xir_call_cancel_origin(const XrXirCall *call);
XR_FUNC uint64_t xir_task_scope_identity(const XirTaskGroup *group);
XR_FUNC XrXirCallStatus xir_task_cancel_origin(XrXirTaskExecutor *executor,
    const XirTask *task,XrXirValueAdmission *admission,XirTaskGroup **output);
/* Driver publication order is separate from Task identity and source order. */
XR_FUNC XrXirCallStatus xir_task_scope_failure_order(XrXirTaskExecutor *executor,uint64_t *output);
XR_FUNC XrXirCallStatus xr_xir_task_executor_poll(XrXirTaskExecutor *executor, uint64_t quantum);
/* The instance owns its root Call; the executor only schedules the activation. */
XR_FUNC bool xr_xir_task_executor_root_idle(const XrXirTaskExecutor *executor);
/* A finished root may still carry a fault from child completion or cleanup. */
XR_FUNC XrXirCallStatus xr_xir_task_executor_completion_status(const XrXirTaskExecutor *executor);
XR_FUNC XrXirCallStatus xr_xir_task_executor_attach_root(XrXirTaskExecutor *executor, XrXirCall *call, uint64_t epoch);
XR_FUNC XrXirCallResult xr_xir_task_executor_poll_root(XrXirTaskExecutor *executor, uint64_t quantum);
/* One authenticated VM callback, with no other activation dispatched. A ready
 * activation retains its exact FIFO position. Driver failures stay recorded. */
XR_FUNC XrXirCallStatus xir_task_executor_visit_vm(XrXirTaskExecutor *,const XrXirExecutorBinding *,uint64_t,bool *);
XR_FUNC XrXirCallStatus xr_xir_task_executor_resume_root(XrXirTaskExecutor *executor, uint64_t epoch, uint64_t wake);
XR_FUNC XrXirCallStatus xr_xir_task_executor_root_wait(const XrXirTaskExecutor *executor, uint64_t epoch, uint64_t wake,
    XrXirWaitRequest *output);
XR_FUNC bool xr_xir_task_executor_view_member(const XrXirTaskExecutor *executor,
    const XrXirCallView *view, bool *child);
/* The actual parked activation must still belong to the original owner lists. */
XR_FUNC XrXirCallStatus xr_xir_task_executor_pending_member(const XrXirTaskExecutor *executor,
    XrXirCall *call,const XrXirTaskWaitToken *token,XrXirValueAdmission *admission);
XR_FUNC bool xr_xir_task_executor_cleanup_active(const XrXirTaskExecutor *executor);
XR_FUNC XrXirCallStatus xr_xir_task_executor_cancel_task(XrXirTaskExecutor *executor,
    XrXirCallView *view,const XrXirValue *task);
XR_FUNC XrXirCallStatus xr_xir_task_executor_cancel_root(XrXirTaskExecutor *executor);
XR_FUNC XrXirCallStatus xr_xir_task_executor_stop(XrXirTaskExecutor *executor);
/* Stops and drains the whole activation group with no control allocation.
 * Language cleanup keeps its ordinary budget. The stopped executor remains
 * owned so its actual terminal root can still be observed. */
XR_FUNC XrXirCallStatus xr_xir_task_executor_drain(XrXirTaskExecutor *executor, XrXirCallStatus hard_failure);
XR_FUNC XrXirCallStatus xr_xir_task_executor_free(XrXirTaskExecutor *executor, XrXirCallBudget *accounting);
XR_FUNC bool xr_xir_task_storage_valid(const XirTask *task);
/* A deferred Root callback preparation fault stops the group through its real
 * cleanup path, never through Parent's await/catch inbox. */
XR_FUNC XrXirCallStatus xir_task_callback_failure(XrXirTaskExecutor *executor,XrXirCallStatus status);
XR_FUNC XrXirCallStatus xir_task_callback_attach(XrXirTaskExecutor *executor,XrXirCall *call);
XR_FUNC bool xir_task_callback_active(const XrXirTaskExecutor *executor);
XR_FUNC XrXirCallStatus xir_task_host_wait_deadline(XrXirTaskExecutor *executor,
    uint64_t epoch,uint64_t wake,uint64_t now_ns,XirHostWaitDeadline *output);
#endif // XXIR_TASK_INTERNAL_H
