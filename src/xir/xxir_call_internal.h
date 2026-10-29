/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_internal.h - Shared executor storage and transition accounting
 *
 * KEY CONCEPT:
 *   Every activation borrows one stable executor budget until physical release.
 */
#ifndef XXIR_CALL_INTERNAL_H
#define XXIR_CALL_INTERNAL_H
#include "xxir_call.h"
#include "xxir_cell_owner_internal.h"
#include "xxir_effect_contract_internal.h"
#include "xxir_channel_rendezvous_internal.h"
static inline bool xir_call_empty_destination(const XrXirValue *value) {
    return value && value->type == XR_XIR_UNIT && !value->reserved && !value->payload;
}
typedef struct XrXirCallObserver XrXirCallObserver;
typedef struct XirCallNativeObserver XirCallNativeObserver;
typedef struct XirCallNativeSourceObserver XirCallNativeSourceObserver;
typedef struct XrXirCallBudget {
    uint64_t byte_limit, requested_limit, requested_bytes, live_bytes, peak_bytes, allocations, frees;
    uint64_t resume_limit, resumes;
    uint64_t transitions, release_tickets, released_frames;
    XrXirDomain *work_domain;
    bool exhausted;
    XrXirCall *observed_calls;
    const XrXirCallObserver *observer;
    const void *observer_instance;
    bool notifying;
    const XirCallNativeObserver *native_observer;
    const void *native_instance;
    XrXirCall *native_hold;
    const XirCallNativeSourceObserver *native_source;
    const void *native_source_instance;
} XrXirCallBudget;
typedef enum XrXirCellParameterRole {
    XR_XIR_CELL_ROLE_UNKNOWN, XR_XIR_CELL_ROLE_OWNED_CAPTURE,
    XR_XIR_CELL_ROLE_SCOPED_REF, XR_XIR_CELL_ROLE_LEXICAL_CLEANUP
} XrXirCellParameterRole;
/* The resolver consumes complete checked provenance, never parameter shape alone. */
typedef XrXirCellParameterRole (*XrXirCellRoleResolver)(void *context,
    uint32_t entry, uint32_t parameter);
/* Metadata is borrowed from a sealed live Instance generation. The Call keeps
 * one admission ledger, executor binding and frame stack across all tables. */
typedef struct XrXirCallMetadata {
    const XrXirCallEntry *entries;
    uint32_t entry_count;
    const XrXirTypeArena *arena;
    void *admission_context;
    XrXirCellRoleResolver cell_role;
    void *cell_context;
} XrXirCallMetadata;

/* These coordinates are read and consumed inside one current callback, never
 * retained as authority after the frame or its cleanup registration retires. */
typedef struct XirCallCleanupOrigin {uint32_t owner,helper,frontier;} XirCallCleanupOrigin;
XR_FUNC XrXirCallStatus xir_call_cleanup_origin(const XrXirCallView *view,uint32_t target,
    XrXirCellRoleResolver role,void *context,XirCallCleanupOrigin *output);

typedef struct XrXirCallRequest {
    uint32_t entry;
    const XrXirValue *arguments;
    uint32_t count;
    XrXirCellRoleResolver cell_role;
    void *cell_context;
} XrXirCallRequest;
/* A pending instruction is not a permission. Instance admission rechecks
 * actual values before the driver transfers a sealed selection to a frame. */
XR_FUNC const XirEffectInvocationSelection *xr_xir_call_invocation(const XrXirCallView *view);
typedef struct XrXirCallFrontier {
    uint32_t instruction,target,producer;
} XrXirCallFrontier;
/* A staged frontier retains only identity. Actual inputs still require a fresh
 * Final selection before acceptance and another check after loan preparation. */
XR_FUNC bool xr_xir_call_invocation_pending(const XrXirCallView *view,XrXirCallFrontier *frontier);
XR_FUNC XrXirCallStatus xr_xir_call_invocation_stage(XrXirCallView *view,
    const XirEffectInvocationSelection *selection);
XR_FUNC XrXirCallStatus xr_xir_call_invocation_accept(XrXirCallView *view,
    const XirEffectInvocationSelection *selection);
/* This view borrows the existing root frame only during a synchronous read. */
typedef struct XirGeneratorCaptureView {
    const XrXirValue *values;uint32_t count,entry;
} XirGeneratorCaptureView;
XR_FUNC XrXirCallStatus xr_xir_call_generator_captures(const XrXirCall *call,
    XrXirValueAdmission *admission,XirGeneratorCaptureView *output);
