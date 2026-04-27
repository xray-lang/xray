/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm.c - Register-based virtual machine implementation
 *
 * KEY CONCEPT:
 *   Register VM with computed goto dispatch, inline caching,
 *   OOP support, and native coroutine integration.
 *
 * WHY THIS DESIGN:
 *   - Computed goto for ~20% faster dispatch than switch-case
 *   - Inline caching for property access optimization
 *   - Coroutine-aware: yield points at function calls and loops
 *
 * RELATED MODULES:
 *   - xworker.c: Coroutine scheduling and execution
 *   - xcoro_gc.c: Per-coroutine garbage collection
 *   - xmodule.c: Module loading and symbol resolution
 */

/* ========== Debug Configuration ========== */

// #define XR_DEBUG_VM

/* ========== Short String Optimization ========== */

// XR_SHORT_STRING_THRESHOLD defined in core/xconstants.h

/* ========== Intern String Helper ========== */

// Compile-time safe intern: avoids manual string length counting

#ifdef XR_DEBUG_VM
#define VM_DEBUG_PRINT(...) printf("[VM DEBUG] " __VA_ARGS__)
#else
#define VM_DEBUG_PRINT(...) ((void) 0)
#endif  // ========== Includes ==========

#include "xvm_internal.h"
#include "../runtime/closure/xcell.h"
#include "../base/xchecks.h"
#include "xvm_checks.h"
#include "xdebug.h"
#include "../runtime/xray_debug_hooks.h"
#include "../runtime/gc/xgc.h"
#include "../runtime/xstdlib_bridge.h"
#include "../runtime/xerror_codes.h"
#include "../runtime/xshared.h"
#include "../runtime/gc/xsystem_heap.h"

#include "../runtime/gc/xcoro_gc.h"
#include "../runtime/gc/xalloc_unified.h"

#include <math.h>
#include <inttypes.h>
#include "../base/xarena.h"
#include "../os/os_time.h"
#include "../runtime/xstrbuf.h"
#include "../runtime/object/xstringbuilder.h"
#include "../runtime/object/xslice.h"
#include "../runtime/object/xjson.h"
#include "../runtime/class/xclass_descriptor.h"
#include "../module/xmodule.h"
#include "../coro/xchannel.h"
#include "../coro/xdeep_copy.h"
#include "../runtime/object/xbigint.h"
#include "../runtime/object/xrange.h"
#include "../runtime/object/xutf8.h"  // XR_UNICODE_MAX
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xstruct_layout.h"
#include "../runtime/value/xmethod_table.h"
#include "../runtime/xray_debug.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/value/xtype_feedback.h"
#ifdef XRAY_HAS_JIT
#include "../jit/xir_jit.h"
#endif

#include "../coro/xworker.h"
#include "../coro/xyieldable.h"
#include "../coro/xcoro_registry.h"
#include "../coro/xtask.h"
#include "../os/os_thread.h"

#include "xvm_profiler.h"

/* ========== Computed Goto Support ========== */
#ifndef XR_USE_COMPUTED_GOTO
#if defined(__GNUC__) || defined(__clang__)
#define XR_USE_COMPUTED_GOTO 1
#else
#define XR_USE_COMPUTED_GOTO 0
#endif
#endif  // ========== Builtin Method Symbol System ==========

/* Bound method implementation lives in runtime/closure/xbound_method.c. */

/* ========== VM Instruction Dispatch ========== */

#define READ_INSTRUCTION() (*frame->pc++)

/* ========== BigInt Binary Operation Helpers ========== */

typedef XrBigInt *(*XrBigIntBinOp)(struct XrCoroutine *, XrBigInt *, XrBigInt *);

// General binary op: ADD/SUB/MUL - auto-promote int operands to BigInt
static inline XrValue vm_bigint_binop(void *ctx, XrValue left, XrValue right, XrBigIntBinOp op) {
    XrBigInt *a =
        XR_IS_BIGINT(left) ? (XrBigInt *) XR_TO_PTR(left) : xr_bigint_new(ctx, XR_TO_INT(left));
    XrBigInt *b =
        XR_IS_BIGINT(right) ? (XrBigInt *) XR_TO_PTR(right) : xr_bigint_new(ctx, XR_TO_INT(right));
    return XR_FROM_PTR(op(ctx, a, b));
}

// Division/modulo op: returns XR_NOTFOUND on division by zero
static inline XrValue vm_bigint_divop(void *ctx, XrValue left, XrValue right, XrBigIntBinOp op) {
    XrBigInt *a =
        XR_IS_BIGINT(left) ? (XrBigInt *) XR_TO_PTR(left) : xr_bigint_new(ctx, XR_TO_INT(left));
    XrBigInt *b =
        XR_IS_BIGINT(right) ? (XrBigInt *) XR_TO_PTR(right) : xr_bigint_new(ctx, XR_TO_INT(right));
    if (xr_bigint_is_zero(b))
        return XR_NOTFOUND;
    return XR_FROM_PTR(op(ctx, a, b));
}

