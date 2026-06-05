/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_resume.h - VM coroutine continuation unroll
 */

#ifndef XVM_RESUME_H
#define XVM_RESUME_H

#include "../coro/xcoroutine.h"
#include "../runtime/xvm_call.h"

XR_FUNC XrVMResult xr_vm_coro_resume_with_unroll(struct XrayIsolate *X, XrCoroutine *coro,
                                                 int resume_status);

#endif  // XVM_RESUME_H
