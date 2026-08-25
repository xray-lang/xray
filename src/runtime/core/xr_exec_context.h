/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_exec_context.h - Execution identity independent from task identity.
 */

#ifndef XR_EXEC_CONTEXT_H
#define XR_EXEC_CONTEXT_H

#include "../../base/xdefs.h"
#include "../../base/xstorage.h"
#include <stdbool.h>
#include <stdint.h>

struct XrCoroHeap;
struct XrCoroutine;
struct XrRuntimeCore;
struct XrSystemHeap;
struct XrValue;
struct XrVMRuntime;

typedef struct XrAllocationContext {
    struct XrRuntimeCore *core;
    struct XrCoroHeap *local_heap;
    struct XrSystemHeap *shared_heap;
    uint8_t domain; /* XrSemanticStorageDomain */
} XrAllocationContext;

/* The execution context borrows this slot from its active executor.  The
 * executor owns the slot lifetime; the channel owns only the publication
 * rule.  Binding is exact and cannot be replaced while the context is live. */
typedef struct XrExecutionErrorChannel {
    struct XrValue *pending;
} XrExecutionErrorChannel;

typedef enum XrExecutionErrorPublishStatus {
    XR_EXEC_ERROR_PUBLISH_OK = 0,
    XR_EXEC_ERROR_PUBLISH_INVALID_ARGUMENT,
    XR_EXEC_ERROR_PUBLISH_NO_ACTIVE_CONTEXT,
    XR_EXEC_ERROR_PUBLISH_WRONG_RUNTIME,
    XR_EXEC_ERROR_PUBLISH_CHANNEL_UNBOUND,
    XR_EXEC_ERROR_PUBLISH_CHANNEL_OCCUPIED,
} XrExecutionErrorPublishStatus;

typedef struct XrExecutionContext {
    struct XrRuntimeCore *core;
    XrAllocationContext *alloc;
    XrExecutionErrorChannel error_channel;
    void *executor;
    struct XrCoroutine *task; /* nullable: allocation never implies task identity */
    uint64_t logical_root_id;
} XrExecutionContext;

XR_FUNC void xr_alloc_context_init(XrAllocationContext *ctx, struct XrRuntimeCore *core,
                                   XrSemanticStorageDomain domain);
/* Allocate a runtime object from an explicit lifetime context. Execution-local
 * contexts use their coroutine heap when present; module contexts use fixed
 * storage; transferable/const-shared/sync-shared contexts use the system heap
 * with their canonical storage tag established at allocation time. */
XR_FUNC void *xr_alloc_context_new_object(XrAllocationContext *ctx, size_t size, uint8_t type);
XR_FUNC void xr_exec_context_init(XrExecutionContext *ctx, struct XrRuntimeCore *core,
                                  XrAllocationContext *alloc);
/* Bind/unbind the executor-owned pending-error slot.  Repeating the same bind
 * is idempotent; rebinding to a different slot and unbinding a non-matching
 * slot fail closed.  Neither operation is permitted while ctx is active. */
XR_FUNC bool xr_exec_context_bind_error_channel(XrExecutionContext *ctx,
                                                struct XrValue *pending);
XR_FUNC bool xr_exec_context_unbind_error_channel(XrExecutionContext *ctx,
                                                  struct XrValue *pending);
/* Publish into the channel of the exact TLS-active execution context.
 * expected_core prevents a caller from crossing runtime identities.  On OK,
 * ownership moves from *owned_error into the pending slot and *owned_error is
 * cleared.  On every rejection the source and destination remain unchanged. */
XR_FUNC XrExecutionErrorPublishStatus
xr_exec_context_publish_error_owned(struct XrRuntimeCore *expected_core,
                                    struct XrValue *owned_error);
XR_FUNC bool xr_exec_context_has_task(const XrExecutionContext *ctx);
/* Direct logical roots install an execution context for the duration of VM
 * dispatch.  A physical coroutine is optional; allocation identity is not. */
XR_FUNC XrExecutionContext *xr_exec_context_current(void);
XR_FUNC XrExecutionContext *xr_exec_context_enter(XrExecutionContext *ctx);
XR_FUNC void xr_exec_context_restore(XrExecutionContext *previous);
XR_FUNC XrAllocationContext *xr_alloc_context_current(void);
XR_FUNC struct XrVMRuntime *xr_exec_context_vm_owner(void);
XR_FUNC const char *xr_storage_domain_name(XrSemanticStorageDomain domain);

#endif /* XR_EXEC_CONTEXT_H */
