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
#include <stdbool.h>
#include <stdint.h>

struct XrCoroHeap;
struct XrCoroutine;
struct XrRuntimeCore;
struct XrSystemHeap;

typedef enum XrStorageOwner {
    XR_STORAGE_NONE = 0,
    XR_STORAGE_EXEC_LOCAL,
    XR_STORAGE_MODULE,
    XR_STORAGE_SHARED_SYSTEM,
    XR_STORAGE_FOREIGN,
} XrStorageOwner;

typedef enum XrStorageMutability {
    XR_STORAGE_READONLY = 0,
    XR_STORAGE_MUTABLE,
    XR_STORAGE_INTERIOR_MUTABLE,
} XrStorageMutability;

typedef enum XrAddressIdentity {
    XR_ADDRESS_NONE = 0,
    XR_ADDRESS_LEXICAL,
    XR_ADDRESS_MODULE_STABLE,
    XR_ADDRESS_SHARED_STABLE,
    XR_ADDRESS_FOREIGN,
} XrAddressIdentity;

typedef struct XrAllocationContext {
    struct XrRuntimeCore *core;
    struct XrCoroHeap *local_heap;
    struct XrSystemHeap *shared_heap;
    uint8_t owner; /* XrStorageOwner */
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
                                   XrStorageOwner owner);
XR_FUNC void xr_exec_context_init(XrExecutionContext *ctx, struct XrRuntimeCore *core,
                                  XrAllocationContext *alloc);
XR_FUNC bool xr_exec_context_has_task(const XrExecutionContext *ctx);
XR_FUNC const char *xr_storage_owner_name(XrStorageOwner owner);

#endif /* XR_EXEC_CONTEXT_H */