#include "xvm_cold_paths.h"

/*
 * Inline dispatch macro for cold-path function results inside vmcase blocks.
 * Handles all return codes except VM_COLD_CONTINUE (caller must handle that).
 * MUST be used directly in vmcase scope (uses vmbreak, goto, return).
 */
#define VM_DISPATCH_COLD(cr)                                                                       \
    do {                                                                                           \
        int _cr = (cr);                                                                            \
        if (_cr == VM_COLD_BREAK)                                                                  \
            vmbreak;                                                                               \
        if (_cr == VM_COLD_STARTFUNC)                                                              \
            goto startfunc;                                                                        \
        if (_cr == VM_COLD_BLOCKED)                                                                \
            return XR_VM_BLOCKED;                                                                  \
        if (_cr == VM_COLD_YIELD)                                                                  \
            return XR_VM_YIELD;                                                                    \
        if (_cr == VM_COLD_FATAL)                                                                  \
            return XR_VM_RUNTIME_ERROR;                                                            \
        if (_cr == VM_COLD_SPAWN_CONT)                                                             \
            return XR_VM_SPAWN_CONT;                                                               \
        if (_cr == VM_COLD_ERROR) {                                                                \
            if (vm_ctx->handler_count == 0)                                                        \
                return XR_VM_RUNTIME_ERROR;                                                        \
            goto startfunc;                                                                        \
        }                                                                                          \
    } while (0)

// Cold path functions moved to xvm_cold_paths.c

/* ========== VM Execution Loop ========== */

