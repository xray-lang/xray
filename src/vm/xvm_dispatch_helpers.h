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
#include "../coro/xcoro_snapshot.h"
#include "../coro/xdeep_copy.h"
#include "../coro/xworker.h"
#include "../coro/xcoroutine.h"
#include "../coro/xyieldable.h"
#include "../coro/xcoro_registry.h"
#include "../module/xmodule.h"
#include "../runtime/mem/xheap.h"
#include "../runtime/mem/xcoro_heap.h"
#include "../runtime/mem/xalloc_unified.h"
#include "../runtime/mem/xsystem_heap.h"
#include "../runtime/xerror_codes.h"
#include "../runtime/xshared.h"
#include "../runtime/value/xtransfer_mode.h"
#include "../coro/xtimer_wheel.h"
#include "../os/os_thread.h"

#define XR_VM_YIELDABLE_READY_REDUCTION_COST 64

/* ========== Dispatch Action Enum ========== */

/* Returned by dispatch helper functions to tell the VM loop what to do next. */
typedef enum {
    XR_DISP_NEXT,        /* Instruction completed; advance to next op */
    XR_DISP_FALLTHROUGH, /* Caller should continue its own fall-through path */
    XR_DISP_RESTART,     /* Frame changed (call pushed); reload from startfunc */
    XR_DISP_RAISE,       /* Exception thrown; caller checks catch reachability */
    XR_DISP_BLOCKED,     /* Coroutine blocked on I/O or channel */
    XR_DISP_SWITCH,      /* Blocked; dispatch loop may switch to a LIFO partner
                          * in place (falls back to XR_VM_BLOCKED when no
                          * admissible partner exists) */
    XR_DISP_YIELD,       /* Coroutine yielded voluntarily */
    XR_DISP_GO_CHILD,    /* Spawned child coroutine needs immediate execution */
    XR_DISP_FATAL,       /* Unrecoverable error; abort VM loop */
} XrDispatchAction;

/* XR_OBJ_STORAGE_INHERIT is an allocation request, never an object-header
 * state. Constructor register 0 is `this`; its already-selected domain is the
 * authoritative domain for every nested allocation in the constructor body. */
static inline uint8_t vm_resolve_allocation_storage_mode(XrValue *base, uint8_t storage_mode) {
    if (storage_mode != XR_OBJ_STORAGE_INHERIT)
        return storage_mode;
    if (base && XR_IS_INSTANCE(base[0])) {
        XrObjHeader *receiver = (XrObjHeader *) XR_TO_PTR(base[0]);
        return XR_OBJ_GET_STORAGE(receiver);
    }
    return XR_OBJ_STORAGE_NORMAL;
}

/* Persist a frame-backed fixed-array value before storing it in a slot that
 * can outlive the current frame. Existing persistent storage is reused when
 * its element type and length match, preserving fixed-array value semantics
 * without allocating on every field update. */
XR_FUNC bool vm_store_persistent_array_ref(XrVMRuntime *isolate, XrValue *destination,
                                           XrValue source);

static inline uint8_t vm_channel_transfer_mode_before(const XrBcCallFrame *frame,
                                                      XrInstruction *pc_after_current) {
    if (!frame || !frame->closure || !frame->closure->proto || !pc_after_current)
        return XR_TRANSFER_SHARE;
    XrInstruction *base = PROTO_CODE_BASE(frame->closure->proto);
    if (pc_after_current <= base + 1)
        return XR_TRANSFER_SHARE;
    XrInstruction prev = pc_after_current[-2];
    if (GET_OPCODE(prev) != OP_NOP || GETARG_A(prev) != 6)
        return XR_TRANSFER_SHARE;
    return (uint8_t) (GETARG_Bx(prev) & XR_TRANSFER_MODE_MASK);
}

/* ========== Intern String Helper ========== */
#define VM_INTERN(s) xr_string_intern(isolate, s, XR_STRLEN_LITERAL(s), 0)
#define VM_INTERN_KEY(s) xr_string_value(VM_INTERN(s))

/* ========== Throw Helper ========== */

/* Create exception, unwind stack, return XR_DISP_RAISE.
 * Must be used in functions that return XrDispatchAction. */
