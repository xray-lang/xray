/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_stack.h - Entry capacity and coroutine storage growth
 */
#ifndef XVM_STACK_H
#define XVM_STACK_H
#include "../runtime/xexec_frame.h"
struct XrCoroutine;
XR_FUNC bool xr_coro_grow_stack(struct XrCoroutine *coro, int extra_slots);
XR_FUNC bool xr_vm_prepare_entry(XrVMContext *ctx, int extra_stack);
#endif // XVM_STACK_H