// Optimized VM loop with local variable caching
XrVMResult run(XrayIsolate *isolate, XrVMContext *vm_ctx) {
    XR_CHECK(isolate != NULL, "run: NULL isolate");
    XR_CHECK(vm_ctx != NULL, "run: NULL vm_ctx");

/* ========== VM State Access Macros ========== */
#define VM_STACK (vm_ctx->stack)
#define VM_STACK_TOP (vm_ctx->stack_top)
#define VM_SET_STACK_TOP(v)                                                                        \
    do {                                                                                           \
        vm_ctx->stack_top = (v);                                                                   \
    } while (0)
#define VM_FRAMES (vm_ctx->frames)
#define VM_FRAME_COUNT (vm_ctx->frame_count)
#define VM_SET_FRAME_COUNT(v)                                                                      \
    do {                                                                                           \
        vm_ctx->frame_count = (v);                                                                 \
    } while (0)

// Catchable runtime error: creates exception, does stack unwinding,
// jumps to catch handler if available, otherwise returns hard error.
// After this macro, control never falls through — it either gotos
// startfunc (catch handler) or returns XR_VM_RUNTIME_ERROR.
#define VM_RUNTIME_ERROR(code, fmt, ...)                                                           \
    do {                                                                                           \
        XrValue _exc = xr_exception_newf(isolate, (code), fmt, ##__VA_ARGS__);                     \
        savepc();                                                                                  \
        {                                                                                          \
            XrDebugHooks *_eh = (XrDebugHooks *) isolate->debug_hooks;                             \
            if (_eh && _eh->on_exception) {                                                        \
                bool _unc = (VM_HANDLER_COUNT == 0);                                               \
                const char *_msg =                                                                 \
                    XR_IS_EXCEPTION(_exc) ? xr_exception_get_message(_exc) : "<exception>";        \
                if (_eh->on_exception(isolate, _msg, _unc) == XR_DBG_ACTION_BREAK) {               \
                    VM_SET_EXCEPTION(_exc);                                                        \
                    ci->pc = pc - 1;                                                               \
                    return XR_VM_DEBUG_BREAK;                                                      \
                }                                                                                  \
            }                                                                                      \
        }                                                                                          \
        xr_vm_unwind_with_trace(isolate, _exc);                                                    \
        if (VM_HANDLER_COUNT == 0)                                                                 \
            return XR_VM_RUNTIME_ERROR;                                                            \
        goto startfunc;                                                                            \
    } while (0)

// Warning-only: prints to stderr but does NOT change control flow or VM state.
// Used for non-fatal diagnostics (e.g. await timeout) where execution continues.
#define VM_RUNTIME_WARN(fmt, ...) fprintf(stderr, "[xray] warn: " fmt "\n", ##__VA_ARGS__)
#define VM_INC_FRAME_COUNT                                                                         \
    do {                                                                                           \
        memset(&vm_ctx->frames[vm_ctx->frame_count], 0, sizeof(XrBcCallFrame));                    \
        vm_ctx->frame_count++;                                                                     \
    } while (0)
#define VM_DEC_FRAME_COUNT                                                                         \
    do {                                                                                           \
        vm_ctx->frame_count--;                                                                     \
    } while (0)
#define VM_HANDLERS (vm_ctx->handlers)
#define VM_HANDLER_COUNT (vm_ctx->handler_count)
#define VM_SET_HANDLER_COUNT(v)                                                                    \
    do {                                                                                           \
        vm_ctx->handler_count = (v);                                                               \
    } while (0)
#define VM_INC_HANDLER_COUNT                                                                       \
    do {                                                                                           \
        vm_ctx->handler_count++;                                                                   \
    } while (0)
#define VM_DEC_HANDLER_COUNT                                                                       \
    do {                                                                                           \
        vm_ctx->handler_count--;                                                                   \
    } while (0)
#define VM_EXCEPTION (vm_ctx->current_exception)
#define VM_SET_EXCEPTION(v)                                                                        \
    do {                                                                                           \
        vm_ctx->current_exception = (v);                                                           \
    } while (0)
// Get current coroutine: prefer vm_ctx, fallback to worker
#define VM_CURRENT_CORO                                                                            \
    (vm_ctx->current_coro ? vm_ctx->current_coro                                                   \
                          : (xr_current_worker() ? xr_current_worker()->m->current_coro : NULL))
#define VM_MODULE_BASE (vm_ctx->module_base_frame)
#define VM_SET_MODULE_BASE(v)                                                                      \
    do {                                                                                           \
        vm_ctx->module_base_frame = (v);                                                           \
    } while (0)
#define VM_TRACE (vm_ctx->trace_execution)

    /* ========== Local Variable Cache ========== */
    // Defensive zero-init: startfunc label reached before these are assigned
    // on the initial entry, and VM_RUNTIME_ERROR at the entry check uses
    // pc/ci for error reporting. Prevents -Wconditional-uninitialized.
    register XrInstruction *pc = NULL;
    register XrValue *base;
    XrValue *k;
    XrClosure *cl;
    XrBcCallFrame *ci = NULL;
    XrInstruction i;
    int invoke_is_tail = 0;  // shared flag: OP_INVOKE_TAIL sets 1, OP_INVOKE sets 0
    XrBcCallFrame *frame;

    /* Resolve the active per-isolate profiler once. NULL is the
     * default for builds compiled without XR_ENABLE_VM_PROFILER and
     * for isolates whose `profiler` slot was never allocated; the
     * VM_PROFILE_* macros / inline helpers all tolerate it. */
    VMProfiler *_vm_prof =
        (VMProfiler *) (vm_ctx && vm_ctx->isolate ? vm_ctx->isolate->profiler : NULL);
    vm_profiler_init(_vm_prof);
    VM_PROFILE_VARS();

// Per-frame struct storage in vm_ctx (lazy-allocated, persists across calls)

/* ========== Inline Macro Definitions ========== */
#undef R
#undef RA
#undef RB
#undef RC
#undef K
#undef KB
#undef KC
#define R(idx) (base[idx])
#define RA(inst) (base + GETARG_A(i))
#define RB(inst) (base + GETARG_B(i))
#define RC(inst) (base + GETARG_C(i))
#define K(idx) (k[idx])
#define KB(inst) (K(GETARG_B(i)))
#define KC(inst) (K(GETARG_C(i)))

/* ========== Stack Boundary Check (Dynamic Growth) ========== */
#define VM_STACK_CHECK(needed_slots)                                                               \
    do {                                                                                           \
        if (vm_ctx && vm_ctx->current_coro) {                                                      \
            XrCoroutine *_coro = (XrCoroutine *) vm_ctx->current_coro;                             \
            XrValue *_new_top = base + (needed_slots);                                             \
            XrValue *_stack_end = vm_ctx->stack + vm_ctx->stack_capacity;                          \
            bool _need_grow = (_new_top > _stack_end);                                             \
            /* Check frame capacity */                                                             \
            if (vm_ctx->frame_count + 1 >= vm_ctx->frame_capacity) {                               \
                _need_grow = true;                                                                 \
            }                                                                                      \
            /* Check max call depth limit */                                                       \
            if (vm_ctx->frame_count >= XR_FRAMES_MAX) {                                            \
                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW,                                            \
                                 "Stack overflow: recursion exceeds %d levels", XR_FRAMES_MAX);    \
            }                                                                                      \
            if (_need_grow) {                                                                      \
                int _extra = XR_STACK_GROW_DEFAULT;                                                \
                if (_new_top > _stack_end) {                                                       \
                    _extra = (int) (_new_top - _stack_end) + XR_STACK_GROW_PADDING;                \
                }                                                                                  \
                /* Save pc before grow */                                                          \
                savepc();                                                                          \
                /* vm_ctx IS &coro->vm_ctx, grow modifies it in place */                           \
                XrValue *_old_stack = vm_ctx->stack;                                               \
                int _old_cap = vm_ctx->stack_capacity;                                             \
                bool _grow_ok;                                                                     \
                _grow_ok = xr_coro_grow_stack(_coro, _extra);                                      \
                if (!_grow_ok) {                                                                   \
                    VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW,                                        \
                                     "Stack overflow: recursion exceeds %d levels (grow failed)",  \
                                     vm_ctx->frame_count);                                         \
                }                                                                                  \
                (void) _old_cap;                                                                   \
                /* Update cached local variables (don't goto startfunc - that restarts execution!) \
                 */                                                                                \
                ci = &VM_FRAMES[VM_FRAME_COUNT - 1];                                               \
                base = VM_STACK + ci->base_offset;                                                 \
            }                                                                                      \
        } else if (vm_ctx) {                                                                       \
            /* Defensive: static stack boundary check (no growth possible) */                      \
            XrValue *_new_top = base + (needed_slots);                                             \
            if (_new_top > vm_ctx->stack + vm_ctx->stack_capacity) {                               \
                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW,                                            \
                                 "Stack overflow: static stack exhausted (%d slots)",              \
                                 vm_ctx->stack_capacity);                                          \
            }                                                                                      \
            if (vm_ctx->frame_count + 1 >= vm_ctx->frame_capacity) {                               \
                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW,                                            \
                                 "Stack overflow: frame limit reached (%d)",                       \
                                 vm_ctx->frame_capacity);                                          \
            }                                                                                      \
        }                                                                                          \
    } while (0)