typedef struct XrXirCallClassAccess {
    const XrXirValue *value;
    XrXirCellAuthority authority;
    XrXirCellLoan loan;
} XrXirCallClassAccess;
XR_FUNC XrXirValueStatus xr_xir_call_class_access_prepare(const XrXirCallView *view,
    const XrXirValue *value,XrXirCallClassAccess *output);
XR_FUNC void xr_xir_call_class_access_commit(const XrXirCallView *view,XrXirCallClassAccess *access);
XR_FUNC void xr_xir_call_class_access_finish(XrXirCallClassAccess *access);
/* Module access still requires the instance's root execution admission. */
XR_FUNC XrXirValueStatus xr_xir_call_cell_read(const XrXirCallView *view,
    const XrXirValue *cell, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_call_cell_write(const XrXirCallView *view,
    const XrXirValue *cell, const XrXirValue *value);
XR_FUNC XrXirValueStatus xr_xir_call_cell_place(const XrXirCallView *view,
    const XrXirValue *cell, XrXirValuePlace *output);
/* Scoped descriptors own evaluated selectors, never a borrowed payload address. */
XR_FUNC XrXirValueStatus xr_xir_call_cell_project(const XrXirCallView *view,
    XrXirType type, const XrXirValue *root, const XrXirValuePath *path, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_call_cell_path_read(const XrXirCallView *view,
    const XrXirValue *cell, const XrXirValuePath *path,
    XrXirValue *output, XrXirFaultDetail *fault);
XR_FUNC XrXirValueStatus xr_xir_call_cell_path_write(const XrXirCallView *view,
    const XrXirValue *cell, const XrXirValuePath *path,
    const XrXirValue *value, XrXirFaultDetail *fault, bool append);
XR_FUNC void *xr_xir_call_budget_allocate(XrXirCallBudget *budget, uint64_t bytes, XrXirCallStatus *status);
XR_FUNC void xr_xir_call_budget_deallocate(XrXirCallBudget *budget, void *memory, uint64_t bytes);
XR_FUNC XrXirCallStatus xr_xir_call_generator_new_budgeted(const XrXirCallConfig *config,
    const XrXirCallRequest *request, XrXirCallBudget *budget, XrXirCall **output);
XR_FUNC XrXirCallStatus xr_xir_call_new_budgeted(const XrXirCallConfig *config,
    const XrXirCallRequest *request, XrXirCallBudget *budget, XrXirCall **output);
/* A registered language cleanup was physically released before retirement. */
XR_FUNC bool xr_xir_call_cleanup_incomplete(const XrXirCall *call);
/* Terminal transfer moves both value and panic, without allocation or retain. */
XR_FUNC XrXirCallStatus xr_xir_call_take_outcome(XrXirCall *call, XrXirCallResult *output);
/* A binding is installed once, before the activation's first transition. */
typedef struct XrXirExecutorBinding {
    const void *owner;
    void *activation;
    uint64_t generation, ticket;
} XrXirExecutorBinding;
typedef struct XrXirTaskWaitToken {
    XrXirExecutorBinding binding;
    uint64_t wake;
    XrXirWaitRequest request;
} XrXirTaskWaitToken;
XR_FUNC XrXirCallStatus xr_xir_call_bind_executor(XrXirCall *call, const XrXirExecutorBinding *binding);
XR_FUNC bool xr_xir_call_executor_member(const XrXirCall *call, const XrXirExecutorBinding *binding);
/* Pure observation of an authenticated root's current external yield. No
 * polling, clock reads, channel completion, allocation or callbacks occur. */
XR_FUNC bool xir_call_bound_yield_pending(const XrXirCall *call,const XrXirExecutorBinding *binding);
/* Observe the stable first driver failure for this exact executor activation.
 * Rejected bindings leave output untouched and never grant entry authority. */
/* Physical release only; never resumes guest cleanup in a failed group. */
XR_FUNC XrXirCallStatus xir_call_close_bound(XrXirCall *call, const XrXirExecutorBinding *binding,
    XrXirCallResult *terminal);
XR_FUNC bool xr_xir_call_driver_failure(const XrXirCall *call, const XrXirExecutorBinding *binding,
    XrXirCallStatus *output);
XR_FUNC bool xr_xir_call_task_wait_token(const XrXirCall *call, XrXirTaskWaitToken *output);
/* Intrusive linking uses the owner's already allocated frame and Task lease. */
XR_FUNC XrXirCallStatus xr_xir_call_link_task_wait(XrXirCall *call, const XrXirTaskWaitToken *token);
/* Moves a fully prepared terminal copy into the exact waiting frame. */
XR_FUNC XrXirCallStatus xr_xir_call_complete_task_wait(XrXirCall *call, const XrXirTaskWaitToken *token,
    XrXirCallResult *outcome);
/* A failed copy terminates only the waiting activation, never the sticky Task. */
XR_FUNC XrXirCallStatus xr_xir_call_fail_task_wait(XrXirCall *call, const XrXirTaskWaitToken *token,
    XrXirCallStatus reason);
/* Only the active callback may stage a watch. SUSPENDED requires returning an
 * empty AWAIT_CHANNEL action; READY observed a changed frontier, BUSY a held lock.
 * No view, frame, or admission callback is borrowed by the notification node. */
XR_FUNC XrXirCallStatus xr_xir_call_watch_channel(XrXirCallView *view,
    const XrXirValue *channel, uint64_t sequence);
/* Allocation-free after payload publication. SUSPENDED moves the unique owner
 * into the Call and requires an empty AWAIT_CHANNEL action. Completion delivers
 * an I64 SendWaitState inbox. READY leaves an already completed owner untouched. */
XR_FUNC XrXirCallStatus xr_xir_call_wait_channel_send(XrXirCallView *view,XrXirChannelSendWait **owner);
/* SUSPENDED moves the unique receive owner with no allocation. Completion
 * moves its actual native Recv<T> into the current frame's owned inbox.
 * READY preserves an already completed owner for immediate local take. */
XR_FUNC XrXirCallStatus xr_xir_call_wait_channel_recv(XrXirCallView *view,XrXirChannelRecvWait **owner);
/* Move one actual returned inbox owner into canonical Unit storage. The exact
 * active view and expected closed type must match; failure preserves both.
 * Moving an acknowledged Channel result allocates nothing and retains nothing. */
XR_FUNC XrXirCallStatus xr_xir_call_take_inbox(XrXirCallView *view,XrXirType expected,XrXirValue *output);
XR_FUNC bool xr_xir_call_channel_waiting(const XrXirCall *call);
XR_FUNC bool xr_xir_call_channel_ready(const XrXirCall *call);
/* Executor accounting failure removes only its exact activation's owned watch. */
XR_FUNC XrXirCallStatus xr_xir_call_fail_channel_wait(XrXirCall *call,
    const XrXirExecutorBinding *binding, XrXirCallStatus reason);
/* Iterator waits reuse the exact binding/wake/request token representation. */
/* Borrowed facts belong to one exact pending wait, never an execution grant. */
struct XrXirInstance;
typedef struct XirIteratorWaitContext {
    struct XrXirInstance *instance;XrXirValueAdmission *admission;
    const XrXirValue *iterator;const XirEffectInvocationSelection *invocation;
    uint32_t caller,instruction;
} XirIteratorWaitContext;
/* Staging a parked Iterator instruction grants no ordinary call frontier. */
XR_FUNC XrXirCallStatus xr_xir_call_iterator_stage(XrXirCallView *view,uint32_t instruction);
XR_FUNC XrXirCallStatus xr_xir_call_iterator_context(XrXirCall *call,
    const XrXirTaskWaitToken *token,XirIteratorWaitContext *output);
/* Rechecks the real demand before binding the parked producer root context. */
XR_FUNC XrXirCallStatus xr_xir_call_iterator_prepare(XrXirCall *call,const XrXirTaskWaitToken *token);
XR_FUNC bool xr_xir_call_iterator_wait_token(const XrXirCall *call,XrXirTaskWaitToken *output);
XR_FUNC XrXirCallStatus xr_xir_call_link_iterator_wait(XrXirCall *call,const XrXirTaskWaitToken *token);
XR_FUNC XrXirCallStatus xr_xir_call_complete_iterator_wait(XrXirCall *call,const XrXirTaskWaitToken *token);
XR_FUNC XrXirCallStatus xr_xir_call_fail_iterator_wait(XrXirCall *call,const XrXirTaskWaitToken *token,
    XrXirCallStatus reason);
#endif // XXIR_CALL_INTERNAL_H
