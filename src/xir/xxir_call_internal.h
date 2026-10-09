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
typedef struct XrXirCallBudget {
    uint64_t byte_limit, requested_limit, requested_bytes, live_bytes, peak_bytes, allocations, frees;
    uint64_t resume_limit, resumes;
    uint64_t transitions, release_tickets, released_frames;
    XrXirDomain *work_domain;
    bool exhausted;
} XrXirCallBudget;
typedef enum XrXirCellParameterRole {
    XR_XIR_CELL_ROLE_UNKNOWN, XR_XIR_CELL_ROLE_OWNED_CAPTURE,
    XR_XIR_CELL_ROLE_SCOPED_REF, XR_XIR_CELL_ROLE_LEXICAL_CLEANUP
} XrXirCellParameterRole;
/* The resolver consumes complete checked provenance, never parameter shape alone. */
typedef XrXirCellParameterRole (*XrXirCellRoleResolver)(void *context,
    uint32_t entry, uint32_t parameter);
typedef struct XrXirCallRequest {
    uint32_t entry;
    const XrXirValue *arguments;
    uint32_t count;
    XrXirCellRoleResolver cell_role;
    void *cell_context;
} XrXirCallRequest;
/* Module access still requires the instance's root execution admission. */
XR_FUNC XrXirValueStatus xr_xir_call_cell_read(const XrXirCallView *view,
    const XrXirValue *cell, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_call_cell_write(const XrXirCallView *view,
    const XrXirValue *cell, const XrXirValue *value);
XR_FUNC XrXirValueStatus xr_xir_call_cell_place(const XrXirCallView *view,
    const XrXirValue *cell, XrXirValuePlace *output);
XR_FUNC void *xr_xir_call_budget_allocate(XrXirCallBudget *budget, uint64_t bytes, XrXirCallStatus *status);
XR_FUNC void xr_xir_call_budget_deallocate(XrXirCallBudget *budget, void *memory, uint64_t bytes);
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
/* Observe the stable first driver failure for this exact executor activation.
 * Rejected bindings leave output untouched and never grant entry authority. */
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
#endif // XXIR_CALL_INTERNAL_H