#define savepc() (ci->pc = pc)

/* Per-coroutine GC safepoint: check if GC is requested and trigger
** at a safe point where the VM stack is in a consistent state.
** This is called at loop back-edges, function boundaries,
** and after object allocation (via checkGC). */
#define XR_GC_STEP_REDS 50
#define VM_GC_SAFEPOINT()                                                                          \
    do {                                                                                           \
        XrCoroutine *_gc_coro = VM_CURRENT_CORO;                                                   \
        if (_gc_coro && _gc_coro->coro_gc && _gc_coro->coro_gc->gc_requested) {                    \
            _gc_coro->coro_gc->gc_requested = 0;                                                   \
            savepc();                                                                              \
            if (frame->closure && frame->closure->proto) {                                         \
                VM_SET_STACK_TOP(base + frame->closure->proto->maxstacksize);                      \
            }                                                                                      \
            xr_coro_gc_step(_gc_coro->coro_gc);                                                    \
            _gc_coro->reductions -= XR_GC_STEP_REDS;                                               \
        }                                                                                          \
    } while (0)

#define checkGC(c) VM_GC_SAFEPOINT()

/* ========== Write Barrier Macros ========== */
// Forward barrier: mark child when black parent writes GC-referencing value
#define VM_BARRIER_VAL(parent_obj, val)                                                            \
    do {                                                                                           \
        XrCoroutine *_bc = VM_CURRENT_CORO;                                                        \
        if (_bc && _bc->coro_gc)                                                                   \
            XR_GC_BARRIER_VAL(_bc->coro_gc, parent_obj, val);                                      \
    } while (0)
// Back barrier: revert black container to gray after mutation
#define VM_BARRIER_BACK(container_obj)                                                             \
    do {                                                                                           \
        XrCoroutine *_bc = VM_CURRENT_CORO;                                                        \
        if (_bc && _bc->coro_gc)                                                                   \
            xr_coro_gc_barrierback(_bc->coro_gc, XR_OBJ2GC(container_obj));                        \
    } while (0)

/* ========== Debug Hook (Zero Overhead When No Debugger) ========== */
/* All decision logic (breakpoints, stepping, pause, logpoints) lives in
 * the on_line hook impl (DAP side).  VM just resolves (path, line) and
 * acts on the return value: BREAK → save pc → return XR_VM_DEBUG_BREAK. */
#define VM_DEBUG_CHECK()                                                                           \
    do {                                                                                           \
        XrDebugHooks *_hooks = (XrDebugHooks *) isolate->debug_hooks;                              \
        if (_hooks && _hooks->is_enabled && _hooks->is_enabled(isolate)) {                         \
            int _pc_idx = (int) (pc - 1 - PROTO_CODE_BASE(cl->proto));                             \
            int _line = (_pc_idx >= 0 && _pc_idx < (int) PROTO_LINE_COUNT(cl->proto))              \
                            ? PROTO_LINE(cl->proto, _pc_idx)                                       \
                            : 0;                                                                   \
            if (_line > 0 && _hooks->on_line) {                                                    \
                XrDebugAction _act = _hooks->on_line(isolate, cl->proto->source_file, _line, cl,   \
                                                     ci, VM_FRAME_COUNT);                          \
                if (_act == XR_DBG_ACTION_BREAK) {                                                 \
                    ci->pc = pc - 1;                                                               \
                    return XR_VM_DEBUG_BREAK;                                                      \
                }                                                                                  \
            }                                                                                      \
        }                                                                                          \
    } while (0)

