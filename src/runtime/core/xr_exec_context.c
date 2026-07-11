/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "xr_exec_context.h"
#include "xr_runtime_core.h"
#include <string.h>

void xr_alloc_context_init(XrAllocationContext *ctx, XrRuntimeCore *core, XrStorageOwner owner) {
    if (!ctx)
        return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->core = core;
    ctx->shared_heap = core ? core->sys_heap : NULL;
    ctx->owner = (uint8_t) owner;
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

const char *xr_storage_owner_name(XrStorageOwner owner) {
    switch (owner) {
        case XR_STORAGE_NONE:
            return "none";
        case XR_STORAGE_EXEC_LOCAL:
            return "exec_local";
        case XR_STORAGE_MODULE:
            return "module";
        case XR_STORAGE_SHARED_SYSTEM:
            return "shared_system";
        case XR_STORAGE_FOREIGN:
            return "foreign";
    }
    return "invalid";
}
