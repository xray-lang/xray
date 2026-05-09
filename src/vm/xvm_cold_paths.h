/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_cold_paths.h - Shared definitions and declarations for VM cold paths
 *
 * KEY CONCEPT:
 *   Cold path functions handle infrequent VM operations (invoke dispatch,
 *   property access by type, coroutine ops). Extracted from xvm.c to reduce
 *   file size while keeping the hot dispatch loop in one place.
 *
 * WHY THIS DESIGN:
 *   All cold path functions are XR_NOINLINE, so extracting
 *   them to a separate compilation unit has zero performance impact.
 */

#ifndef XVM_COLD_PATHS_H
#define XVM_COLD_PATHS_H

#include "xvm_internal.h"

#include "../coro/xchannel.h"
#include "../coro/xdeep_copy.h"
#include "../coro/xworker.h"
#include "../coro/xyieldable.h"
#include "../coro/xcoro_registry.h"
#include "../module/xmodule.h"
#include "../runtime/gc/xgc.h"
#include "../runtime/gc/xcoro_gc.h"
#include "../runtime/gc/xalloc_unified.h"
#include "../runtime/gc/xsystem_heap.h"
#include "../runtime/xerror_codes.h"
#include "../runtime/xshared.h"
#include "../coro/xtimer_wheel.h"
#include "../os/os_thread.h"

/* ========== Branch Prediction ========== */
#if defined(__GNUC__) || defined(__clang__)
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif  // ========== Intern String Helper ==========
#define VM_INTERN(s) xr_string_intern(isolate, s, XR_STRLEN_LITERAL(s), 0)
#define VM_INTERN_KEY(s) xr_string_value(VM_INTERN(s))

/* ========== GC Safe Zone (no-op, retained for future STW GC) ========== */
#define GC_SAFE_ENTER(isolate) ((void) (isolate))
#define GC_SAFE_LEAVE(isolate) ((void) (isolate))

// Get current coroutine in cold path functions (outside run())
#define COLD_CORO(vm_ctx) ((struct XrCoroutine *) (vm_ctx)->current_coro)

/* ========== Cold Path Return Codes ========== */
#define VM_COLD_CONTINUE (-1)
#define VM_COLD_BREAK 0
#define VM_COLD_STARTFUNC 1
#define VM_COLD_BLOCKED 2
#define VM_COLD_YIELD 3
#define VM_COLD_ERROR 4
#define VM_COLD_FATAL 5
#define VM_COLD_GO_CHILD 6

// Cold-path throw helper
#define VM_COLD_THROW(frame_ptr, pc_ptr, code, ...)                                                \
    do {                                                                                           \
        (frame_ptr)->pc = (pc_ptr);                                                                \
        XrValue _exc = xr_exception_newf(isolate, (code), __VA_ARGS__);                            \
        xr_vm_unwind_with_trace(isolate, _exc);                                                    \
        return VM_COLD_ERROR;                                                                      \
    } while (0)

/* ========== Current Coroutine (used by every cold-path TU) ========== */

// Resolve the current coroutine for cold-path code running outside
// run(): prefer the ctx slot, fall back to the worker-local cache.
// Defined here so xvm_cold_call.c / cold_object.c / cold_coro.c /
// cold_chan.c all share a single inline body without any of them
// owning the symbol.
static inline XrCoroutine *vm_cold_get_coro(XrVMContext *vm_ctx) {
    if (vm_ctx->current_coro)
        return (XrCoroutine *) vm_ctx->current_coro;
    XrWorker *w = xr_current_worker();
    return w ? (XrCoroutine *) w->m->current_coro : NULL;
}

/* ========== Channel Deep Copy Helpers ========== */
/* Thin wrappers: canonical logic lives in xchannel_ops.h.
 * These adapt the VM-specific (isolate, vm_ctx) calling convention
 * to the shared (isolate, coro) interface. */
#include "../coro/xchannel_ops.h"

static inline XrValue vm_chan_copy_send(XrayIsolate *isolate, XrValue value) {
    return xr_chan_prepare_send(isolate, value);
}

static inline XrValue vm_chan_copy_recv(XrayIsolate *isolate, XrValue value, XrVMContext *vm_ctx) {
    XrCoroutine *coro = vm_ctx ? (XrCoroutine *) vm_ctx->current_coro : NULL;
    return xr_chan_copy_recv(isolate, value, coro);
}

/* ========== Shared Types ========== */

#define VM_CORO_COLLECT_MAX 10000

typedef struct {
    XrCoroutine *coro;
    const char *state;
} VmCoroEntry;

/* ========== Cold Path Function Declarations ========== */

XR_FUNC int vm_invoke_channel(XrayIsolate *isolate, XrVMContext *vm_ctx, XrChannel *ch,
                              int method_symbol, int nargs, XrValue *base, int a,
                              XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC int vm_invoke_task_handle(XrayIsolate *isolate, XrValue receiver, int method_symbol,
                                  int nargs, XrValue *base, int a, XrBcCallFrame *frame,
                                  XrInstruction *pc);
XR_FUNC int vm_invoke_coro_handle(XrayIsolate *isolate, XrValue receiver, int method_symbol,
                                  int nargs, XrValue *base, int a, XrBcCallFrame *frame,
                                  XrInstruction *pc);
XR_FUNC int vm_invoke_enum(XrayIsolate *isolate, XrValue receiver, int method_symbol, int nargs,
                           XrValue *base, int a, XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC int vm_invoke_class(XrayIsolate *isolate, XrVMContext *vm_ctx, XrValue receiver,
                            int method_symbol, const char *method_name_chars, int nargs,
                            XrValue *base, int a, XrBcCallFrame *frame, XrInstruction *pc,
                            int is_tail);
XR_FUNC int vm_superinvoke(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstruction instr,
                           XrValue *base, XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC int vm_setprop_type_dispatch(XrayIsolate *isolate, XrVMContext *vm_ctx, XrValue obj,
                                     int prop_symbol, XrValue value, XrValue *base, int a,
                                     XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC int vm_setprop_instance_setter(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstance *inst,
                                       XrValue obj, int prop_symbol, XrValue value, XrValue *base,
                                       int c, XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC int vm_collect_all_coros(XrayIsolate *isolate, VmCoroEntry *out, int max_out);
XR_FUNC int vm_coro_ctrl(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstruction instr,
                         XrValue *base);
XR_FUNC int vm_getprop_type_dispatch(XrayIsolate *isolate, XrVMContext *vm_ctx, XrValue obj,
                                     int prop_symbol, XrValue *base, int a, int b,
                                     XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC int vm_getprop_instance_getter(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstance *inst,
                                       XrValue obj, int prop_symbol, XrValue *base, int a,
                                       XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC int vm_invoke_module(XrayIsolate *isolate, XrVMContext *vm_ctx, XrValue receiver,
                             int method_symbol, int nargs, XrValue *base, int a,
                             XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC int vm_go(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstruction instr,
                  XrValue *base, XrBcCallFrame *frame);
XR_FUNC int vm_await(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstruction instr, XrValue *base,
                     XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC int vm_await_timeout(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstruction instr,
                             XrValue *base, XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC int vm_await_all(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstruction instr,
                         XrValue *base, XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC int vm_await_any(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstruction instr,
                         XrValue *base, XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC int vm_select_block(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstruction instr,
                            XrValue *base, XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC int vm_chan_send_timeout(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstruction instr,
                                 XrValue *base, XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC int vm_chan_recv_timeout(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstruction instr,
                                 XrValue *base, XrBcCallFrame *frame, XrInstruction *pc);

#endif