// vmbreak: continue to next instruction
#define vmbreak break

/* ========== Unified Operator Overload Macros ========== */

// Binary operator overload: tries to call operator method on left operand
// Usage: VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, XR_OP_ADD_FLAG, SYMBOL_OP_ADD, "+")
#define VM_TRY_BINARY_OP_OVERLOAD(left_val, right_val, result_reg, op_flag, op_symbol, op_name)    \
    do {                                                                                           \
        XrClass *_cls = NULL;                                                                      \
        if (xr_value_is_instance(left_val)) {                                                      \
            _cls = xr_instance_get_class(xr_value_to_instance(left_val));                          \
        } else if (XR_IS_STRUCT_REF(left_val)) {                                                   \
            _cls = *(XrClass **) xr_to_struct_ptr(left_val);                                       \
        }                                                                                          \
        if (_cls && XCLASS_HAS_OP(_cls, op_flag)) {                                                \
            XrMethod *_method = xr_class_lookup_method(_cls, op_symbol);                           \
            if (_method && _method->type == XMETHOD_OPERATOR && _method->as.closure) {             \
                XrClosure *_closure = _method->as.closure;                                         \
                XrProto *_proto = _closure->proto;                                                 \
                if (2 != _proto->numparams) {                                                      \
                    VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "operator %s requires 1 argument",    \
                                     op_name);                                                     \
                }                                                                                  \
                if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {                                             \
                    VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");                     \
                }                                                                                  \
                /* Place operator frame above caller's maxstacksize to avoid */                    \
                /* clobbering caller's local registers */                                          \
                int _safe_base = (int) (base - VM_STACK) + cl->proto->maxstacksize;                \
                VM_STACK[_safe_base] = (left_val);                                                 \
                VM_STACK[_safe_base + 1] = (right_val);                                            \
                savepc();                                                                          \
                int _fidx = VM_FRAME_COUNT;                                                        \
                VM_INC_FRAME_COUNT;                                                                \
                XrBcCallFrame *_new_frame = &VM_FRAMES[_fidx];                                     \
                _new_frame->closure = _closure;                                                    \
                _new_frame->pc = PROTO_CODE_BASE(_proto);                                          \
                _new_frame->base_offset = _safe_base;                                              \
                _new_frame->result_offset = (int) ((base + (result_reg)) - VM_STACK);              \
                _new_frame->call_status = XR_CALL_KEEP_FUNC;                                       \
                goto startfunc;                                                                    \
            }                                                                                      \
        }                                                                                          \
    } while (0)

// Unary operator overload: tries to call operator method on operand
// Usage: VM_TRY_UNARY_OP_OVERLOAD(vb, a, sym, "-")
#define VM_TRY_UNARY_OP_OVERLOAD(operand_val, result_reg, op_symbol, op_name)                      \
    do {                                                                                           \
        if (xr_value_is_instance(operand_val)) {                                                   \
            XrInstance *_inst = xr_value_to_instance(operand_val);                                 \
            XrClass *_cls = xr_instance_get_class(_inst);                                          \
            XrMethod *_method = xr_class_lookup_method(_cls, op_symbol);                           \
            if (_method && _method->type == XMETHOD_OPERATOR && _method->as.closure) {             \
                XrClosure *_closure = _method->as.closure;                                         \
                XrProto *_proto = _closure->proto;                                                 \
                if (1 != _proto->numparams) {                                                      \
                    VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT,                                       \
                                     "unary operator %s takes no arguments", op_name);             \
                }                                                                                  \
                if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {                                             \
                    VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");                     \
                }                                                                                  \
                /* Place operator frame above caller's maxstacksize */                             \
                int _safe_base = (int) (base - VM_STACK) + cl->proto->maxstacksize;                \
                VM_STACK[_safe_base] = (operand_val);                                              \
                savepc();                                                                          \
                int _fidx = VM_FRAME_COUNT;                                                        \
                VM_INC_FRAME_COUNT;                                                                \
                XrBcCallFrame *_new_frame = &VM_FRAMES[_fidx];                                     \
                _new_frame->closure = _closure;                                                    \
                _new_frame->pc = PROTO_CODE_BASE(_proto);                                          \
                _new_frame->base_offset = _safe_base;                                              \
                _new_frame->result_offset = (int) ((base + (result_reg)) - VM_STACK);              \
                _new_frame->call_status = XR_CALL_KEEP_FUNC;                                       \
                goto startfunc;                                                                    \
            }                                                                                      \
        }                                                                                          \
    } while (0)

    /* ========== startfunc: Enter New Function ========== */
startfunc:
    if (unlikely(VM_FRAME_COUNT > XR_FRAMES_MAX)) {
        VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "Stack overflow: recursion exceeds %d levels",
                         XR_FRAMES_MAX);
    }
    // Defensive assertion: frame count must be valid
    VM_DCHECK_FRAME_COUNT(VM_FRAME_COUNT, XR_FRAMES_MAX);

    ci = &VM_FRAMES[VM_FRAME_COUNT - 1];
    cl = ci->closure;
    XR_DCHECK(cl != NULL && cl->proto != NULL, "frame closure must be valid");

    // Defensive assertion: base_offset must be non-negative
    VM_DCHECK_BASE_OFFSET(ci->base_offset);

    pc = ci->pc;
    base = VM_STACK + ci->base_offset;
    k = (XrValue *) cl->proto->constants.data;
    frame = ci;

    // Check for continuation function return value (C frames only;
    // u.c is a union branch invalid for bytecode frames)
    if ((ci->call_status & XR_CALL_C) && ci->u.c.has_cfunc_result && ci->u.c.result_slot >= 0) {
        base[ci->u.c.result_slot] = ci->u.c.cfunc_result;
        ci->u.c.has_cfunc_result = false;
    }

    // Allocate struct_area for native struct storage.
    // Allocation failures are unrecoverable here (the frame layout is already
    // committed): emit a runtime error and unwind via VM_RUNTIME_ERROR.
    if (cl->proto->struct_area_size > 0) {
        int _sa_idx = VM_FRAME_COUNT - 1;
        if (!vm_ctx->struct_areas) {
            int cap = vm_ctx->frame_capacity;
            uint8_t **areas = (uint8_t **) xr_calloc(cap, sizeof(uint8_t *));
            if (!areas) {
                VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "failed to allocate struct_areas (cap=%d)",
                                 cap);
            }
            uint16_t *caps = (uint16_t *) xr_calloc(cap, sizeof(uint16_t));
            if (!caps) {
                xr_free(areas);
                VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                                 "failed to allocate struct_area_caps (cap=%d)", cap);
            }
            vm_ctx->struct_areas = areas;
            vm_ctx->struct_area_caps = caps;
            vm_ctx->struct_areas_cap = cap;
        } else if (_sa_idx >= vm_ctx->struct_areas_cap) {
            int old_cap = vm_ctx->struct_areas_cap;
            int new_cap = vm_ctx->frame_capacity;
            if (new_cap <= _sa_idx)
                new_cap = _sa_idx + 1;
            // Temp pointer + null check (project memory rule):
            // never overwrite the live pointer on realloc failure.
            uint8_t **new_areas =
                (uint8_t **) xr_realloc(vm_ctx->struct_areas, new_cap * sizeof(uint8_t *));
            if (!new_areas) {
                VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "failed to grow struct_areas to cap=%d",
                                 new_cap);
            }
            vm_ctx->struct_areas = new_areas;
            uint16_t *new_caps =
                (uint16_t *) xr_realloc(vm_ctx->struct_area_caps, new_cap * sizeof(uint16_t));
            if (!new_caps) {
                VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "failed to grow struct_area_caps to cap=%d",
                                 new_cap);
            }
            vm_ctx->struct_area_caps = new_caps;
            memset(vm_ctx->struct_areas + old_cap, 0, (new_cap - old_cap) * sizeof(uint8_t *));
            memset(vm_ctx->struct_area_caps + old_cap, 0, (new_cap - old_cap) * sizeof(uint16_t));
            vm_ctx->struct_areas_cap = new_cap;
        }
        uint16_t need = cl->proto->struct_area_size;
        if (vm_ctx->struct_area_caps[_sa_idx] < need) {
            xr_free(vm_ctx->struct_areas[_sa_idx]);
            uint8_t *area = (uint8_t *) xr_calloc(1, need);
            if (!area) {
                vm_ctx->struct_areas[_sa_idx] = NULL;
                vm_ctx->struct_area_caps[_sa_idx] = 0;
                VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "failed to allocate struct_area (size=%u)",
                                 need);
            }
            vm_ctx->struct_areas[_sa_idx] = area;
            vm_ctx->struct_area_caps[_sa_idx] = need;
        }
    }

    // Record defer start position for current frame
    if (isolate->vm.defer_frame_marks) {
        isolate->vm.defer_frame_marks[VM_FRAME_COUNT - 1] = isolate->vm.defer_count;
    }

    /* ========== Main Loop ========== */

