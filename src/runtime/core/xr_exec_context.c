/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "xr_exec_context.h"
#include "xr_runtime_core.h"
#include "../mem/xcoro_heap.h"
#include "../mem/xfixed_heap.h"
#include "../mem/xsystem_heap.h"
#include <string.h>

static _Thread_local XrExecutionContext *xr_tls_exec_context;

void xr_alloc_context_init(XrAllocationContext *ctx, XrRuntimeCore *core,
                           XrSemanticStorageDomain domain) {
    if (!ctx)
        return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->core = core;
    ctx->shared_heap = core ? core->sys_heap : NULL;
    ctx->domain = (uint8_t) domain;
}

void *xr_alloc_context_new_object(XrAllocationContext *ctx, size_t size, uint8_t type) {
    if (!ctx || !ctx->core || size < sizeof(XrObjHeader) || type >= XR_OBJ_TYPE_MAX)
        return NULL;
    if (ctx->domain == XR_STORAGE_TRANSFERABLE)
        return ctx->shared_heap ? xr_sysheap_alloc_transfer(ctx->shared_heap, size, type) : NULL;
    if (ctx->domain == XR_STORAGE_SYNC_SHARED || ctx->domain == XR_STORAGE_CONST_SHARED)
        return ctx->shared_heap ? xr_sysheap_alloc_shared(ctx->shared_heap, size, type) : NULL;
    /* Execution-local storage always has an exec heap — the root execution
     * included, since task 250 embeds one in the runtime core. Falling through
     * to the fixed heap here is what used to pin every top-level object
     * immortal: the domain said "dies with its execution", the allocator said
     * "lives until teardown", and nothing reported the contradiction. */
    if (ctx->local_heap)
        return xr_coro_heap_new_obj(ctx->local_heap, type, size);
    /* Module-static objects legitimately outlive every execution, so the fixed
     * heap is their home rather than a fallback.
     *
     * An EXEC_LOCAL context arriving here is a different matter: the domain
     * says "dies with its execution" while the allocator says "lives until
     * teardown", and nothing else reports that contradiction. Task 250 gives
     * the VM's root execution a real heap, so the VM no longer lands here.
     *
     * Neither does standalone AOT, despite what this comment claimed until
     * task 252 measured it: AOT does not route through this allocator at all.
     * It has its own object model (native structs, stack-allocated when they
     * do not escape, xrt_arc_alloc otherwise) and never sees a storage domain.
     * Its gap is a different one — no execution-scoped bulk release, so 247's
     * L2 bound does not hold there — and that is 252, not this fallback.
     *
     * The assert therefore guards a path with no known caller. Keep both: an
     * EXEC_LOCAL allocation reaching a heap that outlives the execution is a
     * contradiction worth failing on, whoever introduces it next. */
    XR_DCHECK(ctx->domain != XR_STORAGE_EXEC_LOCAL,
              "EXEC_LOCAL allocation context has no exec heap; object will be immortal");
    return xr_fixed_heap_alloc(&ctx->core->fixed_heap, size, type);
}

void xr_exec_context_init(XrExecutionContext *ctx, XrRuntimeCore *core,
                          XrAllocationContext *alloc) {
    if (!ctx)
        return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->core = core;
    ctx->alloc = alloc;
}

bool xr_exec_context_has_task(const XrExecutionContext *ctx) {
    return ctx && ctx->task != NULL;
}

XrExecutionContext *xr_exec_context_current(void) {
    return xr_tls_exec_context;
}

XrExecutionContext *xr_exec_context_enter(XrExecutionContext *ctx) {
    XrExecutionContext *previous = xr_tls_exec_context;
    xr_tls_exec_context = ctx;
    return previous;
}

void xr_exec_context_restore(XrExecutionContext *previous) {
    xr_tls_exec_context = previous;
}

XrAllocationContext *xr_alloc_context_current(void) {
    XrExecutionContext *ctx = xr_exec_context_current();
    return ctx ? ctx->alloc : NULL;
}

XrVMRuntime *xr_exec_context_vm_owner(void) {
    XrExecutionContext *ctx = xr_exec_context_current();
    return ctx && ctx->core ? xr_runtime_core_vm_owner(ctx->core) : NULL;
}

const char *xr_storage_domain_name(XrSemanticStorageDomain domain) {
    switch (domain) {
        case XR_STORAGE_DOMAIN_UNKNOWN:
            return "unknown";
        case XR_STORAGE_EXEC_LOCAL:
            return "exec_local";
        case XR_STORAGE_MODULE_STATIC:
            return "module_static";
        case XR_STORAGE_TRANSFERABLE:
            return "transferable";
        case XR_STORAGE_CONST_SHARED:
            return "const_shared";
        case XR_STORAGE_SYNC_SHARED:
            return "sync_shared";
        case XR_STORAGE_FOREIGN:
            return "foreign";
    }
    return "invalid";
}
