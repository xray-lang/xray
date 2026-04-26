/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm.h - Register-based virtual machine
 *
 * KEY CONCEPT:
 *   Executes bytecode with register-based instruction set.
 *   Supports coroutine suspension/resumption and multi-core scheduling.
 *
 * VM INVARIANTS:
 *
 *   INVARIANT 1 (Register window): Each call frame owns a contiguous
 *   register window R[0..maxstacksize-1] on the value stack. The window
 *   starts at stack[base_offset]. Adjacent frames must not overlap:
 *   frame[n+1].base_offset >= frame[n].base_offset + frame[n].maxstacksize.
 *
 *   INVARIANT 2 (Stack bounds): stack_top always points to the next
 *   free slot. For the current frame, stack_top = base + maxstacksize.
 *   No instruction may read or write beyond stack_top. The stack is
 *   bounds-checked at frame creation (CHECK_FRAME_OVERFLOW).
 *
 *   INVARIANT 3 (Calling convention):
 *     Function call: caller R[func]=closure, R[func+1..]=args.
 *       Callee base = caller_base + func + 1. R[0]=arg1, R[1]=arg2...
 *       Return value written to stack[base_offset - 1] = caller R[func].
 *     Method call: caller R[a]=return_slot, R[a+1]=this, R[a+2..]=args.
 *       Callee base = caller_base + a + 1. R[0]=this, R[1..]=args.
 *       Return value written to caller R[a].
 *
 *   INVARIANT 4 (Exception handler stack): Exception handlers form
 *   a LIFO stack. Each try block pushes a handler; leaving the block
 *   pops it. On throw, the VM searches handlers top-down for the
 *   innermost matching try. If none found, the frame is unwound
 *   (close upvalues, pop frame) and the search continues in the caller.
 *
 *   INVARIANT 6 (Preemptive yield): The VM decrements reductions on
 *   backward jumps and function calls. When reductions <= 0, the
 *   coroutine yields to the scheduler at the next safe point. This
 *   guarantees no coroutine can starve others indefinitely.
 *
 * EXECUTION FLOW:
 *
 *   run(isolate, vm_ctx)
 *     │
 *     ├─► Fetch instruction (computed goto dispatch)
 *     ├─► Decode A/B/C or A/Bx operands
 *     ├─► Execute opcode handler
 *     │     ├─► Arithmetic: operate on registers, write result
 *     │     ├─► Call: push frame, transfer control
 *     │     ├─► Return: pop frame, write result to caller
 *     │     ├─► Jump: modify PC (backward jump checks reductions)
 *     │     └─► Yield: save state, return VM_YIELD
 *     └─► Loop back to fetch
 *
 * RELATED MODULES:
 *   - vm_state.h: VM state types (XrVMState, XrVMContext)
 *   - xvm_internal.h: Internal VM structures and helpers
 */

#ifndef XVM_H
#define XVM_H

#include "../runtime/value/xchunk.h"
#include "../runtime/value/xvalue.h"
#include "../base/xhashmap.h"
#include "../runtime/xexec_frame.h"
#include "../base/xconstants.h"
#include "../runtime/xvm_call.h"
#include <stdbool.h>

/* ========== VM API ========== */

XR_FUNC void xr_vm_vm_init(XrayIsolate *isolate);
XR_FUNC void xr_vm_vm_free(XrayIsolate *isolate);

XR_FUNC XrVMResult xr_vm_interpret(const char *source);
XR_FUNC XrVMResult xr_vm_interpret_proto(XrayIsolate *isolate, XrProto *proto);
XR_FUNC XrVMResult xr_vm_execute_module(XrayIsolate *isolate, XrProto *proto);

struct XrayIsolate;
XR_FUNC XrVMResult xr_vm_interpret_proto_isolate(struct XrayIsolate *isolate, XrProto *proto);

/* ========== C Function API ========== */

XR_FUNC XrCFunction *xr_vm_cfunction_new(XrayIsolate *isolate, XrCFunctionPtr func, const char *name);
XR_FUNC XrCFunction *xr_vm_yieldable_cfunction_new(XrayIsolate *isolate, XrYieldableCFunctionPtr func, const char *name);
XR_FUNC void xr_vm_cfunction_free(XrCFunction *cfunc);

/* ========== Closure API ========== */
/* Closure creation is declared in runtime/closure/xclosure.h. */

/* ========== Runtime Error ========== */

XR_FUNC void xr_runtime_error(XrayIsolate *isolate, const char *format, ...);

/* ========== Exception Handling API ========== */

XR_FUNC void xr_vm_throw_exception(XrayIsolate *isolate, XrValue exception);
XR_FUNC void xr_vm_add_stacktrace(XrayIsolate *isolate, XrValue exception);
/* Unified throw: records the full stack trace and performs the
 * unwind in one call. See xvm_exception.c for rationale. */
XR_FUNC void xr_vm_unwind_with_trace(XrayIsolate *isolate, XrValue exception);

/* ========== Helper Functions ========== */

XR_FUNC bool xr_vm_is_truthy(XrValue value);

/* ========== C11 Compile-time Checks ========== */

#if __STDC_VERSION__ >= 201112L
#include <assert.h>

// 16384 = 2^14: power-of-2 for alignment, ~1MB memory budget at 64 bytes/frame
static_assert(XR_FRAMES_MAX > 0 && XR_FRAMES_MAX <= 16384,
    "XR_FRAMES_MAX must be between 1 and 16384");
static_assert(XR_STACK_MAX >= XR_FRAMES_MAX,
    "XR_STACK_MAX must be at least XR_FRAMES_MAX");
static_assert(XR_STACK_MAX <= 1024 * 1024,
    "XR_STACK_MAX too large (max 1M)");
static_assert(XR_EXCEPTION_HANDLERS_MAX > 0 && XR_EXCEPTION_HANDLERS_MAX <= 256,
    "XR_EXCEPTION_HANDLERS_MAX must be between 1 and 256");

// 128 bytes = 2 cache lines, ensures good cache locality for hot path
static_assert(sizeof(XrBcCallFrame) <= 128,
    "XrBcCallFrame should fit in two cache lines");

#endif

#endif // XVM_H
