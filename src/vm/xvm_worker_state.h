/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_worker_state.h - VM private worker scratch state
 *
 * KEY CONCEPT:
 *   Scheduler machines keep backend storage opaque. The VM backend owns the
 *   thread-local scratch context used by direct VM calls and C continuation
 *   recovery while coroutine resume frames stay in per-coroutine VM state.
 */

#ifndef XVM_WORKER_STATE_H
#define XVM_WORKER_STATE_H

#include "../base/xforward_decl.h"
#include "../runtime/xexec_frame.h"

struct XrMachine;

XR_FUNC XrVMContext *xr_vm_machine_ctx(struct XrMachine *machine, XrayIsolate *isolate);
XR_FUNC void xr_vm_machine_ctx_set_isolate(struct XrMachine *machine, XrayIsolate *isolate);

#endif  // XVM_WORKER_STATE_H