#define VM_THROW(frame_ptr, pc_ptr, code, ...)                                                     \
    do {                                                                                           \
        (frame_ptr)->pc = (pc_ptr);                                                                \
        XrValue _exc = xr_panic_info_newf(isolate, (code), __VA_ARGS__);                           \
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

/* The exec-local heap that owns what this VM frame allocates.
 *
 * With a coroutine it is that coroutine's heap. An elided root has no
 * coroutine but does have an exec heap on its allocation context;
 * passing NULL there marked objects dead without running destructors or
 * reclaiming memory, so every drop in top-level code was discarded.
 *
 * Kept local to the VM on purpose. xr_current_coro_heap() must NOT take the
 * same fallback: standalone AOT installs an execution context only around
 * cancellation cleanup, so during ordinary coroutine execution the context
 * still names the root — and a coroutine object would be released to the root
 * heap instead of its own. */
static inline XrCoroHeap *vm_exec_local_heap(void) {
    XrAllocationContext *alloc = xr_alloc_context_current();
    return alloc ? alloc->local_heap : NULL;
}

/* The heap a weak handle belongs to: the same EXEC_LOCAL heap the target was
 * allocated on, which is what W4 restricts weak fields to. Same answer as
 * vm_exec_local_heap() — spelled separately only because the weak paths read
 * better naming what they need. */
static inline XrCoroHeap *vm_weak_heap(XrVMContext *vm_ctx) {
    (void) vm_ctx;
    return vm_exec_local_heap();
}

/* ========== VM Suspend Continuation Helpers ========== */

static inline void vm_suspend_replay_current(XrBcCallFrame *frame, XrInstruction *pc) {
    if (!frame)
        return;
    frame->pc = pc - 1;
}

static inline void vm_suspend_continue_from_next(XrBcCallFrame *frame, XrInstruction *pc) {
    if (!frame)
        return;
    frame->pc = pc;
}

static inline void vm_suspend_replay_yielded(XrBcCallFrame *frame, XrInstruction *pc) {
    if (!frame)
        return;
    vm_suspend_replay_current(frame, pc);
    frame->call_status |= XR_CALL_YIELDED;
}

static inline void vm_suspend_clear_yielded(XrBcCallFrame *frame) {
    if (!frame)
        return;
    frame->call_status &= ~(uint32_t) (XR_CALL_YIELDED | XR_CALL_REPLAY_PRESET);
}

/* Pre-publish replay suspend state BEFORE calling a yieldable native.
 *
 * Polling-style natives (EventCount.wait, Semaphore.acquire, CountdownLatch
 * .wait, WorkQueue.pop, ResultGroup.recv, ...) publish BLOCKED while still
 * inside the call, under the wait-queue lock. The moment that lock is
 * released a waker may claim the coroutine and resume it on another worker;
 * any frame write after that races the resumer (a missed XR_CALL_YIELDED
 * skips the replay entirely, and a late pc rollback lands on a frame the
 * resumed coroutine is already executing). So the replay state must be in
 * the frame before the native runs.
 *
 * Continuation-backed natives (Thread.join, sleep, I/O) instead resume
 * after the call instruction with the result delivered via
 * cfunc_result_slot: vm_backend_setup_yield_continuation sees
 * XR_CALL_REPLAY_PRESET, restores continue-from-next and clears it —
 * still before the coroutine becomes claimable. A native that completes
 * without suspending is unwound via vm_suspend_finish_preset. */
static inline void vm_suspend_preset_replay_yielded(XrBcCallFrame *frame, XrInstruction *pc) {
    if (!frame)
        return;
    frame->pc = pc - 1;
    frame->call_status |= XR_CALL_YIELDED | XR_CALL_REPLAY_PRESET;
}

/* Yieldable native returned without suspending (DONE): restore the
 * savepc() continue-from-next pc and drop the pre-set replay state. */
static inline void vm_suspend_finish_preset(XrBcCallFrame *frame, XrInstruction *pc) {
    if (!frame)
        return;
    frame->pc = pc;
    frame->call_status &= ~(uint32_t) (XR_CALL_YIELDED | XR_CALL_REPLAY_PRESET);
}

static inline XrDispatchAction vm_suspend_block_replay(XrBcCallFrame *frame, XrInstruction *pc) {
    vm_suspend_replay_current(frame, pc);
    return XR_DISP_BLOCKED;
}

static inline XrDispatchAction vm_suspend_block_replay_yielded(XrBcCallFrame *frame,
                                                               XrInstruction *pc) {
    vm_suspend_replay_yielded(frame, pc);
    return XR_DISP_BLOCKED;
}

static inline XrDispatchAction vm_suspend_yield_replay_yielded(XrBcCallFrame *frame,
                                                               XrInstruction *pc) {
    vm_suspend_replay_yielded(frame, pc);
    return XR_DISP_YIELD;
}

/* Reduction charge for a channel op that completed without blocking.
 * Shared by the out-of-line helpers (xvm_chan_ops.c) and the inline
 * buffered fast paths in the dispatch loop. */
#define XR_VM_CHAN_READY_REDUCTION_COST 64

static inline XrDispatchAction
vm_ready_operation_next_or_yield(XrVMRuntime *isolate, XrCoroutine *current, XrBcCallFrame *frame,
                                 XrInstruction *pc, int reduction_cost) {
    if (!current || !frame || reduction_cost <= 0)
        return XR_DISP_NEXT;
    if (xr_coro_consume_reds(current, reduction_cost) > 0)
        return XR_DISP_NEXT;
    xr_coro_set_reds(current, XR_CORO_REDUCTIONS);
    if (XR_LIKELY(isolate && isolate->vm.scheduler != NULL)) {
        vm_suspend_continue_from_next(frame, pc);
        return XR_DISP_YIELD;
    }
    return XR_DISP_NEXT;
}

/* ========== Stack Growth Helper for Dispatch Functions ========== */

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

static inline bool vm_rebind_after_native_call(XrVMContext *vm_ctx, int base_offset,
                                               int frame_index, XrValue **base_inout,
                                               XrBcCallFrame **frame_inout) {
    if (!vm_ctx || !base_inout || !frame_inout)
        return false;
    if (frame_index < 0 || frame_index >= vm_ctx->frame_count)
        return false;
    *base_inout = vm_ctx->stack + base_offset;
    *frame_inout = &vm_ctx->frames[frame_index];
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

/* ========== Channel ownership helpers ========== */
#include "../coro/xblock.h"
#include "../coro/xchannel_ops.h"

/* ========== Shared Types ========== */

#define VM_CORO_COLLECT_MAX 10000

typedef XrCoroSnapshotEntry VmCoroEntry;

/* ========== Class Resolution for OP_INVOKE ========== */

/* Resolve the receiver's XrClass for method dispatch.
 * Returns NULL for types that need special handling (module, null). */
static inline XrClass *invoke_resolve_class(XrVMRuntime *isolate, XrValue receiver) {
    if (XR_IS_INT(receiver))
        return isolate->core_rt->native_type_classes[XR_TINT];
    if (XR_IS_FLOAT(receiver))
        return isolate->core_rt->native_type_classes[XR_TFLOAT];
    if (XR_IS_BOOL(receiver))
        return isolate->core_rt->native_type_classes[XR_TBOOL];
    if (XR_IS_AGG_REF(receiver)) {
        XrAggregateLayout *layout = xr_vm_struct_ref_layout(isolate, receiver);
        if (layout && xr_aggregate_layout_is_headerless(layout))
            return xr_vm_struct_layout_class(&isolate->vm, xr_aggregate_layout_id(receiver));
        uint8_t *sptr = (uint8_t *) xr_to_struct_ptr(receiver);
        return *(XrClass **) sptr;
    }
    if (!XR_IS_PTR(receiver))
        return NULL;

    if (XR_IS_STRING(receiver))
        return isolate->core_rt->native_type_classes[XR_TSTRING];

    XrObjHeader *gc = (XrObjHeader *) XR_TO_PTR(receiver);
    XrObjType type = XR_OBJ_GET_TYPE(gc);

    if (type == XR_TINSTANCE) {
        XrInstance *inst = (XrInstance *) gc;
        return inst->klass;
    }
    if (type == XR_TCLASS)
        return (XrClass *) gc;

    /* All native types (string, array, map, set, json, bigint, etc.) */
    if ((int) type < XR_NATIVE_TYPE_MAX)
        return isolate->core_rt->native_type_classes[type];

    return NULL;
}

/* Native method ABIs accept values, never the VM's internal call-bound place
 * descriptor.  Closure methods keep places because their bytecode explicitly
 * consumes them with OP_PLACE_LOAD/STORE; primitives must receive the current
 * referent directly so a collection cannot accidentally persist a stack-slot
 * token as user data. */
static inline bool invoke_unwrap_primitive_args(XrVMContext *vm_ctx, XrValue *args, int nargs) {
    if (!vm_ctx || nargs < 0 || (nargs > 0 && !args))
        return false;
    for (int i = 0; i < nargs; i++) {
        if (!XR_IS_PLACE(args[i]))
            goto materialize_aggregate;
        {
            int64_t slot = args[i].i;
            if (slot < 0 || slot >= vm_ctx->stack_capacity)
                return false;
            args[i] = vm_ctx->stack[slot];
        }
    materialize_aggregate:
        if (XR_IS_AGG_REF(args[i]) && !XR_IS_ARRAY_REF(args[i]) && !XR_IS_SLICE_REF(args[i])) {
            args[i] = xr_vm_struct_materialize_instance(vm_ctx->isolate, args[i]);
            if (XR_IS_NULL(args[i]))
                return false;
        }
    }
    return true;
}

/* ========== Dispatch Helper Declarations ========== */

XR_FUNC XrDispatchAction vm_invoke_channel(XrVMRuntime *isolate, XrVMContext *vm_ctx, XrChannel *ch,
                                           int method_symbol, int nargs, XrValue *base, int a,
                                           XrBcCallFrame *frame, XrInstruction *pc,
                                           uint8_t transfer_mode);
XR_FUNC XrDispatchAction vm_invoke_task_handle(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                               XrValue receiver, int method_symbol, int nargs,
                                               XrValue *base, int a, XrBcCallFrame *frame,
                                               XrInstruction *pc);
XR_FUNC XrDispatchAction vm_invoke_coro_handle(XrVMRuntime *isolate, XrValue receiver,
                                               int method_symbol, int nargs, XrValue *base, int a,
                                               XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC XrDispatchAction vm_invoke_enum(XrVMRuntime *isolate, XrValue receiver, int method_symbol,
                                        int nargs, XrValue *base, int a, XrBcCallFrame *frame,
                                        XrInstruction *pc);
XR_FUNC XrDispatchAction vm_invoke_adt_instance(XrVMRuntime *isolate, XrValue receiver,
                                                int method_symbol, int nargs, XrValue *base, int a,
                                                XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC XrDispatchAction vm_invoke_class(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                         XrValue receiver, int method_symbol, int nargs,
                                         XrValue *base, int a, XrBcCallFrame *frame,
                                         XrInstruction *pc, int is_tail);
XR_FUNC XrDispatchAction vm_superinvoke(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                        XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                        XrInstruction *pc);
XR_FUNC XrDispatchAction vm_setprop_type_dispatch(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                                  XrValue obj, int prop_symbol, XrValue value,
                                                  XrValue *base, int a, XrBcCallFrame *frame,
                                                  XrInstruction *pc);
XR_FUNC XrDispatchAction vm_setprop_instance_setter(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                                    XrInstance *inst, XrValue obj, int prop_symbol,
                                                    XrValue value, XrValue *base, int c,
                                                    XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC int vm_collect_all_coros(XrVMRuntime *isolate, VmCoroEntry *out, int max_out);
XR_FUNC XrDispatchAction vm_coro_ctrl(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                      XrInstruction instr, XrValue *base);
XR_FUNC XrDispatchAction vm_getprop_type_dispatch(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                                  XrValue obj, int prop_symbol, XrValue *base,
                                                  int a, int b, XrBcCallFrame *frame,
                                                  XrInstruction *pc);
XR_FUNC XrDispatchAction vm_getprop_instance_getter(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                                    XrInstance *inst, XrValue obj, int prop_symbol,
                                                    XrValue *base, int a, XrBcCallFrame *frame,
                                                    XrInstruction *pc);
XR_FUNC XrDispatchAction vm_invoke_module(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                          XrValue receiver, int method_symbol, int nargs,
                                          XrValue *base, int a, XrBcCallFrame *frame,
                                          XrInstruction *pc);
XR_FUNC XrDispatchAction vm_go(XrVMRuntime *isolate, XrVMContext *vm_ctx, XrInstruction instr,
                               XrValue *base, XrBcCallFrame *frame);
XR_FUNC XrDispatchAction vm_thread_spawn(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                         XrInstruction instr, XrValue *base, XrBcCallFrame *frame);
XR_FUNC XrDispatchAction vm_gen_start(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                      XrInstruction instr, XrValue *base, XrBcCallFrame *frame);
XR_FUNC XrDispatchAction vm_await(XrVMRuntime *isolate, XrVMContext *vm_ctx, XrInstruction instr,
                                  XrValue *base, XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC XrDispatchAction vm_await_timeout(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                          XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                          XrInstruction *pc);
XR_FUNC XrDispatchAction vm_await_all(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                      XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                      XrInstruction *pc);
XR_FUNC XrDispatchAction vm_await_all_into(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                           XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                           XrInstruction *pc);
XR_FUNC XrDispatchAction vm_await_any(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                      XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                      XrInstruction *pc);
XR_FUNC XrDispatchAction vm_par_for_dispatch(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                             XrInstruction instr, XrValue *base,
                                             XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC XrDispatchAction vm_par_map_dispatch(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                             XrInstruction instr, XrValue *base,
                                             XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC XrDispatchAction vm_par_reduce_dispatch(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                                XrInstruction instr, XrValue *base,
                                                XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC XrDispatchAction vm_time_dispatch(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                          XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                          XrInstruction *pc);
XR_FUNC XrDispatchAction vm_select_block(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                         XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                         XrInstruction *pc);
XR_FUNC XrDispatchAction vm_chan_send(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                      XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                      XrInstruction *pc, uint8_t transfer_mode);
XR_FUNC XrDispatchAction vm_chan_recv(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                      XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                      XrInstruction *pc);
XR_FUNC XrDispatchAction vm_chan_send_timeout(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                              XrInstruction instr, XrValue *base,
                                              XrBcCallFrame *frame, XrInstruction *pc);
XR_FUNC XrDispatchAction vm_chan_recv_timeout(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                              XrInstruction instr, XrValue *base,
                                              XrBcCallFrame *frame, XrInstruction *pc);

#endif  // XVM_DISPATCH_HELPERS_H
