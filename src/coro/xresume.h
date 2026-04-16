/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xresume.h - Coroutine resume and unroll mechanism
 *
 * KEY CONCEPT:
 *   Resume suspended coroutines by processing continuation functions
 *   on the call stack layer by layer (unroll mechanism).
 */

#ifndef XRESUME_H
#define XRESUME_H

#include "xcoroutine.h"
#include "../runtime/xvm_call.h"

// Resume coroutine by processing continuation functions layer by layer
// Returns: VM_OK (continue), VM_BLOCKED (block again), VM_ERROR (error)
XR_FUNC XrVMResult xr_coro_resume_with_unroll(struct XrayIsolate *X, XrCoroutine *coro, int resume_status);

// Set coroutine's resume status (trivial wrapper, inlined for zero overhead)
static inline void xr_coro_set_resume_status(XrCoroutine *coro, int status) {
    if (coro) xr_coro_resume_store(coro, status);
}

// Get coroutine's resume status
static inline int xr_coro_get_resume_status(XrCoroutine *coro) {
    return coro ? xr_coro_resume_load(coro) : XR_RESUME_ERROR;
}

#endif // XRESUME_H
