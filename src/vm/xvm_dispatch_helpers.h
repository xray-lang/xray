/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_helpers.h - Shared declarations for VM dispatch helper functions
 *
 * Each helper handles a specific VM operation (invoke dispatch by receiver
 * type, property access, coroutine ops, channel ops). They return
 * XrDispatchAction so the caller can take the appropriate control-flow
 * action (continue, restart frame, block, yield, etc.).
 */

#ifndef XVM_DISPATCH_HELPERS_H
#define XVM_DISPATCH_HELPERS_H

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

/* ========== Dispatch Action Enum ========== */

/* Returned by dispatch helper functions to tell the VM loop what to do next. */
typedef enum {
    XR_DISP_NEXT,        /* Instruction completed; advance to next op */
    XR_DISP_FALLTHROUGH, /* Caller should continue its own fall-through path */
    XR_DISP_RESTART,     /* Frame changed (call pushed); reload from startfunc */
    XR_DISP_RAISE,       /* Exception thrown; caller checks catch reachability */
    XR_DISP_BLOCKED,     /* Coroutine blocked on I/O or channel */
    XR_DISP_YIELD,       /* Coroutine yielded voluntarily */
    XR_DISP_GO_CHILD,    /* Spawned child coroutine needs immediate execution */
    XR_DISP_FATAL,       /* Unrecoverable error; abort VM loop */
} XrDispatchAction;

/* ========== Intern String Helper ========== */
#define VM_INTERN(s) xr_string_intern(isolate, s, XR_STRLEN_LITERAL(s), 0)
#define VM_INTERN_KEY(s) xr_string_value(VM_INTERN(s))

/* ========== Throw Helper ========== */

/* Create exception, unwind stack, return XR_DISP_RAISE.
 * Must be used in functions that return XrDispatchAction. */
#define VM_THROW(frame_ptr, pc_ptr, code, ...)                                                     \
    do {                                                                                           \
        (frame_ptr)->pc = (pc_ptr);                                                                \
        XrValue _exc = xr_exception_newf(isolate, (code), __VA_ARGS__);                            \
        xr_vm_unwind_with_trace(isolate, _exc);                                                    \
        return XR_DISP_RAISE;                                                                      \
    } while (0)

/* ========== Current Coroutine Helper ========== */

/* Resolve current coroutine for dispatch helpers running outside run():
 * prefer vm_ctx slot, fall back to worker-local cache. */
static inline XrCoroutine *vm_get_coro(XrVMContext *vm_ctx) {
    if (vm_ctx->current_coro)
        return (XrCoroutine *) vm_ctx->current_coro;
    XrWorker *w = xr_current_worker();
    return w ? (XrCoroutine *) w->m->current_coro : NULL;
}

/* ========== Channel Deep Copy Helpers ========== */
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

/* ========== Class Resolution for OP_INVOKE ========== */

/* Resolve the receiver's XrClass for method dispatch.
 * Returns NULL for types that need special handling (module, null). */
static inline XrClass *invoke_resolve_class(XrayIsolate *isolate, XrValue receiver) {
    if (XR_IS_INT(receiver))
        return isolate->native_type_classes[XR_TINT];
    if (XR_IS_FLOAT(receiver))
        return isolate->native_type_classes[XR_TFLOAT];
    if (XR_IS_BOOL(receiver))
        return isolate->native_type_classes[XR_TBOOL];
    if (XR_IS_STRUCT_REF(receiver)) {
        uint8_t *sptr = (uint8_t *) xr_to_struct_ptr(receiver);
        return *(XrClass **) sptr;
    }
    if (!XR_IS_PTR(receiver))
        return NULL;

    XrGCHeader *gc = (XrGCHeader *) XR_TO_PTR(receiver);
    XrObjType type = XR_GC_GET_TYPE(gc);

    if (type == XR_TINSTANCE) {
        XrInstance *inst = (XrInstance *) gc;
        return inst->klass;
    }
    if (type == XR_TCLASS)
        return (XrClass *) gc;

    /* All native types (string, array, map, set, json, bigint, etc.) */
    if ((int) type < XR_NATIVE_TYPE_MAX)
        return isolate->native_type_classes[type];

    return NULL;
}

