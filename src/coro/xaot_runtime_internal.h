/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_runtime_internal.h - Private AOT runtime owner layout
 */

#ifndef XAOT_RUNTIME_INTERNAL_H
#define XAOT_RUNTIME_INTERNAL_H

#include "xaot_coro.h"
#include <stdatomic.h>

#include "../base/xglobal_indices.h"

struct XrAotRuntime {
    uint32_t caps;
    struct XrRuntimeCore *core;
    struct XrRuntime *scheduler;
    struct XrScopeContext *root_scope;
    struct XrCoroutine *root_coro;
    const XrAotValueOps *value_ops;
    XrValue coro_locals;
    /* Backing arena for the coro_locals maps. The maps live as long as the
     * runtime, so their storage must not come from whichever coroutine's
     * execution arena happens to be current at first touch — that arena is
     * bulk-freed when the coroutine's shell is recycled, leaving coro_locals
     * dangling. Created lazily under coro_locals_lock; destroyed with the
     * runtime. */
    void *coro_locals_arena;
    atomic_flag coro_locals_lock;
    XrValue builtins[XR_USER_GLOBALS_START];
};

#endif  // XAOT_RUNTIME_INTERNAL_H
