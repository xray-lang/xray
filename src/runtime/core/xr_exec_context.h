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
struct XrVMRuntime;

typedef struct XrAllocationContext {
    struct XrRuntimeCore *core;
    struct XrCoroHeap *local_heap;
    struct XrSystemHeap *shared_heap;
    uint8_t domain; /* XrSemanticStorageDomain */
} XrAllocationContext;

typedef struct XrExecutionContext {
    struct XrRuntimeCore *core;
    XrAllocationContext *alloc;
    void *errors;
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