/* ========== Dispatch Helper Declarations ========== */

XR_FUNC XrDispatchAction vm_invoke_channel(XrayIsolate *isolate, XrVMContext *vm_ctx, XrChannel *ch,
                                           int method_symbol, int nargs, XrValue *base, int a,
                                           XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC XrDispatchAction vm_invoke_task_handle(XrayIsolate *isolate, XrValue receiver,
                                               int method_symbol, int nargs, XrValue *base, int a,
                                               XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC XrDispatchAction vm_invoke_coro_handle(XrayIsolate *isolate, XrValue receiver,
                                               int method_symbol, int nargs, XrValue *base, int a,
                                               XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC XrDispatchAction vm_invoke_enum(XrayIsolate *isolate, XrValue receiver, int method_symbol,
                                        int nargs, XrValue *base, int a, XrBcCallFrame *frame,
                                        XrInstruction *pc);
XR_FUNC XrDispatchAction vm_invoke_adt_instance(XrayIsolate *isolate, XrValue receiver,
                                                int method_symbol, int nargs, XrValue *base, int a,
                                                XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC XrDispatchAction vm_invoke_class(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                         XrValue receiver, int method_symbol, int nargs,
                                         XrValue *base, int a, XrBcCallFrame *frame,
                                         XrInstruction *pc, int is_tail);
XR_FUNC XrDispatchAction vm_superinvoke(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                        XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                        XrInstruction *pc);
XR_FUNC XrDispatchAction vm_setprop_type_dispatch(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                                  XrValue obj, int prop_symbol, XrValue value,
                                                  XrValue *base, int a, XrBcCallFrame *frame,
                                                  XrInstruction *pc);
XR_FUNC XrDispatchAction vm_setprop_instance_setter(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                                    XrInstance *inst, XrValue obj, int prop_symbol,
                                                    XrValue value, XrValue *base, int c,
                                                    XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC int vm_collect_all_coros(XrayIsolate *isolate, VmCoroEntry *out, int max_out);
XR_FUNC XrDispatchAction vm_coro_ctrl(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                      XrInstruction instr, XrValue *base);
XR_FUNC XrDispatchAction vm_getprop_type_dispatch(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                                  XrValue obj, int prop_symbol, XrValue *base,
                                                  int a, int b, XrBcCallFrame *frame,
                                                  XrInstruction *pc);
XR_FUNC XrDispatchAction vm_getprop_instance_getter(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                                    XrInstance *inst, XrValue obj, int prop_symbol,
                                                    XrValue *base, int a, XrBcCallFrame *frame,
                                                    XrInstruction *pc);
XR_FUNC XrDispatchAction vm_invoke_module(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                          XrValue receiver, int method_symbol, int nargs,
                                          XrValue *base, int a, XrBcCallFrame *frame,
                                          XrInstruction *pc);
XR_FUNC XrDispatchAction vm_go(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstruction instr,
                               XrValue *base, XrBcCallFrame *frame);
XR_FUNC XrDispatchAction vm_await(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstruction instr,
                                  XrValue *base, XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC XrDispatchAction vm_await_timeout(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                          XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                          XrInstruction *pc);
XR_FUNC XrDispatchAction vm_await_all(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                      XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                      XrInstruction *pc);
XR_FUNC XrDispatchAction vm_await_any(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                      XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                      XrInstruction *pc);
XR_FUNC XrDispatchAction vm_select_block(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                         XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                         XrInstruction *pc);
XR_FUNC XrDispatchAction vm_chan_send_timeout(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                              XrInstruction instr, XrValue *base,
                                              XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC XrDispatchAction vm_chan_recv_timeout(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                              XrInstruction instr, XrValue *base,
                                              XrBcCallFrame *frame, XrInstruction *pc);

#endif  // XVM_DISPATCH_HELPERS_H