#if XR_USE_COMPUTED_GOTO
/* ========== Computed Goto Jump Table ========== */
#undef vmbreak
#include "xvm_jumptab.h"

    // Read first instruction and dispatch
    i = *pc++;
    VM_DEBUG_CHECK();  // Check breakpoint before first instruction
    {
        OpCode _first_op = GET_OPCODE(i);
        VM_PROFILE_COUNT(_first_op);
        vmdispatch(_first_op);
    }
#else
// Switch mode macro definitions
#define vmcase(l) case l:
#undef vmbreak
#define vmbreak break

    // Standard switch dispatch
    for (;;) {
        i = *pc++;
        VM_DEBUG_CHECK();  // Check breakpoint at each instruction
        OpCode op = GET_OPCODE(i);
        VM_PROFILE_COUNT(op);

        switch (op) {
#endif

#define TRACE_EXECUTION()                                                                          \
    if (VM_TRACE) {                                                                                \
        printf("          ");                                                                      \
        for (XrValue *slot = VM_STACK; slot < VM_STACK_TOP; slot++) {                              \
            printf("[ ");                                                                          \
            xr_value_print(*slot);                                                                 \
            printf(" ]");                                                                          \
        }                                                                                          \
        printf("\n");                                                                              \
        xr_disassemble_instruction(ci->closure->proto,                                             \
                                   (int) (ci->pc - PROTO_CODE_BASE(ci->closure->proto) - 1));      \
    }

    /* ========================================================
    ** Data Loading Instructions (Inlined)
    ** ======================================================== */

    vmcase(OP_MOVE) {
        // MOVE A B: R[A] = R[B]
        R(GETARG_A(i)) = R(GETARG_B(i));
        vmbreak;
    }

    vmcase(OP_LOADI) {
        R(GETARG_A(i)) = xr_int(GETARG_sBx(i));
        vmbreak;
    }

    vmcase(OP_LOADF) {
        R(GETARG_A(i)) = xr_float((double) GETARG_sBx(i));
        vmbreak;
    }

    vmcase(OP_LOADK) {
        // LOADK A Bx: R[A] = K[Bx]
        R(GETARG_A(i)) = K(GETARG_Bx(i));
        vmbreak;
    }

    vmcase(OP_LOADNULL) {
        // LOADNULL A: R[A] = null
        R(GETARG_A(i)) = xr_null();
        vmbreak;
    }

    vmcase(OP_LOADTRUE) {
        // LOADTRUE A: R[A] = true
        R(GETARG_A(i)) = xr_bool(1);
        vmbreak;
    }

    vmcase(OP_LOADFALSE) {
        // LOADFALSE A: R[A] = false
        R(GETARG_A(i)) = xr_bool(0);
        vmbreak;
    }

/* Arithmetic + unary opcodes — see xvm_dispatch_arith.inc.c. */
#include "xvm_dispatch_arith.inc.c"
/* Box/unbox + StringBuilder helpers — see xvm_dispatch_data.inc.c. */
#include "xvm_dispatch_data.inc.c"
/* Bitwise opcodes — see xvm_dispatch_bitwise.inc.c. */
#include "xvm_dispatch_bitwise.inc.c"

/* Comparison opcodes (branching + producing) — see xvm_dispatch_compare.inc.c. */
#include "xvm_dispatch_compare.inc.c"
/* JMP / TEST / TESTSET — see xvm_dispatch_jump.inc.c. */
#include "xvm_dispatch_jump.inc.c"
/* GETBUILTIN / GETSHARED / SETSHARED / CLOSURE / UPVAL / CELL_* —
 * see xvm_dispatch_closure.inc.c. */
#include "xvm_dispatch_closure.inc.c"
/* PRINT / TYPEOF / TYPENAME / DUMP / TO* / COPY / CHR —
 * see xvm_dispatch_convert.inc.c. */
#include "xvm_dispatch_convert.inc.c"

/* Enum opcodes — see xvm_dispatch_enum.inc.c. */
#include "xvm_dispatch_enum.inc.c"

/* Container creation + array/map R/W + substring + index +
 * slice — see xvm_dispatch_collection.inc.c. */
#include "xvm_dispatch_collection.inc.c"

/* CALL / RETURN / TAILCALL family — see xvm_dispatch_call.inc.c. */
#include "xvm_dispatch_call.inc.c"

/* Class / field IC / Json / property family --
 * see xvm_dispatch_object.inc.c. */
#include "xvm_dispatch_object.inc.c"

/* OOP method invocation family (INVOKE / INVOKE_TAIL /
 * SUPERINVOKE / INVOKE_DIRECT / INVOKE_BUILTIN) --
 * see xvm_dispatch_invoke.inc.c. */
#include "xvm_dispatch_invoke.inc.c"

/* Exception opcodes — see xvm_dispatch_exception.inc.c. */
#include "xvm_dispatch_exception.inc.c"

    /* === Register spill instructions (fully inlined) === */
    vmcase(OP_SPILL) {
        /* ========== OP_SPILL - Register spill to stack frame spill slot (fully inlined) ==========
         */
        // S[A] = R[B], spill slot located after register area: base[MAXREGS + slot]
        int slot = GETARG_A(i);
        int reg = GETARG_B(i);
        base[MAXREGS + slot] = R(reg);
        vmbreak;
    }

    vmcase(OP_RELOAD) {
        /* ========== OP_RELOAD - Restore from spill slot to register (fully inlined) ========== */
        // R[A] = S[B]
        int reg = GETARG_A(i);
        int slot = GETARG_B(i);
        R(reg) = base[MAXREGS + slot];
        vmbreak;
    }

/* Module opcodes — see xvm_dispatch_module.inc.c. */
#include "xvm_dispatch_module.inc.c"

/* Assertion + regex literal opcodes — see file comment in
 * xvm_dispatch_assert.inc.c. The include expands inside
 * the dispatch switch and pulls in the vmcase bodies for
 * OP_ASSERT / OP_ASSERT_EQ / OP_ASSERT_NE / OP_REGEX_COMPILE. */
#include "xvm_dispatch_assert.inc.c"

/* Coroutine + scheduler opcodes — see xvm_dispatch_coro.inc.c. */
#include "xvm_dispatch_coro.inc.c"

/* Channel + select opcodes — see xvm_dispatch_chan.inc.c. */
#include "xvm_dispatch_chan.inc.c"

/* Defer / bytes / scope / time / sleep — see xvm_dispatch_misc.inc.c. */
#include "xvm_dispatch_misc.inc.c"
/* Typed-array / typed-field / struct — see xvm_dispatch_struct.inc.c. */
#include "xvm_dispatch_struct.inc.c"

    /* === Placeholder === */
    vmcase(OP_NOP)
        // No-op instruction
        vmbreak;

#if !XR_USE_COMPUTED_GOTO
    default:
        // Unknown or unimplemented instruction
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Unknown opcode %d", op);
}  // end switch
// Next instruction read at loop start: i = *pc++
}  // end for

