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

/* ========== Stack Growth Helper for Dispatch Functions ========== */
#include "../coro/xcoroutine.h"

/* Ensure the VM stack has room for a frame whose registers will occupy
 * slots [caller_base_offset, caller_base_offset + a + 1 + callee_maxstack)
 * and that one more frame fits in the frames array.
 *
 * Critical for combined slab layout (stack and frames in one allocation):
 * without this check, a callee whose register file extends past
 * stack_capacity will silently overwrite the frames array, corrupting
 * frame.closure / frame.pc and crashing on the next return.
 *
 * On grow, vm_ctx->stack and vm_ctx->frames may move, so callers MUST
 * use *base_inout and *frame_inout after the call rather than their
 * pre-call pointers.
 *
 * Returns true on success (no grow needed, or grow succeeded with
 * pointers refreshed). Returns false on grow failure; caller should
 * throw XR_ERR_STACK_OVERFLOW. */
static inline bool vm_ensure_call_stack(XrVMContext *vm_ctx, int needed_slots, XrValue **base_inout,
                                        XrBcCallFrame **frame_inout, XrInstruction *pc) {
    int needed = needed_slots;
    bool need_grow =
        (needed > vm_ctx->stack_capacity) || (vm_ctx->frame_count + 1 >= vm_ctx->frame_capacity);
    if (!need_grow)
        return true;

    XrCoroutine *coro = vm_get_coro(vm_ctx);
    if (!coro)
        return false;

    /* Snapshot offsets BEFORE grow — pointers may move. */
    int caller_base_off = (int) (*base_inout - vm_ctx->stack);
    int caller_frame_idx = (int) (*frame_inout - vm_ctx->frames);

    /* Save pc into the caller frame so a re-entry (e.g. via startfunc)
     * resumes at the right instruction. */
    (*frame_inout)->pc = pc;

    int extra = (needed > vm_ctx->stack_capacity) ? (needed - vm_ctx->stack_capacity + 64) : 64;
    if (extra < 64)
        extra = 64;
    if (!xr_coro_grow_stack(coro, extra))
        return false;

    /* Re-derive caller pointers from snapshots — stack/frames may have
     * been reallocated to new addresses. */
    *base_inout = vm_ctx->stack + caller_base_off;
    *frame_inout = &vm_ctx->frames[caller_frame_idx];
    return true;
}

/* ========== Unified Frame Push API ==========
 *
 * Single entry point for pushing a new bytecode call frame from external
 * dispatch helpers (xvm_invoke.c, xvm_props.c, ...). Subsumes:
 *   - frame depth limit check (XR_FRAMES_MAX)
 *   - stack/frames capacity grow (vm_ensure_call_stack)
 *   - savepc into the caller frame
 *   - frame_count increment (with debug-mode bounds DCHECK)
 *   - mandatory field init (closure / pc / base_offset)
 *
 * Why exist: every caller that pushes a frame must do exactly the above.
 * Splitting the check across N callsites makes it trivial to forget one
 * (root cause of the OP_INVOKE/vm_invoke_module overflow into frames[]).
 *
 * Pointers may move on grow — base_inout / frame_inout are refreshed.
 *
 * Returns: pointer to the new frame on success; caller may set optional
 *          fields (result_offset / call_status / u.l.* / flags). Returns
 *          NULL on failure; caller must surface it via VM_THROW. */
#include "../base/xconstants.h"
static inline XrBcCallFrame *vm_push_bc_frame(XrVMContext *vm_ctx, XrClosure *closure,
                                              int new_base_off_from_caller, XrValue **base_inout,
                                              XrBcCallFrame **frame_inout, XrInstruction *pc) {
    if (XR_UNLIKELY(vm_ctx->frame_count >= XR_FRAMES_MAX))
        return NULL;

    XrProto *proto = closure->proto;
    int abs_base_off = (int) (*base_inout - vm_ctx->stack) + new_base_off_from_caller;
    int needed = abs_base_off + proto->maxstacksize;

    /* savepc into caller frame so a re-entry resumes correctly. */
    (*frame_inout)->pc = pc;

    if (!vm_ensure_call_stack(vm_ctx, needed, base_inout, frame_inout, pc))
        return NULL;

    int fidx = vm_ctx->frame_count;
    XR_DCHECK(fidx < vm_ctx->frame_capacity,
              "vm_push_bc_frame: frame_count >= frame_capacity post-grow");
    memset(&vm_ctx->frames[fidx], 0, sizeof(XrBcCallFrame));
    vm_ctx->frame_count++;
    XrBcCallFrame *new_frame = &vm_ctx->frames[fidx];
    new_frame->closure = closure;
    new_frame->pc = PROTO_CODE_BASE(proto);
    new_frame->base_offset = abs_base_off;
    return new_frame;
}

/* ========== Channel Deep Copy Helpers ========== */
#include "../coro/xblock.h"
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
