/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xarena_backing.h - Explicit storage ownership for the shared arena algorithm
 */
#ifndef XARENA_BACKING_H
#define XARENA_BACKING_H

#include "xdefs.h"

typedef enum XrArenaStatus {
    XR_ARENA_OK,
    XR_ARENA_BAD_ARGUMENT,
    XR_ARENA_MEMORY_LIMIT,
    XR_ARENA_OVERFLOW,
    XR_ARENA_OUT_OF_MEMORY
} XrArenaStatus;

/* All operations are required. Allocation preserves an empty output on failure. Successful memory is aligned to at
 * least XR_ARENA_ALIGNMENT. Free and release cannot fail or allocate.
 * The operation table is copied; retain keeps its context alive independently.
 * Callers serialize an arena's operations and keep context alive until open. */
typedef struct XrArenaBacking {
    void *context;
    XrArenaStatus (*alloc)(void *context, size_t bytes, void **output);
    void (*free)(void *context, void *memory);
    XrArenaStatus (*retain)(void *context);
    void (*release)(void *context);
} XrArenaBacking;

/* Explicit process-lifetime system storage; no compiler ledger or limits. */
XR_FUNC XrArenaBacking xr_arena_system_backing(void);

#endif // XARENA_BACKING_H