#undef vmcase
#undef vmbreak
#endif  // ========== Closure Pending Handler ==========
/* Jumped to from OP_RETURN/RETURN0/RETURN1 when caller frame has
 * XR_CALL_CLOSURE_PENDING set by xr_yield_call_closure(). */
handle_closure_pending: {
    XrCoroutine *_pcoro = (XrCoroutine *) vm_ctx->current_coro;
    XrContinuation _cont = (XrContinuation) ci->u.c.continuation;
    void *_uctx = ci->u.c.continuation_ctx;

    // Clear pending flag before calling continuation
    ci->call_status &= ~XR_CALL_CLOSURE_PENDING;

    // No continuation registered — just return result via pending_closure_result.
    if (!_cont) {
        return XR_VM_OK;
    }

    XrValue _cresult;
    XrCFuncResult _cstatus = _cont(isolate, XR_RESUME_CLOSURE_DONE, _uctx, &_cresult);

    switch (_cstatus) {
        case XR_CFUNC_DONE:
            /* Continuation complete. If caller is a bytecode frame,
             * store result and continue execution. Otherwise return. */
            ci->call_status &= ~(XR_CALL_HAS_CONT | XR_CALL_C);
            if (ci->closure && ci->closure->proto) {
                XrValue *_base = VM_STACK + ci->base_offset;
                if (ci->u.c.result_slot >= 0) {
                    _base[ci->u.c.result_slot] = _cresult;
                }
                ci->u.c.has_cfunc_result = false;
                VM_SET_STACK_TOP(VM_STACK + ci->base_offset + ci->closure->proto->maxstacksize);
                goto startfunc;
            }
            // cfunc coro frame-0: store result and return
            _pcoro->result = _cresult;
            return XR_VM_OK;

        case XR_CFUNC_BLOCKED:
            return XR_VM_BLOCKED;

        case XR_CFUNC_YIELD:
            return XR_VM_YIELD;

        case XR_CFUNC_CALL_CLOSURE:
            // Another closure pushed, execute it
            goto startfunc;

        case XR_CFUNC_ERROR:
            return XR_VM_RUNTIME_ERROR;
    }
    // Should not reach here
    return XR_VM_RUNTIME_ERROR;
}

// Clean up macro definitions
#undef R
#undef RA
#undef RB
#undef RC
#undef K
#undef KB
#undef KC
#undef savepc
#undef vmbreak
#undef DISPATCH_OPCODE
}

/* ========== API functions moved to xvm_api.c ========== */
// Including: C closure call API, VM execution API, exception handling, Isolate API
