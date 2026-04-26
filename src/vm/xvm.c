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
    #define VM_DEBUG_PRINT(...) ((void)0)
#endif // ========== Includes ==========

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
#include <sys/time.h>
#include <unistd.h>
#include "../base/xarena.h"
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
#include "../runtime/object/xutf8.h"       // XR_UNICODE_MAX
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
#include <sched.h>

#include "xvm_profiler.h"

/* ========== Computed Goto Support ========== */
#ifndef XR_USE_COMPUTED_GOTO
  #if defined(__GNUC__) || defined(__clang__)
    #define XR_USE_COMPUTED_GOTO 1
  #else
    #define XR_USE_COMPUTED_GOTO 0
  #endif
#endif // ========== Builtin Method Symbol System ==========

/* Bound method implementation lives in runtime/closure/xbound_method.c. */

/* ========== VM Instruction Dispatch ========== */

#define READ_INSTRUCTION() (*frame->pc++)

/* ========== BigInt Binary Operation Helpers ========== */

typedef XrBigInt *(*XrBigIntBinOp)(struct XrCoroutine*, XrBigInt*, XrBigInt*);

// General binary op: ADD/SUB/MUL - auto-promote int operands to BigInt
static inline XrValue vm_bigint_binop(
    void *ctx, XrValue left, XrValue right, XrBigIntBinOp op)
{
    XrBigInt *a = XR_IS_BIGINT(left)  ? (XrBigInt*)XR_TO_PTR(left)  : xr_bigint_new(ctx, XR_TO_INT(left));
    XrBigInt *b = XR_IS_BIGINT(right) ? (XrBigInt*)XR_TO_PTR(right) : xr_bigint_new(ctx, XR_TO_INT(right));
    return XR_FROM_PTR(op(ctx, a, b));
}

// Division/modulo op: returns XR_NOTFOUND on division by zero
static inline XrValue vm_bigint_divop(
    void *ctx, XrValue left, XrValue right, XrBigIntBinOp op)
{
    XrBigInt *a = XR_IS_BIGINT(left)  ? (XrBigInt*)XR_TO_PTR(left)  : xr_bigint_new(ctx, XR_TO_INT(left));
    XrBigInt *b = XR_IS_BIGINT(right) ? (XrBigInt*)XR_TO_PTR(right) : xr_bigint_new(ctx, XR_TO_INT(right));
    if (xr_bigint_is_zero(b)) return XR_NOTFOUND;
    return XR_FROM_PTR(op(ctx, a, b));
}

#include "xvm_cold_paths.h"


/*
 * Inline dispatch macro for cold-path function results inside vmcase blocks.
 * Handles all return codes except VM_COLD_CONTINUE (caller must handle that).
 * MUST be used directly in vmcase scope (uses vmbreak, goto, return).
 */
#define VM_DISPATCH_COLD(cr) do { \
    int _cr = (cr); \
    if (_cr == VM_COLD_BREAK) vmbreak; \
    if (_cr == VM_COLD_STARTFUNC) goto startfunc; \
    if (_cr == VM_COLD_BLOCKED) return XR_VM_BLOCKED; \
    if (_cr == VM_COLD_YIELD) return XR_VM_YIELD; \
    if (_cr == VM_COLD_FATAL) return XR_VM_RUNTIME_ERROR; \
    if (_cr == VM_COLD_SPAWN_CONT) return XR_VM_SPAWN_CONT; \
    if (_cr == VM_COLD_ERROR) { \
        if (vm_ctx->handler_count == 0) return XR_VM_RUNTIME_ERROR; \
        goto startfunc; \
    } \
} while(0)


// Cold path functions moved to xvm_cold_paths.c

/* ========== VM Execution Loop ========== */

// Optimized VM loop with local variable caching
XrVMResult run(XrayIsolate *isolate, XrVMContext *vm_ctx) {
    XR_CHECK(isolate != NULL, "run: NULL isolate");
    XR_CHECK(vm_ctx != NULL, "run: NULL vm_ctx");

    /* ========== VM State Access Macros ========== */
    #define VM_STACK            (vm_ctx->stack)
    #define VM_STACK_TOP        (vm_ctx->stack_top)
    #define VM_SET_STACK_TOP(v) do { vm_ctx->stack_top = (v); } while(0)
    #define VM_FRAMES           (vm_ctx->frames)
    #define VM_FRAME_COUNT      (vm_ctx->frame_count)
    #define VM_SET_FRAME_COUNT(v) do { vm_ctx->frame_count = (v); } while(0)

    // Catchable runtime error: creates exception, does stack unwinding,
    // jumps to catch handler if available, otherwise returns hard error.
    // After this macro, control never falls through — it either gotos
    // startfunc (catch handler) or returns XR_VM_RUNTIME_ERROR.
    #define VM_RUNTIME_ERROR(code, fmt, ...) do { \
        XrValue _exc = xr_exception_newf(isolate, (code), fmt, ##__VA_ARGS__); \
        savepc(); \
        { \
            XrDebugHooks *_eh = (XrDebugHooks *)isolate->debug_hooks; \
            if (_eh && _eh->on_exception) { \
                bool _unc = (VM_HANDLER_COUNT == 0); \
                const char *_msg = XR_IS_EXCEPTION(_exc) \
                    ? xr_exception_get_message(_exc) : "<exception>"; \
                if (_eh->on_exception(isolate, _msg, _unc) == XR_DBG_ACTION_BREAK) { \
                    VM_SET_EXCEPTION(_exc); \
                    ci->pc = pc - 1; \
                    return XR_VM_DEBUG_BREAK; \
                } \
            } \
        } \
        xr_vm_unwind_with_trace(isolate, _exc); \
        if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR; \
        goto startfunc; \
    } while(0)

    // Warning-only: prints to stderr but does NOT change control flow or VM state.
    // Used for non-fatal diagnostics (e.g. await timeout) where execution continues.
    #define VM_RUNTIME_WARN(fmt, ...) \
        fprintf(stderr, "[xray] warn: " fmt "\n", ##__VA_ARGS__)
    #define VM_INC_FRAME_COUNT  do { \
        memset(&vm_ctx->frames[vm_ctx->frame_count], 0, sizeof(XrBcCallFrame)); \
        vm_ctx->frame_count++; \
    } while(0)
    #define VM_DEC_FRAME_COUNT  do { vm_ctx->frame_count--; } while(0)
    #define VM_HANDLERS         (vm_ctx->handlers)
    #define VM_HANDLER_COUNT    (vm_ctx->handler_count)
    #define VM_SET_HANDLER_COUNT(v) do { vm_ctx->handler_count = (v); } while(0)
    #define VM_INC_HANDLER_COUNT do { vm_ctx->handler_count++; } while(0)
    #define VM_DEC_HANDLER_COUNT do { vm_ctx->handler_count--; } while(0)
    #define VM_EXCEPTION        (vm_ctx->current_exception)
    #define VM_SET_EXCEPTION(v) do { vm_ctx->current_exception = (v); } while(0)
    // Get current coroutine: prefer vm_ctx, fallback to worker
    #define VM_CURRENT_CORO     (vm_ctx->current_coro ? vm_ctx->current_coro : \
                                 (xr_current_worker() ? xr_current_worker()->m->current_coro : NULL))
    #define VM_MODULE_BASE      (vm_ctx->module_base_frame)
    #define VM_SET_MODULE_BASE(v) do { vm_ctx->module_base_frame = (v); } while(0)
    #define VM_TRACE            (vm_ctx->trace_execution)

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
    int invoke_is_tail = 0; // shared flag: OP_INVOKE_TAIL sets 1, OP_INVOKE sets 0
    XrBcCallFrame *frame;

    /* Resolve the active per-isolate profiler once. NULL is the
     * default for builds compiled without XR_ENABLE_VM_PROFILER and
     * for isolates whose `profiler` slot was never allocated; the
     * VM_PROFILE_* macros / inline helpers all tolerate it. */
    VMProfiler *_vm_prof = (VMProfiler *)(vm_ctx && vm_ctx->isolate
                                           ? vm_ctx->isolate->profiler
                                           : NULL);
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
    #define R(idx)      (base[idx])
    #define RA(inst)    (base + GETARG_A(i))
    #define RB(inst)    (base + GETARG_B(i))
    #define RC(inst)    (base + GETARG_C(i))
    #define K(idx)      (k[idx])
    #define KB(inst)    (K(GETARG_B(i)))
    #define KC(inst)    (K(GETARG_C(i)))

    /* ========== Stack Boundary Check (Dynamic Growth) ========== */
    #define VM_STACK_CHECK(needed_slots) do { \
        if (vm_ctx && vm_ctx->current_coro) { \
            XrCoroutine *_coro = (XrCoroutine*)vm_ctx->current_coro; \
            XrValue *_new_top = base + (needed_slots); \
            XrValue *_stack_end = vm_ctx->stack + vm_ctx->stack_capacity; \
            bool _need_grow = (_new_top > _stack_end); \
            /* Check frame capacity */ \
            if (vm_ctx->frame_count + 1 >= vm_ctx->frame_capacity) { \
                _need_grow = true; \
            } \
            /* Check max call depth limit */ \
            if (vm_ctx->frame_count >= XR_FRAMES_MAX) { \
                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, \
                    "Stack overflow: recursion exceeds %d levels", XR_FRAMES_MAX); \
            } \
            if (_need_grow) { \
                int _extra = XR_STACK_GROW_DEFAULT; \
                if (_new_top > _stack_end) { \
                    _extra = (int)(_new_top - _stack_end) + XR_STACK_GROW_PADDING; \
                } \
                /* Save pc before grow */ \
                savepc(); \
                /* vm_ctx IS &coro->vm_ctx, grow modifies it in place */ \
                XrValue *_old_stack = vm_ctx->stack; \
                int _old_cap = vm_ctx->stack_capacity; \
                bool _grow_ok; \
                _grow_ok = xr_coro_grow_stack(_coro, _extra); \
                if (!_grow_ok) { \
                    VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, \
                        "Stack overflow: recursion exceeds %d levels (grow failed)", vm_ctx->frame_count); \
                } \
                (void)_old_cap; \
                /* Update cached local variables (don't goto startfunc - that restarts execution!) */ \
                ci = &VM_FRAMES[VM_FRAME_COUNT - 1]; \
                base = VM_STACK + ci->base_offset; \
            } \
        } else if (vm_ctx) { \
            /* Defensive: static stack boundary check (no growth possible) */ \
            XrValue *_new_top = base + (needed_slots); \
            if (_new_top > vm_ctx->stack + vm_ctx->stack_capacity) { \
                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, \
                    "Stack overflow: static stack exhausted (%d slots)", vm_ctx->stack_capacity); \
            } \
            if (vm_ctx->frame_count + 1 >= vm_ctx->frame_capacity) { \
                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, \
                    "Stack overflow: frame limit reached (%d)", vm_ctx->frame_capacity); \
            } \
        } \
    } while(0)

    #define savepc()    (ci->pc = pc)

    /* Per-coroutine GC safepoint: check if GC is requested and trigger
    ** at a safe point where the VM stack is in a consistent state.
    ** This is called at loop back-edges, function boundaries,
    ** and after object allocation (via checkGC). */
    #define XR_GC_STEP_REDS 50
    #define VM_GC_SAFEPOINT() do { \
        XrCoroutine *_gc_coro = VM_CURRENT_CORO; \
        if (_gc_coro && _gc_coro->coro_gc && \
            _gc_coro->coro_gc->gc_requested) { \
            _gc_coro->coro_gc->gc_requested = 0; \
            savepc(); \
            if (frame->closure && frame->closure->proto) { \
                VM_SET_STACK_TOP(base + frame->closure->proto->maxstacksize); \
            } \
            xr_coro_gc_step(_gc_coro->coro_gc); \
            _gc_coro->reductions -= XR_GC_STEP_REDS; \
        } \
    } while(0)

    #define checkGC(c) VM_GC_SAFEPOINT()

    /* ========== Write Barrier Macros ========== */
    // Forward barrier: mark child when black parent writes GC-referencing value
    #define VM_BARRIER_VAL(parent_obj, val) do { \
        XrCoroutine *_bc = VM_CURRENT_CORO; \
        if (_bc && _bc->coro_gc) \
            XR_GC_BARRIER_VAL(_bc->coro_gc, parent_obj, val); \
    } while(0)
    // Back barrier: revert black container to gray after mutation
    #define VM_BARRIER_BACK(container_obj) do { \
        XrCoroutine *_bc = VM_CURRENT_CORO; \
        if (_bc && _bc->coro_gc) \
            xr_coro_gc_barrierback(_bc->coro_gc, XR_OBJ2GC(container_obj)); \
    } while(0)

    /* ========== Debug Hook (Zero Overhead When No Debugger) ========== */
    /* All decision logic (breakpoints, stepping, pause, logpoints) lives in
     * the on_line hook impl (DAP side).  VM just resolves (path, line) and
     * acts on the return value: BREAK → save pc → return XR_VM_DEBUG_BREAK. */
    #define VM_DEBUG_CHECK() do { \
        XrDebugHooks *_hooks = (XrDebugHooks *)isolate->debug_hooks; \
        if (_hooks && _hooks->is_enabled && _hooks->is_enabled(isolate)) { \
            int _pc_idx = (int)(pc - 1 - PROTO_CODE_BASE(cl->proto)); \
            int _line = (_pc_idx >= 0 && _pc_idx < (int)PROTO_LINE_COUNT(cl->proto)) \
                ? PROTO_LINE(cl->proto, _pc_idx) : 0; \
            if (_line > 0 && _hooks->on_line) { \
                XrDebugAction _act = _hooks->on_line( \
                    isolate, cl->proto->source_file, _line, cl, ci, VM_FRAME_COUNT); \
                if (_act == XR_DBG_ACTION_BREAK) { \
                    ci->pc = pc - 1; \
                    return XR_VM_DEBUG_BREAK; \
                } \
            } \
        } \
    } while(0)

    // vmbreak: continue to next instruction
    #define vmbreak     break

    /* ========== Unified Operator Overload Macros ========== */

    // Binary operator overload: tries to call operator method on left operand
    // Usage: VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, XR_OP_ADD_FLAG, SYMBOL_OP_ADD, "+")
    #define VM_TRY_BINARY_OP_OVERLOAD(left_val, right_val, result_reg, op_flag, op_symbol, op_name) do { \
        XrClass *_cls = NULL; \
        if (xr_value_is_instance(left_val)) { \
            _cls = xr_instance_get_class(xr_value_to_instance(left_val)); \
        } else if (XR_IS_STRUCT_REF(left_val)) { \
            _cls = *(XrClass**)xr_to_struct_ptr(left_val); \
        } \
        if (_cls && XCLASS_HAS_OP(_cls, op_flag)) { \
            XrMethod *_method = xr_class_lookup_method(_cls, op_symbol); \
            if (_method && _method->type == XMETHOD_OPERATOR && _method->as.closure) { \
                XrClosure *_closure = _method->as.closure; \
                XrProto *_proto = _closure->proto; \
                if (2 != _proto->numparams) { \
                    VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, \
                        "operator %s requires 1 argument", op_name); \
                } \
                if (VM_FRAME_COUNT >= XR_FRAMES_MAX) { \
                    VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow"); \
                } \
                /* Place operator frame above caller's maxstacksize to avoid */ \
                /* clobbering caller's local registers */ \
                int _safe_base = (int)(base - VM_STACK) + cl->proto->maxstacksize; \
                VM_STACK[_safe_base]     = (left_val); \
                VM_STACK[_safe_base + 1] = (right_val); \
                savepc(); \
                int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT; \
                XrBcCallFrame *_new_frame = &VM_FRAMES[_fidx]; \
                _new_frame->closure = _closure; \
                _new_frame->pc = PROTO_CODE_BASE(_proto); \
                _new_frame->base_offset = _safe_base; \
                _new_frame->result_offset = (int)((base + (result_reg)) - VM_STACK); \
                _new_frame->call_status = XR_CALL_KEEP_FUNC; \
                goto startfunc; \
            } \
        } \
    } while(0)

    // Unary operator overload: tries to call operator method on operand
    // Usage: VM_TRY_UNARY_OP_OVERLOAD(vb, a, sym, "-")
    #define VM_TRY_UNARY_OP_OVERLOAD(operand_val, result_reg, op_symbol, op_name) do { \
        if (xr_value_is_instance(operand_val)) { \
            XrInstance *_inst = xr_value_to_instance(operand_val); \
            XrClass *_cls = xr_instance_get_class(_inst); \
            XrMethod *_method = xr_class_lookup_method(_cls, op_symbol); \
            if (_method && _method->type == XMETHOD_OPERATOR && _method->as.closure) { \
                XrClosure *_closure = _method->as.closure; \
                XrProto *_proto = _closure->proto; \
                if (1 != _proto->numparams) { \
                    VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, \
                        "unary operator %s takes no arguments", op_name); \
                } \
                if (VM_FRAME_COUNT >= XR_FRAMES_MAX) { \
                    VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow"); \
                } \
                /* Place operator frame above caller's maxstacksize */ \
                int _safe_base = (int)(base - VM_STACK) + cl->proto->maxstacksize; \
                VM_STACK[_safe_base] = (operand_val); \
                savepc(); \
                int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT; \
                XrBcCallFrame *_new_frame = &VM_FRAMES[_fidx]; \
                _new_frame->closure = _closure; \
                _new_frame->pc = PROTO_CODE_BASE(_proto); \
                _new_frame->base_offset = _safe_base; \
                _new_frame->result_offset = (int)((base + (result_reg)) - VM_STACK); \
                _new_frame->call_status = XR_CALL_KEEP_FUNC; \
                goto startfunc; \
            } \
        } \
    } while(0)

    /* ========== startfunc: Enter New Function ========== */
startfunc:
    if (unlikely(VM_FRAME_COUNT > XR_FRAMES_MAX)) {
        VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW,
            "Stack overflow: recursion exceeds %d levels", XR_FRAMES_MAX);
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
    k = (XrValue*)cl->proto->constants.data;
    frame = ci;

    // Check for continuation function return value
    if (ci->u.c.has_cfunc_result && ci->u.c.result_slot >= 0) {
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
            uint8_t **areas = (uint8_t**)xr_calloc(cap, sizeof(uint8_t*));
            if (!areas) {
                VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                    "failed to allocate struct_areas (cap=%d)", cap);
            }
            uint16_t *caps = (uint16_t*)xr_calloc(cap, sizeof(uint16_t));
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
            if (new_cap <= _sa_idx) new_cap = _sa_idx + 1;
            // Temp pointer + null check (project memory rule):
            // never overwrite the live pointer on realloc failure.
            uint8_t **new_areas = (uint8_t**)xr_realloc(
                vm_ctx->struct_areas, new_cap * sizeof(uint8_t*));
            if (!new_areas) {
                VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                    "failed to grow struct_areas to cap=%d", new_cap);
            }
            vm_ctx->struct_areas = new_areas;
            uint16_t *new_caps = (uint16_t*)xr_realloc(
                vm_ctx->struct_area_caps, new_cap * sizeof(uint16_t));
            if (!new_caps) {
                VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                    "failed to grow struct_area_caps to cap=%d", new_cap);
            }
            vm_ctx->struct_area_caps = new_caps;
            memset(vm_ctx->struct_areas + old_cap, 0, (new_cap - old_cap) * sizeof(uint8_t*));
            memset(vm_ctx->struct_area_caps + old_cap, 0, (new_cap - old_cap) * sizeof(uint16_t));
            vm_ctx->struct_areas_cap = new_cap;
        }
        uint16_t need = cl->proto->struct_area_size;
        if (vm_ctx->struct_area_caps[_sa_idx] < need) {
            xr_free(vm_ctx->struct_areas[_sa_idx]);
            uint8_t *area = (uint8_t*)xr_calloc(1, need);
            if (!area) {
                vm_ctx->struct_areas[_sa_idx] = NULL;
                vm_ctx->struct_area_caps[_sa_idx] = 0;
                VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                    "failed to allocate struct_area (size=%u)", need);
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
    #define vmcase(l)  case l:
    #undef vmbreak
    #define vmbreak    break

    // Standard switch dispatch
    for (;;) {
        i = *pc++;
        VM_DEBUG_CHECK();  // Check breakpoint at each instruction
        OpCode op = GET_OPCODE(i);
        VM_PROFILE_COUNT(op);

        switch (op) {
#endif

#define TRACE_EXECUTION() \
    if (VM_TRACE) { \
        printf("          "); \
        for (XrValue *slot = VM_STACK; slot < VM_STACK_TOP; slot++) { \
            printf("[ "); \
            xr_value_print(*slot); \
            printf(" ]"); \
        } \
        printf("\n"); \
        xr_disassemble_instruction(ci->closure->proto, \
                                  (int)(ci->pc - PROTO_CODE_BASE(ci->closure->proto) - 1)); \
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
                R(GETARG_A(i)) = xr_float((double)GETARG_sBx(i));
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

            /* ========================================================
            ** Arithmetic Instructions (Hot Path Inlined)
            ** ======================================================== */

            vmcase(OP_ADD) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue vb = R(b);
                XrValue vc = R(c);

                // Fast path: integer addition (wrap on overflow)
                if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
                    XR_SET_INT(R(a), (int64_t)((uint64_t)XR_TO_INT(vb) + (uint64_t)XR_TO_INT(vc)));
                    vmbreak;
                }
                // Fast path: float + float (skip XR_TONUMBER overhead)
                if (XR_IS_FLOAT(vb) && XR_IS_FLOAT(vc)) {
                    XR_SET_FLOAT(R(a), vb.f + vc.f);
                    vmbreak;
                }
                // Mixed/extended numeric addition
                {
                    double nb = 0, nc = 0;
                    if (XR_TONUMBER(vb, nb) && XR_TONUMBER(vc, nc)) {
                        XR_SET_FLOAT(R(a), nb + nc);
                        vmbreak;
                    }
                }
                // BigInt addition (auto-promote int operands)
                if (XR_IS_BIGINT(vb) || XR_IS_BIGINT(vc)) {
                    R(a) = vm_bigint_binop(VM_CURRENT_CORO, vb, vc, xr_bigint_add);
                    vmbreak;
                }
                // Operator overload (unified macro)
                VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, XR_OP_ADD_FLAG, SYMBOL_OP_ADD, "+");
                // String concatenation: only reachable via any+any runtime path
                // (compiler guarantees typed string+string uses STRBUF sequence)
                if (XR_IS_STRING(vb) || XR_IS_STRING(vc)) {
                    // Fast path: both are strings — use str_data/len directly, no promote
                    if (XR_IS_STRING(vb) && XR_IS_STRING(vc)) {
                        const char *db = xr_value_str_data(&vb);
                        uint32_t lb = xr_value_str_len(&vb);
                        const char *dc = xr_value_str_data(&vc);
                        uint32_t lc = xr_value_str_len(&vc);
                        size_t total_len = lb + lc;
                        if (total_len < XR_SHORT_STRING_THRESHOLD) {
                            char stack_buf[XR_SHORT_STRING_THRESHOLD];
                            memcpy(stack_buf, db, lb);
                            memcpy(stack_buf + lb, dc, lc);
                            R(a) = xr_string_value(xr_string_intern(isolate, stack_buf, total_len, 0));
                            vmbreak;
                        }
                        XrStrBuf *sb = xr_strbuf_tmp(isolate);
                        xr_strbuf_append_cstr(sb, db, lb);
                        xr_strbuf_append_cstr(sb, dc, lc);
                        R(a) = xr_string_value(xr_strbuf_to_string(sb));
                        vmbreak;
                    }
                    // Slow path: one operand needs toString conversion
                    XrString *str_b = xr_value_to_string(isolate, vb);
                    XrString *str_c = xr_value_to_string(isolate, vc);
                    size_t total_len = str_b->length + str_c->length;

                    if (total_len < XR_SHORT_STRING_THRESHOLD) {
                        char stack_buf[XR_SHORT_STRING_THRESHOLD];
                        memcpy(stack_buf, str_b->data, str_b->length);
                        memcpy(stack_buf + str_b->length, str_c->data, str_c->length);
                        R(a) = xr_string_value(xr_string_intern(isolate, stack_buf, total_len, 0));
                        vmbreak;
                    }
                    XrStrBuf *sb = xr_strbuf_tmp(isolate);
                    xr_strbuf_append_str(sb, str_b);
                    xr_strbuf_append_str(sb, str_c);
                    R(a) = xr_string_value(xr_strbuf_to_string(sb));
                    vmbreak;
                }
                // TypeError: incompatible operand types for '+'
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                    "operator '+' requires both operands to be numeric or both string, got '%s' and '%s'",
                    xr_typeid_name(xr_value_typeid(vb)), xr_typeid_name(xr_value_typeid(vc)));
                vmbreak;
            }

            vmcase(OP_ADDI) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int sc = GETARG_sC(i);
                XrValue vb = R(b);

                if (XR_IS_INT(vb)) {
                    XR_SET_INT(R(a), (int64_t)((uint64_t)XR_TO_INT(vb) + (uint64_t)(int64_t)sc));
                } else if (XR_IS_FLOAT(vb)) {
                    XR_SET_FLOAT(R(a), vb.f + (double)sc);
                } else if (XR_IS_BIGINT(vb)) {
                    R(a) = vm_bigint_binop(VM_CURRENT_CORO, vb, xr_int(sc), xr_bigint_add);
                } else {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "addition requires numeric types");
                }
                vmbreak;
            }

            vmcase(OP_ADDK) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue vb = R(b);
                XrValue kc = k[c];

                // BigInt + constant mixed
                if (XR_IS_BIGINT(vb) || XR_IS_BIGINT(kc)) {
                    R(a) = vm_bigint_binop(VM_CURRENT_CORO, vb, kc, xr_bigint_add);
                    vmbreak;
                }

                if (XR_IS_INT(vb) && XR_IS_INT(kc)) {
                    XR_SET_INT(R(a), (int64_t)((uint64_t)XR_TO_INT(vb) + (uint64_t)XR_TO_INT(kc)));
                } else {
                    double nb = XR_IS_INT(vb) ? (double)XR_TO_INT(vb) : XR_TO_FLOAT(vb);
                    double nc = XR_IS_INT(kc) ? (double)XR_TO_INT(kc) : XR_TO_FLOAT(kc);
                    R(a) = xr_float(nb + nc);
                }
                vmbreak;
            }

            vmcase(OP_SUB) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue vb = R(b);
                XrValue vc = R(c);

                // Fast path: integer subtraction (wrap on overflow)
                if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
                    XR_SET_INT(R(a), (int64_t)((uint64_t)XR_TO_INT(vb) - (uint64_t)XR_TO_INT(vc)));
                    vmbreak;
                }
                // Fast path: float - float
                if (XR_IS_FLOAT(vb) && XR_IS_FLOAT(vc)) {
                    XR_SET_FLOAT(R(a), vb.f - vc.f);
                    vmbreak;
                }
                // Mixed/extended numeric subtraction
                {
                    double nb = 0, nc = 0;
                    if (XR_TONUMBER(vb, nb) && XR_TONUMBER(vc, nc)) {
                        XR_SET_FLOAT(R(a), nb - nc);
                        vmbreak;
                    }
                }
                // BigInt subtraction (auto-promote int operands)
                if (XR_IS_BIGINT(vb) || XR_IS_BIGINT(vc)) {
                    R(a) = vm_bigint_binop(VM_CURRENT_CORO, vb, vc, xr_bigint_sub);
                    vmbreak;
                }
                // Operator overload (unified macro)
                VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, XR_OP_SUB_FLAG, SYMBOL_OP_SUB, "-");
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "subtraction requires numeric types");
            }

            vmcase(OP_SUBI) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int sc = GETARG_sC(i);
                XrValue vb = R(b);

                if (XR_IS_INT(vb)) {
                    XR_SET_INT(R(a), (int64_t)((uint64_t)XR_TO_INT(vb) - (uint64_t)(int64_t)sc));
                } else if (XR_IS_FLOAT(vb)) {
                    XR_SET_FLOAT(R(a), vb.f - (double)sc);
                } else if (XR_IS_BIGINT(vb)) {
                    R(a) = vm_bigint_binop(VM_CURRENT_CORO, vb, xr_int(sc), xr_bigint_sub);
                } else {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "subtraction requires numeric types");
                }
                vmbreak;
            }

            vmcase(OP_SUBK) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue vb = R(b);
                XrValue kc = k[c];

                // BigInt - constant mixed
                if (XR_IS_BIGINT(vb) || XR_IS_BIGINT(kc)) {
                    R(a) = vm_bigint_binop(VM_CURRENT_CORO, vb, kc, xr_bigint_sub);
                    vmbreak;
                }

                if (XR_IS_INT(vb) && XR_IS_INT(kc)) {
                    XR_SET_INT(R(a), (int64_t)((uint64_t)XR_TO_INT(vb) - (uint64_t)XR_TO_INT(kc)));
                } else {
                    double nb = XR_IS_INT(vb) ? (double)XR_TO_INT(vb) : XR_TO_FLOAT(vb);
                    double nc = XR_IS_INT(kc) ? (double)XR_TO_INT(kc) : XR_TO_FLOAT(kc);
                    XR_SET_FLOAT(R(a), nb - nc);
                }
                vmbreak;
            }

            vmcase(OP_MUL) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue vb = R(b);
                XrValue vc = R(c);

                // Fast path: integer multiplication (wrap on overflow)
                if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
                    XR_SET_INT(R(a), (int64_t)((uint64_t)XR_TO_INT(vb) * (uint64_t)XR_TO_INT(vc)));
                    vmbreak;
                }
                // Fast path: float * float
                if (XR_IS_FLOAT(vb) && XR_IS_FLOAT(vc)) {
                    XR_SET_FLOAT(R(a), vb.f * vc.f);
                    vmbreak;
                }
                // Mixed/extended numeric multiplication
                {
                    double nb = 0, nc = 0;
                    if (XR_TONUMBER(vb, nb) && XR_TONUMBER(vc, nc)) {
                        XR_SET_FLOAT(R(a), nb * nc);
                        vmbreak;
                    }
                }
                // BigInt multiplication (auto-promote int operands)
                if (XR_IS_BIGINT(vb) || XR_IS_BIGINT(vc)) {
                    R(a) = vm_bigint_binop(VM_CURRENT_CORO, vb, vc, xr_bigint_mul);
                    vmbreak;
                }
                // String repeat: string * int or int * string
                if (XR_IS_STRING(vb) && XR_IS_INT(vc)) {
                    XrString *str = xr_value_to_string(isolate, vb);
                    xr_Integer count = XR_TO_INT(vc);
                    XrString *result = xr_string_repeat(isolate, str, count);
                    R(a) = result ? xr_string_value(result) : xr_null();
                    vmbreak;
                }
                if (XR_IS_INT(vb) && XR_IS_STRING(vc)) {
                    xr_Integer count = XR_TO_INT(vb);
                    XrString *str = xr_value_to_string(isolate, vc);
                    XrString *result = xr_string_repeat(isolate, str, count);
                    R(a) = result ? xr_string_value(result) : xr_null();
                    vmbreak;
                }
                // Operator overload (unified macro)
                VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, XR_OP_MUL_FLAG, SYMBOL_OP_MUL, "*");
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "multiplication requires numeric types");
            }

            vmcase(OP_MULI) {
                // MULI A B sC: R[A] = R[B] * sC
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int sc = GETARG_sC(i);
                XrValue vb = R(b);

                if (XR_IS_INT(vb)) {
                    XR_SET_INT(R(a), (int64_t)((uint64_t)XR_TO_INT(vb) * (uint64_t)(int64_t)sc));
                    vmbreak;
                }
                if (XR_IS_FLOAT(vb)) {
                    XR_SET_FLOAT(R(a), vb.f * (double)sc);
                    vmbreak;
                }
                if (XR_IS_BIGINT(vb)) {
                    R(a) = vm_bigint_binop(VM_CURRENT_CORO, vb, xr_int(sc), xr_bigint_mul);
                    vmbreak;
                }
                // Operator overload: convert immediate to XrValue
                {
                    XrValue vc = xr_int(sc);
                    VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, XR_OP_MUL_FLAG, SYMBOL_OP_MUL, "*");
                }
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "multiplication requires numeric types");
            }

            vmcase(OP_MULK) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue vb = R(b);
                XrValue vc = k[c];

                // BigInt * constant mixed
                if (XR_IS_BIGINT(vb) || XR_IS_BIGINT(vc)) {
                    R(a) = vm_bigint_binop(VM_CURRENT_CORO, vb, vc, xr_bigint_mul);
                    vmbreak;
                }

                // Fast path: integer multiplication (wrap on overflow)
                if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
                    XR_SET_INT(R(a), (int64_t)((uint64_t)XR_TO_INT(vb) * (uint64_t)XR_TO_INT(vc)));
                    vmbreak;
                }
                // Float/mixed multiplication
                {
                    double nb = 0, nc = 0;
                    if (XR_TONUMBER(vb, nb) && XR_TONUMBER(vc, nc)) {
                        XR_SET_FLOAT(R(a), nb * nc);
                        vmbreak;
                    }
                }
                // Operator overload
                VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, XR_OP_MUL_FLAG, SYMBOL_OP_MUL, "*");
                R(a) = xr_null();
                vmbreak;
            }

            vmcase(OP_DIV) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue vb = R(b);
                XrValue vc = R(c);

                // BigInt division (auto-promote, with zero check)
                if (XR_IS_BIGINT(vb) || XR_IS_BIGINT(vc)) {
                    R(a) = vm_bigint_divop(VM_CURRENT_CORO, vb, vc, xr_bigint_div);
                    if (unlikely(XR_IS_NOTFOUND(R(a)))) {
                        VM_RUNTIME_ERROR(XR_ERR_DIV_BY_ZERO, "division by zero");
                    }
                    vmbreak;
                }

                // Fast path: int / int → int (type determines result)
                if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
                    xr_Integer nb = XR_TO_INT(vb);
                    xr_Integer nc = XR_TO_INT(vc);
                    if (nc == 0) {
                        VM_RUNTIME_ERROR(XR_ERR_DIV_BY_ZERO, "division by zero");
                    }
                    R(a) = xr_int(nb / nc);
                    vmbreak;
                }

                // Mixed or float: promote to float
                if ((XR_IS_INT(vb) || XR_IS_FLOAT(vb)) &&
                    (XR_IS_INT(vc) || XR_IS_FLOAT(vc))) {
                    double nb = XR_IS_INT(vb) ? (double)XR_TO_INT(vb) : XR_TO_FLOAT(vb);
                    double nc = XR_IS_INT(vc) ? (double)XR_TO_INT(vc) : XR_TO_FLOAT(vc);
                    if (nc == 0.0) {
                        VM_RUNTIME_ERROR(XR_ERR_DIV_BY_ZERO, "division by zero");
                    }
                    R(a) = xr_float(nb / nc);
                    vmbreak;
                }

                // Operator overload (unified macro)
                VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, XR_OP_DIV_FLAG, SYMBOL_OP_DIV, "/");
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "division requires numeric types");
            }

            vmcase(OP_DIVK) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue vb = R(b);
                XrValue vc = k[c];

                // BigInt / constant mixed
                if (XR_IS_BIGINT(vb) || XR_IS_BIGINT(vc)) {
                    R(a) = vm_bigint_divop(VM_CURRENT_CORO, vb, vc, xr_bigint_div);
                    if (unlikely(XR_IS_NOTFOUND(R(a)))) {
                        VM_RUNTIME_ERROR(XR_ERR_DIV_BY_ZERO, "division by zero");
                    }
                    vmbreak;
                }

                // int / int → int (type determines result)
                if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
                    xr_Integer nb = XR_TO_INT(vb);
                    xr_Integer nc = XR_TO_INT(vc);
                    if (nc == 0) {
                        VM_RUNTIME_ERROR(XR_ERR_DIV_BY_ZERO, "division by zero");
                    }
                    R(a) = xr_int(nb / nc);
                } else if ((XR_IS_INT(vb) || XR_IS_FLOAT(vb)) && (XR_IS_INT(vc) || XR_IS_FLOAT(vc))) {
                    // Mixed or float: promote to float
                    double nb = XR_IS_INT(vb) ? (double)XR_TO_INT(vb) : XR_TO_FLOAT(vb);
                    double nc = XR_IS_INT(vc) ? (double)XR_TO_INT(vc) : XR_TO_FLOAT(vc);
                    if (nc == 0.0) {
                        VM_RUNTIME_ERROR(XR_ERR_DIV_BY_ZERO, "division by zero");
                    }
                    R(a) = xr_float(nb / nc);
                } else {
                    // Operator overload
                    VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, XR_OP_DIV_FLAG, SYMBOL_OP_DIV, "/");
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "division requires numeric types");
                }
                vmbreak;
            }

            vmcase(OP_MOD) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue vb = R(b);
                XrValue vc = R(c);

                // Fast path: integer modulo
                if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
                    xr_Integer divisor = XR_TO_INT(vc);
                    if (divisor == 0) {
                        VM_RUNTIME_ERROR(XR_ERR_MOD_BY_ZERO, "modulo by zero");
                    }
                    R(a) = xr_int(XR_TO_INT(vb) % divisor);
                    vmbreak;
                }

                // BigInt modulo (auto-promote, with zero check)
                if (XR_IS_BIGINT(vb) || XR_IS_BIGINT(vc)) {
                    R(a) = vm_bigint_divop(VM_CURRENT_CORO, vb, vc, xr_bigint_mod);
                    if (unlikely(XR_IS_NOTFOUND(R(a)))) {
                        VM_RUNTIME_ERROR(XR_ERR_MOD_BY_ZERO, "modulo by zero");
                    }
                    vmbreak;
                }

                // Operator overload (unified macro)
                VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, XR_OP_MOD_FLAG, SYMBOL_OP_MOD, "%%");
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "modulo requires integer types");
            }

            vmcase(OP_MODK) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue vb = R(b);
                XrValue vc = k[c];

                if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
                    xr_Integer divisor = XR_TO_INT(vc);
                    if (divisor == 0) {
                        VM_RUNTIME_ERROR(XR_ERR_MOD_BY_ZERO, "modulo by zero");
                    }
                    R(a) = xr_int(XR_TO_INT(vb) % divisor);
                } else {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "modulo requires integer types");
                }
                vmbreak;
            }

            vmcase(OP_UNM) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue vb = R(b);

                // Fast path: numeric negation (wrap on overflow)
                if (XR_IS_INT(vb)) {
                    XR_SET_INT(R(a), (int64_t)(-(uint64_t)XR_TO_INT(vb)));
                    vmbreak;
                }
                if (XR_IS_FLOAT(vb)) {
                    R(a) = xr_float(-XR_TO_FLOAT(vb));
                    vmbreak;
                }

                // BigInt negation
                if (XR_IS_BIGINT(vb)) {
                    XrBigInt *bigint = (XrBigInt*)XR_TO_PTR(vb);
                    XrBigInt *result = xr_bigint_neg(VM_CURRENT_CORO, bigint);
                    R(a) = XR_FROM_PTR(result);
                    vmbreak;
                }

                // Operator overload: unary minus uses "-" symbol
                if (xr_value_is_instance(vb)) {
                    XrInstance *inst_obj = xr_value_to_instance(vb);
                    XrClass *cls = xr_instance_get_class(inst_obj);
                    XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
                    int op_sym = xr_symbol_register_in_table(sym_table, "-");
                    XrMethod *op_method = xr_class_lookup_method(cls, op_sym);

                    if (op_method != NULL && op_method->type == XMETHOD_OPERATOR && op_method->as.closure != NULL) {
                        XrClosure *closure = op_method->as.closure;
                        XrProto *proto = closure->proto;

                        if (1 != proto->numparams) {
                            VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "unary operator - takes no arguments");
                        }
                        if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                            VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
                        }

                        R(a + 1) = vb; // this
                        savepc();
                        int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                        XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                        new_frame->closure = closure;
                        new_frame->pc = PROTO_CODE_BASE(proto);
                        new_frame->base_offset = (int)((base + a + 1) - VM_STACK);
                        goto startfunc;
                    }
                }

                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "operand must be numeric");
            }

            vmcase(OP_NOT) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue vb = R(b);

                // Fast path: boolean negation
                if (XR_IS_BOOL(vb)) {
                    R(a) = xr_bool(!XR_TO_BOOL(vb));
                    vmbreak;
                }

                // Operator overload: logical not uses "!" symbol
                if (xr_value_is_instance(vb)) {
                    XrInstance *inst_obj = xr_value_to_instance(vb);
                    XrClass *cls = xr_instance_get_class(inst_obj);
                    XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
                    int op_sym = xr_symbol_register_in_table(sym_table, "!");
                    XrMethod *op_method = xr_class_lookup_method(cls, op_sym);

                    if (op_method != NULL && op_method->type == XMETHOD_OPERATOR && op_method->as.closure != NULL) {
                        XrClosure *closure = op_method->as.closure;
                        XrProto *proto = closure->proto;

                        if (1 != proto->numparams) {
                            VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "unary operator ! takes no arguments");
                        }
                        if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                            VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
                        }

                        R(a + 1) = vb; // this
                        savepc();
                        int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                        XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                        new_frame->closure = closure;
                        new_frame->pc = PROTO_CODE_BASE(proto);
                        new_frame->base_offset = (int)((base + a + 1) - VM_STACK);
                        goto startfunc;
                    }
                }

                // Default: negate truthiness
                R(a) = xr_bool(!xr_vm_is_truthy(vb));
                vmbreak;
            }

            // Box/Unbox: typed storage (TypedArray/TypedField) ↔ tagged boundary
            // BOX creates tagged value from raw payload
            // UNBOX extracts raw payload with type check
            vmcase(OP_BOX_I64) {
                R(GETARG_A(i)) = XR_FROM_INT(R(GETARG_B(i)).i);
                vmbreak;
            }
            vmcase(OP_BOX_F64) {
                R(GETARG_A(i)) = XR_FROM_FLOAT(R(GETARG_B(i)).f);
                vmbreak;
            }
            vmcase(OP_UNBOX_I64) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue src = R(b);
                if (XR_IS_INT(src)) {
                    XR_SET_INT(R(a), XR_TO_INT(src));
                } else if (XR_IS_FLOAT(src)) {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                        "cannot implicitly convert float to int (use int() for explicit conversion)");
                } else {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                        "cannot assign non-int value to int variable");
                }
                vmbreak;
            }
            vmcase(OP_UNBOX_F64) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue src = R(b);
                if (XR_IS_FLOAT(src)) {
                    R(a).f = XR_TO_FLOAT(src);
                    R(a).tag = XR_TAG_F64;
                } else if (XR_IS_INT(src)) {
                    // int → float promotion (allowed)
                    R(a).f = (double)XR_TO_INT(src);
                    R(a).tag = XR_TAG_F64;
                } else {
                    // non-numeric → float is not allowed (second gate: runtime)
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                        "cannot implicitly convert non-numeric value to float");
                }
                vmbreak;
            }

            vmcase(OP_ARRAY_GET_NOCHECK) {
                // Array access without bounds check (compiler proved index is valid)
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrArray *arr = XR_TO_ARRAY(R(b));
                int idx = (int)XR_TO_INT(R(c));
                R(a) = (arr->elem_type == XR_ELEM_ANY)
                    ? ((XrValue*)arr->data)[idx]
                    : xr_array_get_element(arr, idx);
                vmbreak;
            }

            /* === StringBuilder Instructions === */

            vmcase(OP_STRBUF_NEW) {
                int a = GETARG_A(i);

                XrStringBuilder *sb = xr_stringbuilder_new(VM_CURRENT_CORO);
                R(a) = xr_stringbuilder_value(sb);
                checkGC(base + a + 1);
                vmbreak;
            }

            vmcase(OP_STRBUF_APPEND) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);

                XrStringBuilder *sb = xr_to_stringbuilder(R(a));

                XrValue vb = R(b);
                if (XR_IS_STRING(vb)) {
                    xr_stringbuilder_append_cstr(sb, xr_value_str_data(&vb), xr_value_str_len(&vb));
                } else if (XR_IS_INT(vb)) {
                    xr_stringbuilder_append_int(sb, XR_TO_INT(vb));
                } else if (XR_IS_FLOAT(vb)) {
                    xr_stringbuilder_append_float(sb, XR_TO_FLOAT(vb));
                } else {
                    XrString *str = xr_value_to_string(isolate, vb);
                    xr_stringbuilder_append_str(sb, str);
                }
                vmbreak;
            }

            vmcase(OP_STRBUF_FINISH) {
                int a = GETARG_A(i);

                XrStringBuilder *sb = xr_to_stringbuilder(R(a));
                XrString *result = xr_stringbuilder_to_string(sb);

                R(a) = xr_string_value(result);
                checkGC(base + a + 1);
                vmbreak;
            }

            /* ========================================================
            ** Bitwise Operations
            ** ======================================================== */

            vmcase(OP_BAND) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue vb = R(b);
                XrValue vc = R(c);

                // Fast path: integer bitwise operation
                if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
                    R(a) = xr_int(XR_TO_INT(vb) & XR_TO_INT(vc));
                    vmbreak;
                }
                // BigInt bitwise AND
                if (XR_IS_BIGINT(vb) && XR_IS_BIGINT(vc)) {
                    XrBigInt *ba = (XrBigInt*)XR_TO_PTR(vb);
                    XrBigInt *bb = (XrBigInt*)XR_TO_PTR(vc);
                    XrBigInt *result = xr_bigint_and(VM_CURRENT_CORO, ba, bb);
                    R(a) = XR_FROM_PTR(result);
                    vmbreak;
                }

                // Operator overload
                if (xr_value_is_instance(vb)) {
                    XrInstance *inst_obj = xr_value_to_instance(vb);
                    XrClass *cls = xr_instance_get_class(inst_obj);
                    if (XCLASS_HAS_OP(cls, XR_OP_BAND_FLAG)) {
                        XrMethod *op_method = xr_class_lookup_method(cls, SYMBOL_OP_BAND);

                        if (op_method != NULL && op_method->type == XMETHOD_OPERATOR && op_method->as.closure != NULL) {
                            XrClosure *closure = op_method->as.closure;
                            XrProto *proto = closure->proto;

                            if (1 + 1 != proto->numparams) {
                                VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "operator & requires 1 argument");
                            }
                            if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
                            }

                            R(a + 1) = vb;
                            R(a + 2) = vc;
                            savepc();
                            int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                            XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                            new_frame->closure = closure;
                            new_frame->pc = PROTO_CODE_BASE(proto);
                            new_frame->base_offset = (int)((base + a + 1) - VM_STACK);
                            goto startfunc;
                        }
                    }
                }

                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "bitwise AND requires integer types");
            }

            vmcase(OP_BOR) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue vb = R(b);
                XrValue vc = R(c);

                // Fast path: integer bitwise operation
                if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
                    R(a) = xr_int(XR_TO_INT(vb) | XR_TO_INT(vc));
                    vmbreak;
                }
                // BigInt bitwise OR
                if (XR_IS_BIGINT(vb) && XR_IS_BIGINT(vc)) {
                    XrBigInt *ba = (XrBigInt*)XR_TO_PTR(vb);
                    XrBigInt *bb = (XrBigInt*)XR_TO_PTR(vc);
                    XrBigInt *result = xr_bigint_or(VM_CURRENT_CORO, ba, bb);
                    R(a) = XR_FROM_PTR(result);
                    vmbreak;
                }

                // Operator overload
                if (xr_value_is_instance(vb)) {
                    XrInstance *inst_obj = xr_value_to_instance(vb);
                    XrClass *cls = xr_instance_get_class(inst_obj);
                    if (XCLASS_HAS_OP(cls, XR_OP_BOR_FLAG)) {
                        XrMethod *op_method = xr_class_lookup_method(cls, SYMBOL_OP_BOR);

                        if (op_method != NULL && op_method->type == XMETHOD_OPERATOR && op_method->as.closure != NULL) {
                            XrClosure *closure = op_method->as.closure;
                            XrProto *proto = closure->proto;

                            if (1 + 1 != proto->numparams) {
                                VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "operator | requires 1 argument");
                            }
                            if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
                            }

                            R(a + 1) = vb;
                            R(a + 2) = vc;
                            savepc();
                            int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                            XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                            new_frame->closure = closure;
                            new_frame->pc = PROTO_CODE_BASE(proto);
                            new_frame->base_offset = (int)((base + a + 1) - VM_STACK);
                            goto startfunc;
                        }
                    }
                }

                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "bitwise OR requires integer types");
            }

            vmcase(OP_BXOR) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue vb = R(b);
                XrValue vc = R(c);

                // Fast path: integer bitwise operation
                if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
                    R(a) = xr_int(XR_TO_INT(vb) ^ XR_TO_INT(vc));
                    vmbreak;
                }
                // BigInt bitwise XOR
                if (XR_IS_BIGINT(vb) && XR_IS_BIGINT(vc)) {
                    XrBigInt *ba = (XrBigInt*)XR_TO_PTR(vb);
                    XrBigInt *bb = (XrBigInt*)XR_TO_PTR(vc);
                    XrBigInt *result = xr_bigint_xor(VM_CURRENT_CORO, ba, bb);
                    R(a) = XR_FROM_PTR(result);
                    vmbreak;
                }

                // Operator overload
                if (xr_value_is_instance(vb)) {
                    XrInstance *inst_obj = xr_value_to_instance(vb);
                    XrClass *cls = xr_instance_get_class(inst_obj);
                    if (XCLASS_HAS_OP(cls, XR_OP_BXOR_FLAG)) {
                        XrMethod *op_method = xr_class_lookup_method(cls, SYMBOL_OP_BXOR);

                        if (op_method != NULL && op_method->type == XMETHOD_OPERATOR && op_method->as.closure != NULL) {
                            XrClosure *closure = op_method->as.closure;
                            XrProto *proto = closure->proto;

                            if (1 + 1 != proto->numparams) {
                                VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "operator ^ requires 1 argument");
                            }
                            if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
                            }

                            R(a + 1) = vb;
                            R(a + 2) = vc;
                            savepc();
                            int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                            XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                            new_frame->closure = closure;
                            new_frame->pc = PROTO_CODE_BASE(proto);
                            new_frame->base_offset = (int)((base + a + 1) - VM_STACK);
                            goto startfunc;
                        }
                    }
                }

                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "bitwise XOR requires integer types");
            }

            vmcase(OP_BNOT) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue vb = R(b);

                // Fast path: integer bitwise NOT
                if (XR_IS_INT(vb)) {
                    R(a) = xr_int(~XR_TO_INT(vb));
                    vmbreak;
                }

                // Operator overload: bitwise NOT uses "~" symbol
                if (xr_value_is_instance(vb)) {
                    XrInstance *inst_obj = xr_value_to_instance(vb);
                    XrClass *cls = xr_instance_get_class(inst_obj);
                    XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
                    int op_sym = xr_symbol_register_in_table(sym_table, "~");
                    XrMethod *op_method = xr_class_lookup_method(cls, op_sym);

                    if (op_method != NULL && op_method->type == XMETHOD_OPERATOR && op_method->as.closure != NULL) {
                        XrClosure *closure = op_method->as.closure;
                        XrProto *proto = closure->proto;

                        if (1 != proto->numparams) {
                            VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "unary operator ~ takes no arguments");
                        }
                        if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                            VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
                        }

                        R(a + 1) = vb;
                        savepc();
                        int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                        XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                        new_frame->closure = closure;
                        new_frame->pc = PROTO_CODE_BASE(proto);
                        new_frame->base_offset = (int)((base + a + 1) - VM_STACK);
                        goto startfunc;
                    }
                }

                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "bitwise NOT requires integer type");
            }

            vmcase(OP_SHL) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue vb = R(b);
                XrValue vc = R(c);

                // Fast path: integer left shift
                if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
                    xr_Integer shift = XR_TO_INT(vc);
                    if (shift >= 0 && shift < XR_INT64_BITS) {
                        XR_SET_INT(R(a), XR_TO_INT(vb) << shift);
                    } else {
                        R(a) = xr_int(0);
                    }
                    vmbreak;
                }
                // BigInt left shift
                if (XR_IS_BIGINT(vb) && XR_IS_INT(vc)) {
                    XrBigInt *ba = (XrBigInt*)XR_TO_PTR(vb);
                    uint32_t shift = (uint32_t)XR_TO_INT(vc);
                    XrBigInt *result = xr_bigint_shl(VM_CURRENT_CORO, ba, shift);
                    R(a) = XR_FROM_PTR(result);
                    vmbreak;
                }

                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "shift operation requires integer types");
            }

            vmcase(OP_SHR) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue vb = R(b);
                XrValue vc = R(c);

                // Fast path: integer right shift
                if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
                    xr_Integer shift = XR_TO_INT(vc);
                    if (shift >= 0 && shift < XR_INT64_BITS) {
                        R(a) = xr_int(XR_TO_INT(vb) >> shift);
                    } else {
                        R(a) = xr_int(0);  // Shift out of range returns 0
                    }
                    vmbreak;
                }
                // BigInt right shift
                if (XR_IS_BIGINT(vb) && XR_IS_INT(vc)) {
                    XrBigInt *ba = (XrBigInt*)XR_TO_PTR(vb);
                    uint32_t shift = (uint32_t)XR_TO_INT(vc);
                    XrBigInt *result = xr_bigint_shr(VM_CURRENT_CORO, ba, shift);
                    R(a) = XR_FROM_PTR(result);
                    vmbreak;
                }

                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "shift operation requires integer types");
            }

            /* ========================================================
            ** Comparison and Jump Instructions
            ** ======================================================== */

            vmcase(OP_EQ) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int k_flag = GETARG_C(i);
                if (vm_values_equal(R(a), R(b)) != k_flag) pc++;
                vmbreak;
            }

            vmcase(OP_EQK) {
                // OP_EQK: constant equality comparison
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int k_flag = GETARG_C(i);
                if (vm_values_equal(R(a), k[b]) != k_flag) pc++;
                vmbreak;
            }

            vmcase(OP_EQI) {
                // OP_EQI: immediate value equality comparison
                int a = GETARG_A(i);
                int sb = GETARG_sB(i);
                int k_flag = GETARG_C(i);
                XrValue imm_val = xr_int(sb);
                if (vm_values_equal(R(a), imm_val) != k_flag) pc++;
                vmbreak;
            }

// Register-register comparison macro (LE, GT, GE share identical structure)
#define VM_CMP_RR(op, int_op, float_op, str_cmp_op) \
    vmcase(op) { \
        XrValue va = R(GETARG_A(i)); \
        XrValue vb = R(GETARG_B(i)); \
        int k_flag = GETARG_C(i); \
        bool cond; \
        if (XR_IS_INT(va) && XR_IS_INT(vb)) { \
            cond = XR_TO_INT(va) int_op XR_TO_INT(vb); \
        } else if ((XR_IS_INT(va) || XR_IS_FLOAT(va)) && \
                   (XR_IS_INT(vb) || XR_IS_FLOAT(vb))) { \
            double na = XR_IS_INT(va) ? (double)XR_TO_INT(va) : XR_TO_FLOAT(va); \
            double nb = XR_IS_INT(vb) ? (double)XR_TO_INT(vb) : XR_TO_FLOAT(vb); \
            cond = na float_op nb; \
        } else if (XR_IS_STRING(va) && XR_IS_STRING(vb)) { \
            cond = xr_string_compare(xr_value_to_string(isolate, va), xr_value_to_string(isolate, vb)) str_cmp_op 0; \
        } else { \
            if (k_flag == 0) pc++; \
            vmbreak; \
        } \
        if (cond != k_flag) pc++; \
        vmbreak; \
    }

// Register-immediate comparison macro (LTI, LEI, GTI, GEI share identical structure)
#define VM_CMP_RI(op, int_op, float_op) \
    vmcase(op) { \
        XrValue va = R(GETARG_A(i)); \
        int sb = GETARG_sB(i); \
        int k_flag = GETARG_C(i); \
        bool cond; \
        if (XR_IS_INT(va)) { \
            cond = XR_TO_INT(va) int_op sb; \
        } else { \
            cond = XR_TO_FLOAT(va) float_op (double)sb; \
        } \
        if (cond != k_flag) pc++; \
        vmbreak; \
    }

            vmcase(OP_LT) {
                // LT A B k: if (R[A] < R[B]) != k then pc++
                XrValue va = R(GETARG_A(i));
                XrValue vb = R(GETARG_B(i));
                int k_flag = GETARG_C(i);

                // Fast path: int direct comparison
                if (XR_IS_INT(va) && XR_IS_INT(vb)) {
                    if ((XR_TO_INT(va) < XR_TO_INT(vb)) != k_flag) pc++;
                } else if ((XR_IS_INT(va) || XR_IS_FLOAT(va)) &&
                           (XR_IS_INT(vb) || XR_IS_FLOAT(vb))) {
                    double na = XR_IS_INT(va) ? (double)XR_TO_INT(va) : XR_TO_FLOAT(va);
                    double nb = XR_IS_INT(vb) ? (double)XR_TO_INT(vb) : XR_TO_FLOAT(vb);
                    if ((na < nb) != k_flag) pc++;
                } else if (XR_IS_STRING(va) && XR_IS_STRING(vb)) {
                    const char *da = xr_value_str_data(&va);
                    uint32_t la = xr_value_str_len(&va);
                    const char *db = xr_value_str_data(&vb);
                    uint32_t lb = xr_value_str_len(&vb);
                    uint32_t ml = la < lb ? la : lb;
                    int cmp = memcmp(da, db, ml);
                    if (cmp == 0) cmp = (la > lb) - (la < lb);
                    if ((cmp < 0) != k_flag) pc++;
                } else {
                    // Non-comparable types: treat as false
                    if (k_flag == 0) pc++;
                }
                vmbreak;
            }

            vmcase(OP_LTI) {
                // LTI A sB k: if (R[A] < sB) != k then pc++
                XrValue va = R(GETARG_A(i));
                int sb = GETARG_sB(i);
                int k_flag = GETARG_C(i);
                bool cond;

                if (XR_IS_INT(va)) {
                    cond = XR_TO_INT(va) < sb;
                } else {
                    cond = XR_TO_FLOAT(va) < (double)sb;
                }
                if (cond != k_flag) pc++;
                vmbreak;
            }

            VM_CMP_RR(OP_LE, <=, <=, <=)
            VM_CMP_RI(OP_LEI, <=, <=)

            /* ========================================================
            ** Comparison Instructions (Produce Boolean Value)
            ** ======================================================== */

            vmcase(OP_CMP_EQ) {
                // OP_CMP_EQ: equality comparison
                int dest = GETARG_A(i);
                int left = GETARG_B(i);
                int right = GETARG_C(i);
                // Fast path: primitive types
                if ((XR_IS_INT(R(left)) || XR_IS_FLOAT(R(left)) || XR_IS_BOOL(R(left)) || XR_IS_NULL(R(left))) &&
                    (XR_IS_INT(R(right)) || XR_IS_FLOAT(R(right)) || XR_IS_BOOL(R(right)) || XR_IS_NULL(R(right)))) {
                    R(dest) = xr_bool(vm_values_equal(R(left), R(right)));
                    vmbreak;
                }
                // Operator overload
                VM_TRY_BINARY_OP_OVERLOAD(R(left), R(right), dest, XR_OP_EQ_FLAG, SYMBOL_OP_EQ, "==");
                // Deep comparison
                R(dest) = xr_bool(vm_values_equal_deep(isolate, R(left), R(right)));
                vmbreak;
            }

            vmcase(OP_CMP_NE) {
                // OP_CMP_NE: inequality comparison
                int dest = GETARG_A(i);
                int left = GETARG_B(i);
                int right = GETARG_C(i);
                if ((XR_IS_INT(R(left)) || XR_IS_FLOAT(R(left)) || XR_IS_BOOL(R(left)) || XR_IS_NULL(R(left))) &&
                    (XR_IS_INT(R(right)) || XR_IS_FLOAT(R(right)) || XR_IS_BOOL(R(right)) || XR_IS_NULL(R(right)))) {
                    R(dest) = xr_bool(!vm_values_equal(R(left), R(right)));
                    vmbreak;
                }
                // Operator overload for != (uses == operator, negated)
                VM_TRY_BINARY_OP_OVERLOAD(R(left), R(right), dest, XR_OP_NE_FLAG, SYMBOL_OP_NE, "!=");
                R(dest) = xr_bool(!vm_values_equal_deep(isolate, R(left), R(right)));
                vmbreak;
            }

            vmcase(OP_CMP_EQ_STRICT) {
                // OP_CMP_EQ_STRICT: strict equality comparison
                int dest = GETARG_A(i);
                int left = GETARG_B(i);
                int right = GETARG_C(i);
                R(dest) = xr_bool(vm_values_equal(R(left), R(right)));
                vmbreak;
            }

            vmcase(OP_CMP_NE_STRICT) {
                // OP_CMP_NE_STRICT: strict inequality comparison
                int dest = GETARG_A(i);
                int left = GETARG_B(i);
                int right = GETARG_C(i);
                R(dest) = xr_bool(!vm_values_equal(R(left), R(right)));
                vmbreak;
            }

            vmcase(OP_CMP_LT) {
                // OP_CMP_LT: less than comparison
                int dest = GETARG_A(i);
                int left = GETARG_B(i);
                int right = GETARG_C(i);
                VM_TRY_BINARY_OP_OVERLOAD(R(left), R(right), dest, XR_OP_LT_FLAG, SYMBOL_OP_LT, "<");
                R(dest) = xr_bool(vm_numeric_less(R(left), R(right)));
                vmbreak;
            }

            vmcase(OP_CMP_LE) {
                // OP_CMP_LE: less than or equal comparison
                int dest = GETARG_A(i);
                int left = GETARG_B(i);
                int right = GETARG_C(i);
                VM_TRY_BINARY_OP_OVERLOAD(R(left), R(right), dest, XR_OP_LE_FLAG, SYMBOL_OP_LE, "<=");
                R(dest) = xr_bool(vm_numeric_less_equal(R(left), R(right)));
                vmbreak;
            }

            vmcase(OP_IS) {
                // OP_IS: runtime type check - R[A] = (R[B] is Type[C])
                int dest = GETARG_A(i);
                int src = GETARG_B(i);
                int type_idx = GETARG_C(i);
                XrValue val = R(src);

                // Get type info from constant pool
                XrValue type_val = K(type_idx);
                bool result = false;

                // Basic type checking based on value type
                XrTypeId val_tid = xr_value_typeid(val);
                if (XR_IS_INT(type_val)) {
                    int expected_type = (int)XR_TO_INT(type_val);
                    result = (val_tid == (XrTypeId)expected_type);
                }

                R(dest) = xr_bool(result);
                vmbreak;
            }

            vmcase(OP_CHECKTYPE) {
                /* OP_CHECKTYPE A B: assert R[A] matches type bitmask K(B).
                 * K(B) is a bitmask where bit[tid] = 1 for each allowed type.
                 * Single type: mask = (1 << tid).  Union: OR of member bits. */
                int src = GETARG_A(i);
                int type_idx = GETARG_B(i);
                XrValue val = R(src);
                XrValue type_val = K(type_idx);

                if (XR_IS_INT(type_val)) {
                    int64_t expected_mask = XR_TO_INT(type_val);
                    XrTypeId actual_tid = xr_value_typeid(val);
                    if (!((1LL << actual_tid) & expected_mask)) {
                        savepc();
                        // Build human-readable expected type list from bitmask
                        char expect_buf[128];
                        int pos = 0;
                        for (int tid = 0; tid < XR_TID_COUNT && pos < 110; tid++) {
                            if (!((1LL << tid) & expected_mask)) continue;
                            if (pos > 0) { expect_buf[pos++] = ' '; expect_buf[pos++] = '|'; expect_buf[pos++] = ' '; }
                            const char *n = xr_typeid_name((XrTypeId)tid);
                            int nl = (int)strlen(n);
                            if (pos + nl >= 120) { memcpy(expect_buf + pos, "...", 3); pos += 3; break; }
                            memcpy(expect_buf + pos, n, nl);
                            pos += nl;
                        }
                        expect_buf[pos] = '\0';
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                            "TypeError: expected '%s', got '%s'",
                            expect_buf, xr_typeid_name(actual_tid));
                    }
                }
                vmbreak;
            }

            vmcase(OP_ISNULL) {
                // ISNULL A k: if (R[A] == null) != k then pc++
                XrValue va = R(GETARG_A(i));
                int k_flag = GETARG_B(i);
                bool is_null = XR_IS_NULL(va);
                if (is_null != k_flag) pc++;
                vmbreak;
            }

            vmcase(OP_ISNULL_SET) {
                // ISNULL_SET A B: R[A] = (R[B] == null)
                int dest = GETARG_A(i);
                int src = GETARG_B(i);
                R(dest) = xr_bool(XR_IS_NULL(R(src)));
                vmbreak;
            }

            /* ========================================================
            ** Control Flow Instructions
            ** ======================================================== */

            vmcase(OP_JMP) {
                // JMP sJ: pc += sJ
                int offset = GETARG_sJ(i);

                /* Reductions check on backward jumps to prevent
                ** infinite loops from starving other coroutines.
                ** Performance impact < 3%, enables fair scheduling.
                ** Also serves as GC safe point for per-coroutine GC. */
                if (offset < 0 && vm_ctx && vm_ctx->current_coro) {
                    XrCoroutine *coro = (XrCoroutine *)vm_ctx->current_coro;

                    /* GC safe point: check and trigger GC at loop back-edge.
                    ** Stack is consistent here (between instructions). */
                    VM_GC_SAFEPOINT();

                    if (--coro->reductions <= 0) {
                        if (xr_coro_flags_has(coro, XR_CORO_FLG_CANCEL_REQUESTED)) {
                            return XR_VM_CANCELLED;
                        }
                        coro->reductions = XR_CORO_REDUCTIONS;
                        frame->pc = pc - 1;
                        return XR_VM_YIELD;
                    }

#ifdef XRAY_HAS_JIT
                    /* OSR: try entering JIT code at this loop header.
                     *
                     * Priority order (eliminates atomic contention):
                     *   1. jit_entry set → direct OSR (zero atomics)
                     *   2. bg compilation done → install + OSR
                     *   3. bg compilation in progress → skip (no fetch_add)
                     *   4. not yet queued → count via fetch_add, trigger at threshold
                     */
                    if (isolate->vm.jit) {
                        XrProto *_osr_proto = cl->proto;
                        bool _do_osr = false;

                        if (_osr_proto->jit_entry) {
                            // Already compiled: attempt OSR on every back-edge
                            _do_osr = true;
                        } else {
                            void *_pend = atomic_load_explicit(
                                &_osr_proto->jit_entry_pending, memory_order_acquire);
                            if (_pend && (uintptr_t)_pend > 1) {
                                // Background compilation done → install + try OSR
                                xir_jit_install_bg_result(_osr_proto);
                                _do_osr = (_osr_proto->jit_entry != NULL);
                            } else if (!_pend) {
                                // Not yet queued: count and trigger at threshold
                                if (atomic_fetch_add_explicit(&_osr_proto->exec_count,
                                        1, memory_order_relaxed) + 1
                                    == (uint32_t)isolate->vm.jit_threshold) {
                                    _do_osr = true;
                                }
                            }
                            // _pend == sentinel (0x1): compilation in progress, skip
                        }

                        if (_do_osr) {
                            uint32_t _target_pc = (uint32_t)(pc + offset - PROTO_CODE_BASE(_osr_proto));
                            coro->jit_ctx->call_closure = cl;
                            coro->jit_ctx->osr_deopt_pc = -1;
                            XrValue _osr_result;
                            int _osr_rc = xir_jit_osr_trigger(isolate->vm.jit, _osr_proto, coro,
                                                     _target_pc, base, _osr_proto->maxstacksize,
                                                     _osr_proto->return_type_info
                                                         ? xr_type_to_slot_type(_osr_proto->return_type_info) : XR_SLOT_ANY,
                                                     &_osr_result);
                            if (_osr_rc == XIR_JIT_OK) {
                                _osr_proto->osr_pending = false;
                                vm_ctx->last_nret = 1;
                                if (ci->base_offset > 0) {
                                    XrValue *ret_slot = (ci->call_status & XR_CALL_KEEP_FUNC)
                                        ? VM_STACK + ci->result_offset
                                        : VM_STACK + ci->base_offset - 1;
                                    *ret_slot = _osr_result;
                                }
                                VM_DEC_FRAME_COUNT;
                                if (VM_MODULE_BASE >= 0 && VM_FRAME_COUNT == VM_MODULE_BASE)
                                    return XR_VM_OK;
                                ci = &VM_FRAMES[VM_FRAME_COUNT - 1];
                                if (!ci->closure || !ci->closure->proto) return XR_VM_OK;
                                VM_SET_STACK_TOP(VM_STACK + ci->base_offset + ci->closure->proto->maxstacksize);
                                goto startfunc;
                            }
                            if (_osr_rc == XIR_JIT_SUSPEND) {
                                savepc();
                                return XR_VM_BLOCKED;
                            }
                            if (coro->jit_ctx->osr_deopt_pc >= 0) {
                                pc = PROTO_CODE_BASE(_osr_proto) + coro->jit_ctx->osr_deopt_pc;
                                coro->jit_ctx->osr_deopt_pc = -1;
                                offset = 0;
                            }
                        }
                    }
#endif
                }

                pc += offset;
                vmbreak;
            }

            vmcase(OP_TEST) {
                // TEST A k: if (bool(R[A])) != k then pc++
                XrValue va = R(GETARG_A(i));
                int k_flag = GETARG_B(i);
                bool truthy = vm_is_truthy(va);
                if (truthy != k_flag) pc++;
                vmbreak;
            }

            vmcase(OP_TESTSET) {
                /* OP_TESTSET: logical && / || always returns bool.
                 * Previously returned the original operand.
                 * Now returns xr_bool() for type consistency. */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int k_flag = GETARG_C(i);
                bool truthy = vm_is_truthy(R(b));
                if (truthy != k_flag) {
                    pc++; // Skip next instruction
                } else {
                    R(a) = xr_bool(truthy);
                }
                vmbreak;
            }

            vmcase(OP_GETBUILTIN) {
                // OP_GETBUILTIN: read-only builtin global O(1)
                int a = GETARG_A(i);
                int builtin_index = GETARG_Bx(i);

                if (builtin_index < isolate->vm.builtin_count) {
                    R(a) = isolate->vm.builtins[builtin_index];
                } else {
                    R(a) = xr_null();
                }
                vmbreak;
            }

            vmcase(OP_GETSHARED) {
                // OP_GETSHARED: get shared variable O(1)
                int a = GETARG_A(i);
                int shared_index = GETARG_Bx(i) + cl->proto->shared_offset;
                R(a) = xr_shared_array_get(&isolate->vm.shared, shared_index);

                vmbreak;
            }

            vmcase(OP_SETSHARED) {
                // OP_SETSHARED: set shared variable O(1)
                int a = GETARG_A(i);
                int shared_index = GETARG_Bx(i) + cl->proto->shared_offset;

                // Decref old value if it's a shared object
                XrValue old_val = xr_shared_array_get(&isolate->vm.shared, shared_index);
                if (XR_IS_PTR(old_val)) {
                    XrGCHeader *old_obj = XR_VALUE_GCPTR(old_val);
                    if (old_obj && XR_GC_IS_SHARED(old_obj)) {
                        int new_refc = xr_shared_decref(old_obj);
                        if (new_refc == 0) {
                            xr_shared_destroy(old_obj);
                        }
                    }
                }

                xr_shared_array_set(&isolate->vm.shared, shared_index, R(a));
                vmbreak;
            }

            vmcase(OP_CLOSURE) {
                /* OP_CLOSURE: create closure, populate upvals[] from proto descriptors.
                ** All captures are BY_VALUE: const → raw value, let → cell ref.
                ** Sources: SRC_REG (from register) or SRC_UPVAL (from enclosing upvals[]). */
                int a = GETARG_A(i);
                int bx = GETARG_Bx(i);
                XrProto *proto = PROTO_PROTO(cl->proto, bx);
                XrClosure *closure = xr_closure_new(isolate, proto,
                    (XrCoroutine *)vm_ctx->current_coro);
                int nuv = DYNARRAY_COUNT(&proto->upvalues);
                for (int j = 0; j < nuv; j++) {
                    UpvalInfo *uv = &DYNARRAY_GET(&proto->upvalues, j, UpvalInfo);
                    if (uv->source == UPVAL_SRC_REG) {
                        closure->upvals[j] = R(uv->index);
                    } else if (uv->source == UPVAL_SRC_UPVAL) {
                        int idx = uv->index;
                        closure->upvals[j] = (idx < cl->upval_count) ? cl->upvals[idx] : xr_null();
                    } else {
                        closure->upvals[j] = xr_null(); // fallback
                    }
                }
                R(a) = xr_value_from_closure(closure);
                checkGC(base + a + 1);
                vmbreak;
            }

            vmcase(OP_UPVAL_GET) {
                /* OP_UPVAL_GET: R[A] = cl->upvals[B]
                ** Flat upvalue read from current closure's upvals array.
                ** Used for BY_VALUE captured variables (const, loop vars). */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                R(a) = (b < cl->upval_count) ? cl->upvals[b] : xr_null();
                vmbreak;
            }

            vmcase(OP_CELL_NEW) {
                /* OP_CELL_NEW: R[A] = new_cell(R[A])
                ** Wraps current register value in a heap-allocated XrCell (32B).
                ** Used for captured mutable let vars. */
                int a = GETARG_A(i);
                XrCell *cell = xr_cell_new(isolate,
                    (XrCoroutine *)vm_ctx->current_coro);
                cell->value = R(a);
                R(a) = xr_make_ptr_val(cell);
                checkGC(base + a + 1);
                vmbreak;
            }

            vmcase(OP_CELL_GET) {
                /* OP_CELL_GET: R[A] = cell_deref(R[B])
                ** Read value from XrCell. */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrCell *cell = (XrCell *)R(b).ptr;
                R(a) = cell ? cell->value : xr_null();
                vmbreak;
            }

            vmcase(OP_CELL_SET) {
                /* OP_CELL_SET: cell_store(R[A], R[B])
                ** Write value into XrCell. */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrCell *cell = (XrCell *)R(a).ptr;
                if (cell) cell->value = R(b);
                vmbreak;
            }

            vmcase(OP_PRINT) {
                /* OP_PRINT: print value with toString support
                ** A: value register
                ** B: 1=add space before (not first argument)
                ** C: bit0=newline, bit1-2=slot_type hint (0=ANY, 1=I64, 2=F64)
                **
                ** If value is instance with toString() method, call it first
                */
                int a = GETARG_A(i);
                int add_space = GETARG_B(i);
                int c_field = GETARG_C(i);
                int newline = c_field & 1;
                int slot_hint = (c_field >> 1) & 3;

                // Reconstruct tagged value from raw slot if hint provided
                XrValue val;
                if (slot_hint == 1) {
                    val = XR_FROM_INT(R(a).i);
                } else if (slot_hint == 2) {
                    val = XR_FROM_FLOAT(R(a).f);
                } else {
                    val = R(a);
                }

                if (add_space) printf(" ");

                // Check if instance has toString method
                if (xr_value_is_instance(val)) {
                    XrInstance *inst = xr_value_to_instance(val);
                    XrClass *cls = xr_instance_get_class(inst);
                    if (cls) {
                        XrMethod *method = xr_class_lookup_method(cls, SYMBOL_TOSTRING);
                        if (method && method->type == XMETHOD_CLOSURE && method->as.closure) {
                            // Call toString() method
                            XrClosure *closure = method->as.closure;
                            XrProto *proto = closure->proto;

                            // Setup call: R[a+1] = this (instance)
                            R(a + 1) = val;

                            // Save current PC (continue after return)
                            savepc();

                            // Create new call frame
                            int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                            XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                            new_frame->closure = closure;
                            new_frame->pc = PROTO_CODE_BASE(proto);
                            new_frame->base_offset = (int)((base + a + 1) - VM_STACK);

                            // Mark as toString print call, return value needs printing
                            // Use flags to mark (check on return)
                            new_frame->flags = newline ? 0x02 : 0x01; // 0x01=print, 0x02=print+newline

                            goto startfunc;
                        }
                    }
                }

                // Default print: use unified xr_value_to_string
                XrString *print_str = xr_value_to_string(isolate, val);
                printf("%s", print_str->data);
                if (newline) printf("\n");

                vmbreak;
            }

            vmcase(OP_TYPEOF) {
                // Returns int (XrTypeId) for fast comparison
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int slot_hint = GETARG_C(i);

                XrValue val;
                if (slot_hint == 1) {
                    val = XR_FROM_INT(R(b).i);
                } else if (slot_hint == 2) {
                    val = XR_FROM_FLOAT(R(b).f);
                } else {
                    val = R(b);
                }
                R(a) = XR_FROM_INT((int64_t)xr_value_typeid(val));
                vmbreak;
            }

            vmcase(OP_TYPENAME) {
                // Returns type name as string
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int slot_hint = GETARG_C(i);

                XrValue val;
                if (slot_hint == 1) {
                    val = XR_FROM_INT(R(b).i);
                } else if (slot_hint == 2) {
                    val = XR_FROM_FLOAT(R(b).f);
                } else {
                    val = R(b);
                }
                const char *type_name = NULL;
                // For instances, return class name
                if (xr_value_is_instance(val)) {
                    XrInstance *inst = xr_value_to_instance(val);
                    XrClass *cls = xr_instance_get_class(inst);
                    if (cls && cls->name) type_name = cls->name;
                }
                // For enum values, return enum name
                if (type_name == NULL && XR_IS_ENUM_VALUE(val)) {
                    XrEnumValue *ev = (XrEnumValue*)XR_TO_PTR(val);
                    if (ev->enum_name) type_name = ev->enum_name;
                }
                if (type_name == NULL) {
                    type_name = xr_typeid_name(xr_value_typeid(val));
                }
                size_t len = strlen(type_name);
                XrString *str = xr_string_intern(isolate, type_name, len, 0);
                R(a) = xr_string_value(str);
                vmbreak;
            }

            vmcase(OP_DUMP) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue val = R(a);
                xr_value_dump(val, b);
                vmbreak;
            }

            vmcase(OP_TOINT) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue val = R(b);
                if (XR_IS_INT(val)) {
                    R(a) = val;
                } else if (XR_IS_FLOAT(val)) {
                    R(a) = xr_int((xr_Integer)XR_TO_FLOAT(val));
                } else if (XR_IS_STRING(val)) {
                    XrString *str = XR_TO_STRING(val);
                    const char *p = str->data;
                    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
                    char *end;
                    long long v = strtoll(p, &end, 10);
                    R(a) = (end == p) ? xr_null() : xr_int((xr_Integer)v);
                } else if (XR_IS_BOOL(val)) {
                    R(a) = xr_int(XR_TO_BOOL(val) ? 1 : 0);
                } else {
                    R(a) = xr_null();
                }
                vmbreak;
            }

            vmcase(OP_TOFLOAT) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue val = R(b);
                if (XR_IS_FLOAT(val)) {
                    R(a) = val;
                } else if (XR_IS_INT(val)) {
                    R(a) = xr_float((xr_Number)XR_TO_INT(val));
                } else if (XR_IS_STRING(val)) {
                    XrString *str = XR_TO_STRING(val);
                    const char *p = str->data;
                    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
                    char *end;
                    double v = strtod(p, &end);
                    R(a) = (end == p) ? xr_null() : xr_float(v);
                } else if (XR_IS_BOOL(val)) {
                    R(a) = xr_float(XR_TO_BOOL(val) ? 1.0 : 0.0);
                } else {
                    R(a) = xr_null();
                }
                vmbreak;
            }

            vmcase(OP_TOSTRING) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int slot_hint = GETARG_C(i);

                XrValue val;
                if (slot_hint == 1) {
                    // Raw I64: format directly
                    int64_t raw = R(b).i;
                    char buf[24];
                    int len = snprintf(buf, sizeof(buf), "%" PRId64, raw);
                    R(a) = xr_string_value(xr_string_intern(isolate, buf, len, 0));
                    vmbreak;
                } else if (slot_hint == 2) {
                    // Raw F64: format directly
                    val = XR_FROM_FLOAT(R(b).f);
                    R(a) = xr_string_value(xr_value_to_string(isolate, val));
                    vmbreak;
                }

                val = R(b);
                if (XR_IS_STRING(val)) {
                    R(a) = val;
                } else if (XR_IS_NULL(val)) {
                    // null propagation: string(null) → null
                    R(a) = xr_null();
                } else {
                    R(a) = xr_string_value(xr_value_to_string(isolate, val));
                }
                vmbreak;
            }

            vmcase(OP_TOBOOL) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue val = R(b);
                bool result = true;
                if (XR_IS_BOOL(val)) {
                    result = XR_TO_BOOL(val);
                } else if (XR_IS_NULL(val)) {
                    result = false;
                } else if (XR_IS_INT(val)) {
                    result = XR_TO_INT(val) != 0;
                } else if (XR_IS_FLOAT(val)) {
                    result = XR_TO_FLOAT(val) != 0.0;
                } else if (XR_IS_STRING(val)) {
                    XrString *str = XR_TO_STRING(val);
                    result = str->length > 0;
                } else if (XR_IS_ARRAY(val)) {
                    XrArray *arr = XR_TO_ARRAY(val);
                    result = arr->length > 0;
                } else if (XR_IS_MAP(val)) {
                    XrMap *map = XR_TO_MAP(val);
                    result = xr_map_size(map) > 0;
                } else if (XR_IS_SET(val)) {
                    XrSet *set = XR_TO_SET(val);
                    result = xr_set_size(set) > 0;
                }
                R(a) = xr_bool(result);
                vmbreak;
            }

            vmcase(OP_COPY) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue _src = R(b);
                // Fast path: flat-copyable struct (alloc + memcpy, no recursion)
                if (XR_IS_PTR(_src) && XR_HEAP_TYPE(_src) == XR_TINSTANCE) {
                    XrInstance *_inst = (XrInstance *)XR_TO_PTR(_src);
                    XrClass *_cls = _inst->klass;
                    if ((_cls->flags & (XR_CLASS_VALUE_TYPE | XR_CLASS_FLAT_COPYABLE)) ==
                        (XR_CLASS_VALUE_TYPE | XR_CLASS_FLAT_COPYABLE)) {
                        uint32_t _fc = xr_class_instance_field_count(_cls);
                        size_t _sz = sizeof(XrInstance) + sizeof(XrValue) * _fc;
                        XrInstance *_new = (XrInstance *)xr_gc_alloc(
                            xr_isolate_get_gc(isolate), _sz, XR_TINSTANCE);
                        if (_new) {
                            memcpy(_new->fields, _inst->fields, sizeof(XrValue) * _fc);
                            _new->klass = _cls;
                            _new->gc.extra = (_new->gc.extra & 0x01) | (_inst->gc.extra & ~0x01);
                            R(a) = XR_FROM_PTR(_new);
                            vmbreak;
                        }
                    }
                }
                R(a) = xr_deep_copy_to_coro(isolate, _src,
                    (XrCoroutine *)vm_ctx->current_coro);
                vmbreak;
            }

            vmcase(OP_CHR) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue val = R(b);
                if (XR_IS_INT(val)) {
                    xr_Integer cp = XR_TO_INT(val);
                    if (cp >= 0 && cp <= XR_UNICODE_MAX) {
                        char buf[4];
                        int len = 0;
                        if (cp < 0x80) {
                            buf[0] = (char)cp; len = 1;
                        } else if (cp < 0x800) {
                            buf[0] = (char)(0xC0 | (cp >> 6));
                            buf[1] = (char)(0x80 | (cp & 0x3F)); len = 2;
                        } else if (cp < 0x10000) {
                            buf[0] = (char)(0xE0 | (cp >> 12));
                            buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            buf[2] = (char)(0x80 | (cp & 0x3F)); len = 3;
                        } else {
                            buf[0] = (char)(0xF0 | (cp >> 18));
                            buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                            buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            buf[3] = (char)(0x80 | (cp & 0x3F)); len = 4;
                        }
                        R(a) = xr_string_value(xr_string_intern(isolate, buf, len, 0));
                    } else {
                        R(a) = xr_null();
                    }
                } else {
                    R(a) = xr_null();
                }
                vmbreak;
            }

            /* Enum opcodes — see xvm_dispatch_enum.inc.c. */
            #include "xvm_dispatch_enum.inc.c"

            /* Container creation + array/map R/W + substring + index +
             * slice — see xvm_dispatch_collection.inc.c. */
            #include "xvm_dispatch_collection.inc.c"

            /* CALL / RETURN / TAILCALL family — see xvm_dispatch_call.inc.c. */
            #include "xvm_dispatch_call.inc.c"

            /* ========================================================
            ** OOP Instructions
            ** ======================================================== */

            vmcase(OP_CLASS_CREATE_FROM_DESCRIPTOR) {
                // R[A] optionally holds a runtime-resolved super class
                // (for `extends` whose parent comes from a local, upvalue
                // or imported module member). The codegen side computes
                // the parent into R[A] before this instruction; a non-
                // class value (nil) means "fall back to descriptor-
                // encoded resolution".
                int a = GETARG_A(i);
                int bx = GETARG_Bx(i);
                XrValue desc_val = k[bx];
                XrClassDescriptor *desc = (XrClassDescriptor*)XR_TO_PTR(desc_val);
                XrProto *proto = cl->proto;
                XrClass *super_override = NULL;
                XrValue super_slot = R(a);
                if (XR_IS_CLASS(super_slot)) {
                    super_override = XR_TO_CLASS(super_slot);
                }
                XrClass *cls = xr_class_from_descriptor(isolate, desc, proto, cl, base,
                                                        vm_ctx, super_override);
                R(a) = XR_FROM_PTR(cls);
                vmbreak;
            }

            vmcase(OP_CLINIT_CALL) {
                // OP_CLINIT_CALL: call static constructor
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrClass *cls = xr_value_to_class(R(a));

                // Skip if class already initialized
                if (cls->flags & XR_CLASS_INITIALIZED) {
                    vmbreak;
                }

                // Get class descriptor
                XrProto *proto = cl->proto;
                XrValue desc_val = PROTO_CONSTANT(proto, b);
                XrClassDescriptor *desc = (XrClassDescriptor*)XR_TO_PTR(desc_val);

                // Skip if no static constructor
                if (desc->clinit_proto_index < 0) {
                    vmbreak;
                }

                // Get and execute static constructor
                XrProto *clinit_proto = DYNARRAY_GET(&proto->protos, desc->clinit_proto_index, XrProto*);
                XrCoroutine *_clinit_coro = (XrCoroutine *)vm_ctx->current_coro;
                XrClosure *clinit_closure;
                if (_clinit_coro && _clinit_coro->coro_gc) {
                    clinit_closure = (XrClosure*)xr_coro_gc_newobj(_clinit_coro->coro_gc, XR_TFUNCTION, sizeof(XrClosure));
                } else {
                    clinit_closure = (XrClosure*)xr_gc_alloc(&isolate->gc, sizeof(XrClosure), XR_TFUNCTION);
                }
                xr_gc_header_init_type(&clinit_closure->gc, XR_TFUNCTION);
                clinit_closure->proto = clinit_proto;

                cls->flags |= XR_CLASS_INITIALIZED;
                savepc();
                int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                new_frame->closure = clinit_closure;
                new_frame->pc = PROTO_CODE_BASE(clinit_proto);
                new_frame->base_offset = (int)((base + a + 1) - VM_STACK);
                goto startfunc;
            }

            // Abstract method support
            vmcase(OP_ABSTRACT_ERROR) {
                // OP_ABSTRACT_ERROR: abstract method call error
                int a = GETARG_A(i);
                XrValue method_name_val = k[a];
                const char *method_name = XR_IS_STRING(method_name_val) ? XR_TO_STRING(method_name_val)->data : "<unknown>";
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_CALL, "cannot call abstract method '%s'", method_name);
            }

            vmcase(OP_SET_STORAGE_CTX) {
                /* OP_SET_STORAGE_CTX: set storage mode context
                ** A = storage mode (0=normal, 1=shared)
                **
                ** For class instance shared support
                ** Set before constructor call, OP_INVOKE reads this context
                */
                int storage_mode = GETARG_A(i);
                isolate->current_storage_mode = (uint8_t)storage_mode;
                vmbreak;
            }

            vmcase(OP_TO_SHARED) {
                /* OP_TO_SHARED: convert to shared object
                ** A = destination register
                ** B = source register
                **
                ** If already shared, increment reference count
                ** Otherwise deep copy to system heap
                */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue src = R(b);
                R(a) = xr_to_shared(isolate, src);
                vmbreak;
            }

            vmcase(OP_MAP_SETKS) {
                // OP_MAP_SETKS: batch set fields
                int a = GETARG_A(i);
                int count = GETARG_B(i);
                XrInstance *inst_obj = xr_value_to_instance(R(a));
                for (int j = 0; j < count; j++) {
                    XrValue val = R(a + 1 + j);
                    inst_obj->fields[j] = val;
                }
                VM_BARRIER_BACK(inst_obj);
                vmbreak;
            }

            vmcase(OP_GETFIELD) {
                // OP_GETFIELD: instance field read by index
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int field_idx = GETARG_C(i);

                XrValue inst_val = R(b);
                XrInstance *inst_obj = xr_value_to_instance(inst_val);
                R(a) = inst_obj->fields[field_idx];
                vmbreak;
            }

            vmcase(OP_SETFIELD) {
                // OP_SETFIELD: instance field write by index
                int a = GETARG_A(i);
                int field_idx = GETARG_B(i);
                int c = GETARG_C(i);

                XrValue inst_val = R(a);
                XrInstance *inst_obj = xr_value_to_instance(inst_val);
                XrValue val = R(c);
                inst_obj->fields[field_idx] = val;
                VM_BARRIER_VAL(inst_obj, val);
                vmbreak;
            }

            vmcase(OP_GETFIELD_IC) {
                /* R[A] = R[B].K[C] - inline cache field access
                ** Uses field_name_idx as IC key (constant per call site). */
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int field_name_idx = GETARG_C(i);

                XrValue inst_val = R(b);
                if (!xr_value_is_instance(inst_val)) {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY, "field access requires instance object");
                }

                XrInstance *inst_obj = xr_value_to_instance(inst_val);
                XrClass *cls = inst_obj->klass;

                // Lazily ensure the per-ctx IC table for this proto.
                XrICFieldTable *ic_table =
                    xr_vm_ctx_ensure_ic_fields(vm_ctx, frame->closure->proto);
                if (!ic_table) {
                    VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                                     "OP_GETFIELD_IC: failed to allocate IC table");
                }

                size_t cache_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
                XR_VM_IC_ASSERT_INDEX(cache_index, frame->closure->proto);
                XrICField *cache = xr_ic_field_table_get(ic_table, (int)cache_index);
                if (cache) { XR_VM_IC_FIELD_BIND(cache, (int)cache_index); }

                int field_idx = -1;

                // Fast path: monomorphic IC hit
                if (cache && xr_ic_field_lookup_mono(cache, cls, field_name_idx, &field_idx)) {
                    R(a) = inst_obj->fields[field_idx];
                    vmbreak;
                }

                // Fast path: polymorphic IC hit
                if (cache && xr_ic_field_lookup_poly(cache, cls, field_name_idx, &field_idx)) {
                    R(a) = inst_obj->fields[field_idx];
                    vmbreak;
                }

                // Slow path: string lookup
                XrValue field_name_val = K(field_name_idx);
                if (!XR_IS_STRING(field_name_val)) {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "field name must be a string");
                }
                XrString *field_name = XR_TO_STRING(field_name_val);
                field_idx = xr_class_lookup_field_by_name(isolate, cls, field_name->data);

                if (field_idx < 0) {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY, "field '%s' not found", field_name->data);
                }

                R(a) = inst_obj->fields[field_idx];

                // Update IC cache
                if (cache) {
                    xr_ic_field_update(cache, cls, field_idx, field_name_idx);
                }
                vmbreak;
            }

            // Json dynamic object instructions (V2 zero-copy design)

            vmcase(OP_NEWJSON) {
                /* OP_NEWJSON: create Json object
                ** A = destination register
                ** B = Shape constant index
                ** C = storage mode (0=normal, 1=shared)
                */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int storage_mode = GETARG_C(i);
                XrValue shape_val = k[b];
                // Shape stored as integer pointer (not GC managed)
                XrShape *shape = (XrShape*)(intptr_t)XR_TO_INT(shape_val);

                XrJson *json;
                if (storage_mode != 0 && isolate->sys_heap) {
                    // shared: allocate on system heap
                    int field_count = shape->in_object_capacity;
                    size_t size = xr_json_size(field_count);
                    json = (XrJson*)xr_sysheap_alloc_shared(isolate->sys_heap, size, XR_TJSON);
                    if (json) {
                        xr_json_init_inplace(json, shape);
                        XR_GC_SET_STORAGE(&json->gc, storage_mode);
                        if (storage_mode == XR_GC_STORAGE_SHARED) {
                            xr_shared_set_refc(&json->gc, 1);
                        }
                    }
                } else {
                    // normal: allocate on coroutine heap
                    json = xr_json_new_with_shape(VM_CURRENT_CORO, shape);
                }

                R(a) = xr_json_value(json);
                if (storage_mode == 0) checkGC(base + a + 1);
                vmbreak;
            }

            vmcase(OP_JSON_GET) {
                // OP_JSON_GET: read field by index
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrJson *json = xr_value_to_json(R(b));
                R(a) = xr_json_get_field(json, (uint16_t)c);
                vmbreak;
            }

            vmcase(OP_JSON_SET) {
                // OP_JSON_SET: write field by index
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrJson *json = xr_value_to_json(R(a));
                XrValue val = R(c);
                xr_json_set_field(json, (uint16_t)b, val);
                VM_BARRIER_VAL(json, val);
                vmbreak;
            }

            vmcase(OP_JSON_GETK) {
                // OP_JSON_GETK: read field by Symbol
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i); // Local symbol index
                XrJson *json = xr_value_to_json(R(b));
                R(a) = xr_json_get(isolate, json, (SymbolId)PROTO_SYMBOL(cl->proto, c));
                vmbreak;
            }

            vmcase(OP_JSON_SETK) {
                // OP_JSON_SETK: write field by Symbol (supports zero-copy conversion)
                int a = GETARG_A(i);
                int b = GETARG_B(i); // Local symbol index
                int c = GETARG_C(i);
                XrJson *json = xr_value_to_json(R(a));
                XrValue val = R(c);
                xr_json_set(isolate, json, (SymbolId)PROTO_SYMBOL(cl->proto, b), val);
                VM_BARRIER_VAL(json, val);
                vmbreak;
            }

            vmcase(OP_JSON_INIT) {
                // OP_JSON_INIT: direct index write during initialization
                int a = GETARG_A(i);
                int b = GETARG_B(i); // Field index
                int c = GETARG_C(i);
                XrJson *json = xr_value_to_json(R(a));
                XrValue val = R(c);
                xr_json_set_field(json, (uint16_t)b, val);
                VM_BARRIER_VAL(json, val);
                vmbreak;
            }

            vmcase(OP_JSON_INIT_I) {
                // OP_JSON_INIT_I: init field with immediate integer
                int a = GETARG_A(i);
                int b = GETARG_B(i); // Field index
                int c = GETARG_sC(i); // Signed immediate value
                XrJson *json = xr_value_to_json(R(a));
                xr_json_set_field(json, (uint16_t)b, xr_int(c));
                vmbreak;
            }

            vmcase(OP_JSON_INIT_N) {
                // OP_JSON_INIT_N: init field with null
                int a = GETARG_A(i);
                int b = GETARG_B(i); // Field index
                XrJson *json = xr_value_to_json(R(a));
                xr_json_set_field(json, (uint16_t)b, xr_null());
                vmbreak;
            }

            vmcase(OP_INVOKE) {
                /* Unified calling convention (eliminates argument shifting!)
                 *
                 * Register layout:
                 *   R[A]   = return value position
                 *   R[A+1] = this (receiver)
                 *   R[A+2] = arg1
                 *   R[A+3] = arg2
                 *   ...
                 *
                 * Instruction format: OP_INVOKE A B C
                 *   A = base register
                 *   B = method symbol
                 *   C = argument count (excluding this)
                 *
                 * OP_INVOKE_TAIL shares this dispatch via invoke_dispatch label.
                 * invoke_is_tail=1 causes closure methods to reuse current frame.
                 */
                TRACE_EXECUTION();
                invoke_is_tail = 0;
                invoke_dispatch: ;

                /* Reductions check intentionally absent here: only OP_JMP checks.
                ** GC safepoint handled at startfunc label. */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int nargs = GETARG_C(i);

                /* Persist pc once for every dispatch path. Builtin
                 * handlers that throw a contract exception (e.g.
                 * WeakMap.set / WeakSet.add receiver-type guards)
                 * call xr_vm_throw_exception, which reads frame->pc
                 * to compute the throw line and rewrites it to the
                 * matching catch handler. */
                savepc();

                /* After a builtin handler returns, surface any
                 * pending exception thrown from inside it. The throw
                 * already redirected frame->pc, so we just refresh
                 * dispatch locals via startfunc (or fall out to the
                 * embedder when no handler is on the stack). */
                #define VM_BUILTIN_INVOKE_CHECK_EXC() do { \
                    if (unlikely(!XR_IS_NULL(VM_EXCEPTION))) { \
                        if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR; \
                        goto startfunc; \
                    } \
                } while (0)

                // Declared here (before all invoke_* labels) so jumps past
                // the assignment below still observe a deterministic NULL.
                const char *method_name_chars = NULL;

                // receiver at R[a+1] (new calling convention)
                XrValue receiver = R(a + 1);


                // Dereference local index → global symbol via per-function symbol table
                int method_symbol = PROTO_SYMBOL(cl->proto, b);

                // Method inline optimization (fast path)
                // Property access .length is the standard way to get collection size

                // Cold path: task handle methods (works even after executor detach)
                if (xr_value_is_task(receiver)) {
                    savepc();
                    int _cr = vm_invoke_task_handle(isolate, receiver, method_symbol, nargs, base, a, ci, pc);
                    if (_cr == VM_COLD_BREAK) vmbreak;
                    if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                    goto startfunc;
                }

                // Cold path: legacy coroutine handle methods
                if (xr_value_is_coro(receiver)) {
                    savepc();
                    int _cr = vm_invoke_coro_handle(isolate, receiver, method_symbol, nargs, base, a, ci, pc);
                    if (_cr == VM_COLD_BREAK) vmbreak;
                    if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                    goto startfunc;
                }

                // Channel methods: inline send/recv hot path, cold path for rest
                if (xr_value_is_channel(receiver)) {
                    XrChannel *ch = xr_value_to_channel(receiver);

                    // Hot path: ch.send(value) - inline blocking send
                    if (nargs == 1 && method_symbol == SYMBOL_SEND) {
                        XrCoroutine *_cur = (XrCoroutine *)VM_CURRENT_CORO;
                        if (_cur && xr_coro_resume_load(_cur) == XR_RESUME_CHANNEL) {
                            xr_coro_resume_store(_cur, XR_RESUME_OK);
                            R(a) = xr_null();
                            vmbreak;
                        }
                        XrValue _sv = vm_chan_copy_send(isolate, R(a + 2));
                        // Pre-save frame state before channel call.  If send blocks, channel func sets BLOCKED
                        // under lock — coro must already have saved state.
                        if (_cur) _cur->send_value = _sv;
                        savepc();
                        frame->pc = pc - 1;
                        frame->call_status |= XR_CALL_YIELDED;
                        XrChanResult _cr = xr_channel_send(ch, _sv, _cur);
                        if (_cr == XR_CHAN_OK) {
                            frame->call_status &= ~XR_CALL_YIELDED;
                            R(a) = xr_null();
                            vmbreak;
                        } else if (_cr == XR_CHAN_BLOCK) {
                            return XR_VM_BLOCKED;
                        } else {
                            frame->call_status &= ~XR_CALL_YIELDED;
                        }
                        // Closed or error: fall through to cold path
                    }

                    // Hot path: ch.recv() - inline blocking recv
                    if (nargs == 0 && method_symbol == SYMBOL_RECV) {
                        XrCoroutine *_cur = (XrCoroutine *)VM_CURRENT_CORO;
                        if (_cur) {
                            int _rs = xr_coro_resume_load(_cur);
                            if (_rs == XR_RESUME_CHANNEL) {
                                xr_coro_resume_store(_cur, XR_RESUME_OK);
                                R(a) = vm_chan_copy_recv(isolate, R(a), vm_ctx);
                                vmbreak;
                            }
                            if (_rs == XR_RESUME_CHANNEL_CLOSED) {
                                xr_coro_resume_store(_cur, XR_RESUME_OK);
                                _cur->wait_channel = NULL;
                            }
                        }
                        // Set recv_slot BEFORE xr_channel_recv: once coro enters
                        // recvq, a sender on another worker may immediately dequeue
                        // and write to recv_slot.  Setting it after return races.
                        if (_cur) _cur->recv_slot = &R(a);
                        // Pre-save frame state — see send path.
                        savepc();
                        frame->pc = pc - 1;
                        frame->call_status |= XR_CALL_YIELDED;
                        XrValue _rv;
                        XrChanResult _cr = xr_channel_recv(ch, &_rv, _cur);
                        if (_cr == XR_CHAN_OK) {
                            frame->call_status &= ~XR_CALL_YIELDED;
                            R(a) = vm_chan_copy_recv(isolate, _rv, vm_ctx);
                            vmbreak;
                        } else if (_cr == XR_CHAN_CLOSED) {
                            frame->call_status &= ~XR_CALL_YIELDED;
                            R(a) = xr_null();
                            vmbreak;
                        } else if (_cr == XR_CHAN_BLOCK) {
                            return XR_VM_BLOCKED;
                        } else {
                            frame->call_status &= ~XR_CALL_YIELDED;
                        }
                        // Error: fall through to cold path
                    }

                    // Cold path: other channel methods
                    savepc();
                    int _cr = vm_invoke_channel(isolate, vm_ctx, ch, method_symbol, nargs, base, a, frame, pc);
                    if (_cr == VM_COLD_BREAK) vmbreak;
                    if (_cr == VM_COLD_BLOCKED) return XR_VM_BLOCKED;
                    if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                    goto startfunc;
                }

                if (nargs == 0 && method_symbol == SYMBOL_IS_EMPTY) {
                    // isEmpty() method inline: directly check count == 0
                    if (XR_IS_ARRAY(receiver)) {
                        XrArray *arr = XR_TO_ARRAY(receiver);
                        R(a) = xr_bool(arr->length == 0);
                        vmbreak; // Skip method call!
                    }
                    else if (XR_IS_STRING(receiver)) {
                        R(a) = xr_bool(xr_value_str_len(&receiver) == 0);
                        vmbreak; // Skip method call!
                    }
                    else if (XR_IS_MAP(receiver)) {
                        XrMap *map = XR_TO_MAP(receiver);
                        R(a) = xr_bool(map->count == 0);
                        vmbreak; // Skip method call!
                    }
                    else if (XR_IS_SET(receiver)) {
                        XrSet *set = XR_TO_SET(receiver);
                        R(a) = xr_bool(set->count == 0);
                        vmbreak; // Skip method call!
                    }
                }

                /* === Type-based dispatch: O(1) jump table === */

                /* Struct ref: value-type constructor/method call.
                 * struct_ref layout: [XrClass* 8B][fields...] in struct_area */
                if (XR_IS_STRUCT_REF(receiver)) {
                    uint8_t *sptr = (uint8_t*)xr_to_struct_ptr(receiver);
                    XrClass *scls = *(XrClass**)sptr;
                    XrMethod *method = xr_class_lookup_method(scls, method_symbol);

                    if (method == NULL || method->type == XMETHOD_NONE) {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD,
                            "struct '%s' has no method '%s'", scls->name, _mn ? _mn : "?");
                    }
                    if (method->type == XMETHOD_PRIMITIVE && method->as.primitive != NULL) {
                        R(a) = method->as.primitive(isolate, &R(a + 1), nargs + 1);
                        vmbreak;
                    }
                    if (method->type == XMETHOD_CLOSURE && method->as.closure != NULL) {
                        XrClosure *closure = method->as.closure;
                        XrProto *proto = closure->proto;
                        if (nargs + 1 != proto->numparams) {
                            VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT,
                                "constructor expects %d arguments, got %d",
                                proto->numparams - 1, nargs);
                        }
                        if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                            VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
                        }
                        // this (struct_ref) already in R[a+1]
                        savepc();
                        int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                        XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                        new_frame->closure = closure;
                        new_frame->pc = PROTO_CODE_BASE(proto);
                        new_frame->base_offset = (int)((base + a + 1) - VM_STACK);
                        goto startfunc;
                    }
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "struct method has invalid type");
                }

                if (!XR_IS_PTR(receiver)) {
                    if (XR_IS_INT(receiver)) goto invoke_int;
                    if (XR_IS_FLOAT(receiver)) goto invoke_float;
                    if (XR_IS_BOOL(receiver)) goto invoke_bool;
                    // SSO removed: all strings are heap PTR, handled by XR_TSTRING case
                    goto invoke_type_error;
                }
                switch (XR_GC_GET_TYPE((XrGCHeader*)XR_TO_PTR(receiver))) {
                    case XR_TINSTANCE:
                    case XR_TCLASS:          goto invoke_class_or_instance;
                    case XR_TSTRING:         goto invoke_string;
                    case XR_TARRAY:          goto invoke_array;
                    case XR_TMAP:            goto invoke_map;
                    case XR_TSET:            goto invoke_set;
                    case XR_TJSON:           goto invoke_json;
                    case XR_TMODULE:         goto invoke_module;
                    case XR_TENUM_VALUE:
                    case XR_TENUM_TYPE:      goto invoke_enum;
                    case XR_TITERATOR:       goto invoke_iterator;
                    case XR_TBIGINT:         goto invoke_bigint;
                    case XR_TSTRINGBUILDER:  goto invoke_stringbuilder;
                    case XR_TARRAY_SLICE:    goto invoke_slice;
                    case XR_TRANGE:          goto invoke_range;
                    default:                 goto invoke_native_type;
                }

                // Cold path: enum methods
                invoke_enum: {
                    savepc();
                    int _cr = vm_invoke_enum(isolate, receiver, method_symbol, nargs, base, a, ci, pc);
                    if (_cr == VM_COLD_BREAK) vmbreak;
                    if (_cr == VM_COLD_ERROR) {
                        if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                        goto startfunc;
                    }
                    // VM_COLD_CONTINUE: fall through to class/instance path
                }

                // Original logic: normal method call
                // Args from R[a+2] (R[a]=return value, R[a+1]=this)

                // Iterator methods (hot path: for-in loops call hasNext+next every iteration)
                invoke_iterator:
                if (xr_is_iterator(receiver)) {
                    XrIterator *iter = xr_value_to_iterator(receiver);
                    if (unlikely(!iter)) {
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid iterator object");
                    }
                    if (method_symbol == SYMBOL_HASNEXT) {
                        R(a) = xr_bool(xr_iterator_has_next(iter));
                    } else if (method_symbol == SYMBOL_NEXT) {
                        R(a) = xr_iterator_next(iter);
                    } else if (method_symbol == SYMBOL_TOSTRING) {
                        R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                    } else {
                        XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
                        const char *mname = xr_symbol_get_name_in_table(sym_table, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD,
                            "iterator does not support method: %s", mname ? mname : "?");
                    }
                    vmbreak;
                }

                /* === Map builtin methods === */
                invoke_map:
                if (XR_IS_MAP(receiver)) {
                    /* See xmap_methods.h — unified method table dispatch.
                     * WeakMap-blocked methods return XR_NOTFOUND from the
                     * method body itself. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        XR_TID_MAP, method_symbol, SYMBOL_BUILTIN_COUNT);
                    R(a) = _slot ? _slot->fn(isolate, receiver, &R(a + 2), nargs)
                                 : XR_NOTFOUND;
                    VM_BUILTIN_INVOKE_CHECK_EXC();
                    if (unlikely(XR_IS_NOTFOUND(R(a)))) {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "Map has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === Json builtin methods === */
                invoke_json:
                if (xr_value_is_json(receiver)) {
                    /* See xjson_methods.h — unified method table dispatch. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        XR_TID_JSON, method_symbol, SYMBOL_BUILTIN_COUNT);
                    R(a) = _slot ? _slot->fn(isolate, receiver, &R(a + 2), nargs)
                                 : XR_NOTFOUND;
                    VM_BUILTIN_INVOKE_CHECK_EXC();
                    if (unlikely(XR_IS_NOTFOUND(R(a)))) {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "Json has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === String builtin methods === */
                invoke_string:
                if (XR_IS_STRING(receiver)) {
                    /* See xstring_methods.h — unified method table dispatch.
                     * Receiver stays in heap form to skip the SSO→promote
                     * round-trip the legacy dispatcher used to do. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        XR_TID_STRING, method_symbol, SYMBOL_BUILTIN_COUNT);
                    R(a) = _slot ? _slot->fn(isolate, receiver, &R(a + 2), nargs)
                                 : XR_NOTFOUND;
                    VM_BUILTIN_INVOKE_CHECK_EXC();
                    if (unlikely(XR_IS_NOTFOUND(R(a)))) {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "String has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === Array builtin methods === */
                invoke_array:
                if (XR_IS_ARRAY(receiver)) {
                    /* See xarray_methods.h — unified method table dispatch. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        XR_TID_ARRAY, method_symbol, SYMBOL_BUILTIN_COUNT);
                    R(a) = _slot ? _slot->fn(isolate, receiver, &R(a + 2), nargs)
                                 : XR_NOTFOUND;
                    VM_BUILTIN_INVOKE_CHECK_EXC();
                    if (unlikely(XR_IS_NOTFOUND(R(a)))) {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "Array has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === Set builtin methods === */
                invoke_set:
                if (XR_IS_SET(receiver)) {
                    /* See xset_methods.h — unified method table dispatch.
                     * WeakSet-blocked methods return XR_NOTFOUND from the
                     * method body itself, so the same
                     * "method not found" diagnostic surfaces. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        XR_TID_SET, method_symbol, SYMBOL_BUILTIN_COUNT);
                    R(a) = _slot ? _slot->fn(isolate, receiver, &R(a + 2), nargs)
                                 : XR_NOTFOUND;
                    VM_BUILTIN_INVOKE_CHECK_EXC();
                    if (unlikely(XR_IS_NOTFOUND(R(a)))) {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "Set has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === Int builtin methods === */
                invoke_int:
                if (XR_IS_INT(receiver)) {
                    /* See xint_methods.h — unified method table dispatch. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        XR_TID_INT, method_symbol, SYMBOL_BUILTIN_COUNT);
                    if (likely(_slot != NULL)) {
                        R(a) = _slot->fn(isolate, receiver, &R(a + 2), nargs);
                        VM_BUILTIN_INVOKE_CHECK_EXC();
                    } else {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "int has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === Float builtin methods === */
                invoke_float:
                if (XR_IS_FLOAT(receiver)) {
                    /* See xfloat_methods.h — unified method table dispatch. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        XR_TID_FLOAT, method_symbol, SYMBOL_BUILTIN_COUNT);
                    if (likely(_slot != NULL)) {
                        R(a) = _slot->fn(isolate, receiver, &R(a + 2), nargs);
                        VM_BUILTIN_INVOKE_CHECK_EXC();
                    } else {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "float has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === Bool builtin methods === */
                invoke_bool:
                if (XR_IS_BOOL(receiver)) {
                    /* Bool dispatches through the unified method table.
                     * Hot small methods (e.g. toString) are static-inline
                     * in xbool_methods.h, so AOT inlines them at the call
                     * site; the VM reaches the same body via this table
                     * indirection. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        XR_TID_BOOL, method_symbol, SYMBOL_BUILTIN_COUNT);
                    if (likely(_slot != NULL)) {
                        R(a) = _slot->fn(isolate, receiver, &R(a + 2), nargs);
                        VM_BUILTIN_INVOKE_CHECK_EXC();
                    } else {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "bool has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === BigInt builtin methods === */
                invoke_bigint:
                if (XR_IS_BIGINT(receiver)) {
                    /* BigInt dispatches through the unified method table.
                     * See xbigint_methods.h — every method is a static
                     * inline wrapper around an extern xr_bigint_*
                     * primitive, so AOT inlines the wrapper at the call
                     * site and the VM reaches a single out-of-line copy
                     * via this indirection. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        XR_TID_BIGINT, method_symbol, SYMBOL_BUILTIN_COUNT);
                    if (likely(_slot != NULL)) {
                        R(a) = _slot->fn(isolate, receiver, &R(a + 2), nargs);
                        VM_BUILTIN_INVOKE_CHECK_EXC();
                    } else {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "BigInt has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === Stdlib/third-party native type method call (via native_type_classes mapping) === */
                invoke_native_type:
                if (XR_IS_PTR(receiver)) {
                    XrGCHeader *gc = (XrGCHeader*)XR_TO_PTR(receiver);
                    XrObjType native_type = XR_GC_GET_TYPE(gc);

                    // Find bound XrClass
                    if (native_type < XR_NATIVE_TYPE_MAX) {
                        XrClass *native_klass = isolate->native_type_classes[native_type];
                        if (native_klass) {
                            // Lookup through class methods
                            XrMethod *method = xr_class_lookup_method(native_klass, method_symbol);
                            if (method && method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
                                // Native method: call C function directly
                                R(a) = method->as.primitive(isolate, &R(a + 1), nargs + 1);
                                vmbreak;
                            }
                        }
                    }
                }

                // Cold path: module export function call
                invoke_module:
                if (xr_value_is_module(receiver)) {
                    savepc();
                    int _cr = vm_invoke_module(isolate, vm_ctx, receiver, method_symbol, nargs, base, a, ci, pc);
                    if (_cr == VM_COLD_BREAK) vmbreak;
                    if (_cr == VM_COLD_STARTFUNC) goto startfunc;
                    if (_cr == VM_COLD_BLOCKED) return XR_VM_BLOCKED;
                    if (_cr == VM_COLD_YIELD) return XR_VM_YIELD;
                    if (_cr == VM_COLD_FATAL) return XR_VM_RUNTIME_ERROR;
                    if (_cr == VM_COLD_ERROR) {
                        if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                        goto startfunc;
                    }
                }

                // Class/Instance method call path.
                // method_name_chars is declared at the top of invoke_dispatch
                // so every `goto invoke_*` sibling label observes NULL.
                invoke_class_or_instance: ;
                if (xr_value_is_class(receiver) || xr_value_is_instance(receiver)) {
                    // Get method name only when needed
                    XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
                    method_name_chars = xr_symbol_get_name_in_table(sym_table, method_symbol);
                    if (method_name_chars == NULL) {
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "invalid method symbol: %d", method_symbol);
                    }
                }

                // Lazily ensure the per-ctx method IC table for this proto.
                XrICMethodTable *ic_method_table =
                    xr_vm_ctx_ensure_ic_methods(vm_ctx, frame->closure->proto);
                if (!ic_method_table) {
                    VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                                     "OP_INVOKE: failed to allocate IC table");
                }

                // Range method call
                invoke_range:
                if (XR_IS_RANGE(receiver)) {
                    XrRange *rng = xr_value_to_range(receiver);
                    if (method_symbol == SYMBOL_TOSTRING) {
                        R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                        vmbreak;
                    } else if (method_symbol == SYMBOL_TO_ARRAY) {
                        R(a) = xr_range_to_array(VM_CURRENT_CORO, rng);
                        vmbreak;
                    } else if (method_symbol == SYMBOL_CONTAINS) {
                        if (nargs >= 1 && XR_IS_INT(R(a + 2))) {
                            R(a) = xr_bool(xr_range_contains(rng, XR_TO_INT(R(a + 2))));
                        } else {
                            R(a) = xr_bool(false);
                        }
                        vmbreak;
                    } else {
                        XrSymbolTable *st = (XrSymbolTable*)isolate->symbol_table;
                        const char *mname = xr_symbol_get_name_in_table(st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD,
                            "Range has no method '%s'", mname ? mname : "?");
                    }
                }

                // Support StringBuilder method call
                invoke_stringbuilder:
                if (xr_is_stringbuilder(receiver)) {
                    XrClass *klass = xr_value_get_class(isolate, receiver);
                    if (klass) {
                        XrMethod *method = xr_class_lookup_method(klass, method_symbol);
                        if (method != NULL && method->type == XMETHOD_PRIMITIVE && method->as.primitive != NULL) {
                            R(a) = method->as.primitive(isolate, &R(a + 1), nargs + 1);
                            vmbreak;
                        }
                    }
                }

                // Support basic types (Int, Float, Bool) method call
                // Args from R[a+1] (this at a+1)
                if (XR_IS_INT(receiver) || XR_IS_FLOAT(receiver) || XR_IS_BOOL(receiver)) {
                    XrClass *klass = xr_value_get_class(isolate, receiver);

                    if (klass) {
                        XrMethod *method = xr_class_lookup_method(klass, method_symbol);

                        if (method != NULL && method->type == XMETHOD_PRIMITIVE && method->as.primitive != NULL) {
                            XrCFunctionPtr cfunc = method->as.primitive;
                            XrValue result = cfunc(isolate, &R(a + 1), nargs + 1);

                            R(a) = result;
                            vmbreak;
                        } else {
                            // Universal toString fallback for basic types
                            if (method_symbol == SYMBOL_TOSTRING) {
                                R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                                vmbreak;
                            }
                            // Basic type method not found - show type and method name
                            XrSymbolTable *st = (XrSymbolTable*)isolate->symbol_table;
                            const char *mname = xr_symbol_get_name_in_table(st, method_symbol);
                            const char *tname = XR_IS_INT(receiver) ? "int" :
                                               (XR_IS_FLOAT(receiver) ? "float" : "bool");
                            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD,
                                "%s type has no method '%s'", tname, mname ? mname : "?");
                        }
                    }
                }

                // Support ArraySlice method call
                invoke_slice:
                if (XR_IS_PTR(receiver)) {
                    XrGCHeader *gc = (XrGCHeader*)XR_TO_PTR(receiver);
                    int heap_type = XR_GC_GET_TYPE(gc);

                    if (heap_type == XR_TARRAY_SLICE) {
                        XrClass *klass = xr_value_get_class(isolate, receiver);
                        if (klass) {
                            XrMethod *method = xr_class_lookup_method(klass, method_symbol);
                            if (method && method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
                                R(a) = method->as.primitive(isolate, &R(a + 1), nargs + 1);
                                vmbreak;
                            }
                        }
                        if (method_symbol == SYMBOL_TOSTRING) {
                            R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                            vmbreak;
                        }
                        XrSymbolTable *st = (XrSymbolTable*)isolate->symbol_table;
                        const char *name = xr_symbol_get_name_in_table(st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "ArraySlice has no method '%s'", name ? name : "?");
                    }
                }

                // Cold path: class constructor / static method
                if (xr_value_is_class(receiver)) {
                    int _cr = vm_invoke_class(isolate, vm_ctx, receiver, method_symbol,
                                              method_name_chars, nargs, base, a, ci, pc,
                                              invoke_is_tail);
                    if (_cr == VM_COLD_BREAK) vmbreak;
                    if (_cr == VM_COLD_STARTFUNC) goto startfunc;
                    if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                    goto startfunc;
                } else {
                    // Instance method call
                    if (!xr_value_is_instance(receiver)) {
                        // Universal toString fallback for any remaining type
                        if (method_symbol == SYMBOL_TOSTRING) {
                            R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                            vmbreak;
                        }
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "method '%s' called on non-instance", method_name_chars);
                    }
                    XrInstance *inst = xr_value_to_instance(receiver);

                    // Find method via polymorphic inline cache
                    size_t cache_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
                    XR_VM_IC_ASSERT_INDEX(cache_index, frame->closure->proto);
                    XrICMethod *cache = xr_ic_method_table_get(ic_method_table, cache_index);
                    if (cache) { XR_VM_IC_METHOD_BIND(cache, (int)cache_index); }

                    XrMethod *method = NULL;
                    if (cache) {
                        method = xr_ic_method_lookup(cache, inst->klass, method_symbol);
                    } else {
                        method = xr_class_lookup_method(inst->klass, method_symbol);
                    }

                    if (method == NULL || method->type == XMETHOD_NONE) {
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "method '%s' not found", method_name_chars);
                    }

                    // Support PRIMITIVE type instance methods (reflection API)
                    // Args from R[a+1] (this at a+1)
                    if (method->type == XMETHOD_PRIMITIVE && method->as.primitive != NULL) {
                        R(a) = method->as.primitive(isolate, &R(a + 1), nargs + 1);
                        vmbreak;
                    }

                    if (method->type != XMETHOD_CLOSURE || method->as.closure == NULL) {
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "method '%s' has invalid type", method_name_chars);
                    }

                    // Reuse method closure directly
                    XrClosure *closure = method->as.closure;
                    XrProto *proto = closure->proto;

                    // Check argument count
                    if (nargs + 1 != proto->numparams) {
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "method '%s' expects %d arguments but got %d",
                                         method_name_chars, proto->numparams - 1, nargs);
                    }

                    if (invoke_is_tail) {
                        // Tail call: reuse current stack frame
                        memmove(base, &R(a + 1), sizeof(XrValue) * (nargs + 1));
                        ci->closure = closure;
                        ci->pc = PROTO_CODE_BASE(proto);
                        VM_SET_STACK_TOP(base + proto->maxstacksize);
                        goto startfunc;
                    }

                    // Check stack space
                    if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                        VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
                    }

                    // Unified calling convention: this already in R[a+1], args in R[a+2]..., no shift needed!

                    // Save current frame pc
                    savepc();

                    // Create new call frame
                    int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                    XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                    new_frame->closure = closure;
                    new_frame->pc = PROTO_CODE_BASE(proto);
                    new_frame->base_offset = (int)((base + a + 1) - VM_STACK);

                    // Jump to new function
                    goto startfunc;
                }

                invoke_type_error: {
                    XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                    const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "method '%s' called on unsupported type", _mn ? _mn : "?");
                }
                vmbreak;
            }

            vmcase(OP_INVOKE_TAIL) {
                /* Method tail call: reuse OP_INVOKE's full dispatch,
                 * but reuse current stack frame for closure methods.
                 * Set flag and jump into OP_INVOKE's code. */
                TRACE_EXECUTION();
                invoke_is_tail = 1;
                goto invoke_dispatch;
            }

            vmcase(OP_SUPERINVOKE) {
                TRACE_EXECUTION();
                savepc();
                int _cr = vm_superinvoke(isolate, vm_ctx, i, base, ci, pc);
                if (_cr == VM_COLD_STARTFUNC) goto startfunc;
                if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                goto startfunc;
            }

            /* ========================================================
            ** Optimized instructions: compile-time determined index (fully inlined)
            ** ======================================================== */

            vmcase(OP_INVOKE_DIRECT) {
                /* ========== OP_INVOKE_DIRECT - Direct method call (unified calling convention) ==========
                 *
                 * Format: OP_INVOKE_DIRECT A B C
                 *   A = base register (return value in R[A], this in R[A+1])
                 *   B = method index
                 *   C = nargs | tail_flag (bit 7: 0x80 = tail call)
                 *
                 * Layout: R[A]=return value, R[A+1]=this, R[A+2]=arg1, ...
                 */
                int a = GETARG_A(i);
                int method_idx = GETARG_B(i);
                int c_raw = GETARG_C(i);
                int is_tail_direct = (c_raw & 0x80);
                int nargs = c_raw & 0x7F;

                XrValue receiver = R(a + 1);
                XrClass *cls;
                if (XR_IS_STRUCT_REF(receiver)) {
                    // Stack-allocated struct: class ptr at head
                    cls = *(XrClass**)xr_to_struct_ptr(receiver);
                } else {
                    XrInstance *inst_obj = xr_value_to_instance(receiver);
                    cls = xr_instance_get_class(inst_obj);
                }
                XrMethod *method = &cls->methods[method_idx];
                XrClosure *closure = method->as.closure;

                if (is_tail_direct) {
                    // Tail call: reuse current stack frame
                    memmove(base, &R(a + 1), sizeof(XrValue) * (nargs + 1));
                    ci->closure = closure;
                    ci->pc = PROTO_CODE_BASE(closure->proto);
                    VM_SET_STACK_TOP(base + closure->proto->maxstacksize);
                    goto startfunc;
                }

                savepc();
                int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                new_frame->closure = closure;
                new_frame->pc = PROTO_CODE_BASE(closure->proto);
                new_frame->base_offset = (int)((base + a + 1) - VM_STACK); // this at R[a+1]
                goto startfunc;
            }


            vmcase(OP_INVOKE_BUILTIN) {
                /* ========== OP_INVOKE_BUILTIN — builtin-type method call ==========
                 *
                 * Format: OP_INVOKE_BUILTIN A B C
                 *   A = base register (return value in R[A], this in R[A+1])
                 *   B = method symbol
                 *   C = nargs (argument count, excluding this)
                 *
                 * Dispatch fast path (this opcode hits ~all .map/.push/.has
                 * call sites in real code, so it has to be tight):
                 *
                 *   1. Per-call-site monomorphic IC keyed on (XrTypeId,
                 *      slot*). On hit, jump straight to slot->fn — skipping
                 *      both the 10-branch type chain and the unified
                 *      table indirection.
                 *
                 *   2. On miss (uninitialized, type changed, etc.) fall
                 *      into the unified xr_method_table_lookup over the
                 *      ten migrated types. Cache (sticky first-write-wins)
                 *      after a successful dispatch.
                 *
                 *   3. If the type isn't migrated (StringBuilder, slice,
                 *      null, raw heap object), fall through to the
                 *      type-specific branches at the bottom.
                 */
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int method_symbol = PROTO_SYMBOL(cl->proto, GETARG_B(i));
                int nargs = GETARG_C(i);

                XrValue receiver = R(a + 1);
                XrValue *args = &R(a + 2);

                /* Persist the local pc into the frame before calling
                 * any builtin handler. If the handler throws (see e.g.
                 * the WeakMap.set / WeakSet.add receiver-type guards),
                 * xr_vm_add_stacktrace and xr_vm_throw_exception read
                 * frame->pc to record the throw site and to redirect
                 * to the catch handler. */
                savepc();

                /* Lazy IC table allocation. cache_index = pc - 1 - base
                 * because pc has already advanced past this instruction.
                 * Pre-sized to PROTO_CODE_COUNT in xr_ic_builtin_table_new
                 * so the index is always valid. */
                XrICBuiltinTable *_btab =
                    xr_vm_ctx_ensure_ic_builtin(vm_ctx, cl->proto);
                if (unlikely(!_btab)) {
                    VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                        "OP_INVOKE_BUILTIN: failed to allocate IC table");
                }
                size_t _cidx = (size_t)(pc - 1 - PROTO_CODE_BASE(cl->proto));
                XrICBuiltin *_ic = &_btab->caches[_cidx];

                int _rcv_tid = (int)xr_value_typeid(receiver);

                if (likely(_ic->slot != NULL && _ic->cached_tid == _rcv_tid)) {
                    /* IC hit: direct dispatch, no chain, no table lookup. */
                    if (likely(_ic->hits != UINT16_MAX)) _ic->hits++;
                    R(a) = _ic->slot->fn(isolate, receiver, args, nargs);
                } else {
                    if (_ic->slot) {
                        /* IC miss on already-filled cache (poly site).
                         * We don't replace the cached slot — sticky
                         * first-write-wins keeps the hot type fast. */
                        if (likely(_ic->misses != UINT16_MAX)) _ic->misses++;
                    }

                    /* Unified slow path: single lookup covers all ten
                     * migrated types. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        _rcv_tid, method_symbol, SYMBOL_BUILTIN_COUNT);
                    if (_slot) {
                        R(a) = _slot->fn(isolate, receiver, args, nargs);
                        if (!_ic->slot) {
                            /* First-write-wins. cached_tid fits in
                             * int16_t because XR_TID_COUNT < 256. */
                            _ic->slot = _slot;
                            _ic->cached_tid = (int16_t)_rcv_tid;
                        }
                    } else if (xr_is_stringbuilder(receiver)) {
                    // StringBuilder: dispatch through native_type_classes
                    XrClass *klass = isolate->native_type_classes[XR_TSTRINGBUILDER];
                    if (klass) {
                        XrMethod *method = xr_class_lookup_method(klass, method_symbol);
                        if (method && method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
                            R(a) = method->as.primitive(isolate, &R(a + 1), nargs + 1);
                            vmbreak;
                        }
                    }
                    if (method_symbol == SYMBOL_TOSTRING) {
                        R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                        vmbreak;
                    }
                    XrSymbolTable *st = (XrSymbolTable*)isolate->symbol_table;
                    const char *mn = xr_symbol_get_name_in_table(st, method_symbol);
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "StringBuilder has no method '%s'", mn ? mn : "?");
                } else if (XR_IS_PTR(receiver)) {
                    // Slice type method dispatch
                    XrGCHeader *gc = (XrGCHeader*)XR_TO_PTR(receiver);
                    int heap_type = XR_GC_GET_TYPE(gc);

                    if (heap_type == XR_TARRAY_SLICE) {
                        // Get class for slice type, lookup method through class
                        XrClass *klass = xr_value_get_class(isolate, receiver);
                        if (klass) {
                            XrMethod *method = xr_class_lookup_method(klass, method_symbol);
                            if (method && method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
                                R(a) = method->as.primitive(isolate, &R(a + 1), nargs + 1);
                                vmbreak;
                            }
                        }
                        if (method_symbol == SYMBOL_TOSTRING) {
                            R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                            vmbreak;
                        }
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "slice type has no such method (symbol %d)", method_symbol);
                    }
                    // Universal toString fallback for all other heap types
                    if (method_symbol == SYMBOL_TOSTRING) {
                        R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                        vmbreak;
                    }
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "this type does not support builtin method call");
                } else if (XR_IS_NULL(receiver)) {
                    if (method_symbol == SYMBOL_TOSTRING) {
                        R(a) = xr_string_value(xr_string_intern(isolate, "null", 4, 0));
                    } else {
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "null has no method");
                    }
                } else {
                    // Universal toString fallback
                    if (method_symbol == SYMBOL_TOSTRING) {
                        R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                    } else {
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "this type does not support builtin method call");
                    }
                }
                } /* close slow-path else (IC miss) */
                // If a builtin handler threw a catchable exception
                // (e.g. WeakMap.set with non-object key), the throw
                // logic already redirected frame->pc to the matching
                // handler and stashed the exception on the ctx; we
                // just need to refresh the dispatch locals and either
                // continue at the catch site or surface the error to
                // the embedder when nothing caught it.
                if (unlikely(!XR_IS_NULL(VM_EXCEPTION))) {
                    if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                    goto startfunc;
                }
                // Unified method-not-found check for all builtin dispatch functions
                if (XR_IS_NOTFOUND(R(a))) {
                    XrSymbolTable *st = (XrSymbolTable*)isolate->symbol_table;
                    const char *name = xr_symbol_get_name_in_table(st, method_symbol);
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD,
                        "type '%s' has no method '%s'", xr_typeid_name(xr_value_typeid(receiver)), name ? name : "?");
                }
                vmbreak;
            }


            /* ========================================================
            ** Property access instructions
            ** ======================================================== */

            vmcase(OP_GETPROP) {
                // R[A] = R[B].Symbol[C] - Get property/method
                // Use Symbol direct dispatch, 10x performance improvement
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);

                XrValue obj = R(b);
                int prop_symbol = PROTO_SYMBOL(cl->proto, c); // Dereference local index → global symbol

                // Fixed array .length
                if (XR_IS_ARRAY_REF(obj) && prop_symbol == SYMBOL_LENGTH) {
                    R(a) = XR_FROM_INT((int64_t)XR_ARRAY_REF_ELEM_COUNT(obj));
                    vmbreak;
                }

                // Stack-allocated struct field access
                if (XR_IS_STRUCT_REF(obj)) {
                    uint8_t *sptr = (uint8_t*)xr_to_struct_ptr(obj);
                    XrClass *scls = *(XrClass**)sptr;
                    int fidx = xr_class_lookup_field(scls, prop_symbol);
                    if (fidx >= 0 && scls->struct_layout && fidx < scls->struct_layout->field_count) {
                        XrStructFieldLayout *sf = &scls->struct_layout->fields[fidx];
                        uint8_t *fp = sptr + 8 + sf->offset;
                        switch (sf->native_type) {
                            case XR_NATIVE_I64:  R(a) = XR_FROM_INT(*(int64_t*)fp); break;
                            case XR_NATIVE_F64:  R(a) = XR_FROM_FLOAT(*(double*)fp); break;
                            case XR_NATIVE_BOOL: R(a).descriptor = 0; R(a).i = *(uint8_t*)fp ? 1 : 0; R(a).tag = XR_TAG_BOOL; break;
                            case XR_NATIVE_I32:  R(a) = XR_FROM_INT((int64_t)*(int32_t*)fp); break;
                            case XR_NATIVE_F32:  R(a) = XR_FROM_FLOAT((double)*(float*)fp); break;
                            case XR_NATIVE_STRING: {
                                XrString *s = *(XrString**)fp;
                                R(a) = s ? XR_FROM_STR(s) : xr_null();
                                break;
                            }
                            default: R(a) = xr_null(); break;
                        }
                        vmbreak;
                    }
                    // Field not found: might be a method, fall through to cold path
                }

                // Fast path: Instance is the most common target, skip if-else chain
                if (xr_value_is_instance(obj)) goto getprop_instance;

                // Json fast path with Shape IC
                if (xr_value_is_json(obj)) {
                    XrJson *json = xr_value_to_json(obj);
                    uint16_t shape_id = xr_gc_get_shape_id(&json->gc);

                    // Lazily ensure the per-ctx IC table for this proto.
                    XrICFieldTable *ic_table_j =
                        xr_vm_ctx_ensure_ic_fields(vm_ctx, frame->closure->proto);
                    if (!ic_table_j) {
                        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                                         "OP_GETPROP: failed to allocate IC table");
                    }
                    size_t jic_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
                    XrICField *jic = xr_ic_field_table_get(ic_table_j, (int)jic_index);

                    // IC hit: shape_id + symbol match → direct field access (in-object only)
                    uint16_t jic_idx;
                    if (jic && xr_ic_json_lookup(jic, shape_id, prop_symbol, &jic_idx)) {
                        R(a) = json->fields[jic_idx];
                        vmbreak;
                    }

                    // IC miss: full shape lookup
                    XrShape *shape = xr_shape_get_by_id(isolate, shape_id);
                    if (shape && shape->symbol_to_index &&
                        prop_symbol >= (int)shape->min_symbol &&
                        prop_symbol <= (int)shape->max_symbol) {
                        int idx = shape->symbol_to_index[prop_symbol - shape->min_symbol];
                        if (idx >= 0) {
                            if (idx < shape->in_object_capacity) {
                                R(a) = json->fields[idx];
                                // Update IC for next hit (in-object fields only)
                                if (jic) xr_ic_json_update(jic, shape_id, (uint16_t)idx, prop_symbol);
                                vmbreak;
                            }
                            // Overflow field: fall through to slow path
                        }
                    }

                    // Slow path: overflow field or field not found
                    R(a) = xr_json_get(isolate, json, prop_symbol);
                    vmbreak;
                }

                // Cold path: all non-instance, non-json type dispatch
                {
                    savepc();
                    int _cr = vm_getprop_type_dispatch(isolate, vm_ctx, obj, prop_symbol, base, a, b, frame, pc);
                    if (_cr == VM_COLD_BREAK) vmbreak;
                    if (_cr == VM_COLD_STARTFUNC) goto startfunc;
                    if (_cr == VM_COLD_ERROR) {
                        if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                        goto startfunc;
                    }
                    // VM_COLD_CONTINUE: fall through to instance path
                }

                getprop_instance: ;
                XrInstance *inst = xr_value_to_instance(obj);

                // Cold path: getter method lookup
                {
                    int _cr = vm_getprop_instance_getter(isolate, vm_ctx, inst, obj, prop_symbol, base, a, frame, pc);
                    if (_cr == VM_COLD_BREAK) vmbreak;
                    if (_cr == VM_COLD_STARTFUNC) goto startfunc;
                    if (_cr == VM_COLD_ERROR) {
                        if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                        goto startfunc;
                    }
                    // VM_COLD_CONTINUE: no getter, fall through to field access
                }

                // No getter: access as regular field

                // Field access Inline Cache optimization (per-ctx)
                XrICFieldTable *ic_table_g =
                    xr_vm_ctx_ensure_ic_fields(vm_ctx, frame->closure->proto);
                if (!ic_table_g) {
                    VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                                     "OP_GETPROP: failed to allocate IC table");
                }

                // Get IC for current instruction
                size_t cache_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
                XR_VM_IC_ASSERT_INDEX(cache_index, frame->closure->proto);
                XrICField *cache = xr_ic_field_table_get(ic_table_g, cache_index);
                if (cache) { XR_VM_IC_FIELD_BIND(cache, (int)cache_index); }

                XrClass *inst_class = inst->klass;
                int field_index = -1;

                // Fast path 1: Monomorphic IC hit (verify symbol match)
                if (cache && xr_ic_field_lookup_mono(cache, inst_class, prop_symbol, &field_index)) {
                    // Monomorphic hit: direct field access!
                    R(a) = inst->fields[field_index];
                    vmbreak;
                }

                // Fast path 2: Polymorphic IC hit (verify symbol match)
                if (cache && xr_ic_field_lookup_poly(cache, inst_class, prop_symbol, &field_index)) {
                    // Polymorphic hit: direct field access!
                    R(a) = inst->fields[field_index];
                    vmbreak;
                }

                // Slow path: Symbol lookup for field index
                field_index = xr_class_lookup_field(inst_class, prop_symbol);

                if (field_index >= 0) {
                    // Get instance field count for bounds check
                    uint32_t inst_field_count = xr_class_instance_field_count(inst_class);

                    // Bounds check: prevent out-of-bounds access
                    if ((uint32_t)field_index >= inst_field_count) {
                        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "field index out of bounds: %d >= %d", field_index, inst_field_count);
                    }

                    // Field exists: access and update IC
                    R(a) = inst->fields[field_index];

                    // Update IC cache (pass symbol)
                    if (cache) {
                        xr_ic_field_update(cache, inst_class, field_index, prop_symbol);
                    }
                } else {
                    // Field not found: try method lookup for method reference
                    XrMethod *_m = xr_class_lookup_method(inst_class, prop_symbol);
                    if (_m && _m->as.closure) {
                        R(a) = XR_FROM_PTR(_m->as.closure);
                    } else {
                        const char* class_name = inst_class->name ? inst_class->name : "?";
                        XrSymbolTable *_st2 = (XrSymbolTable*)isolate->symbol_table;
                        const char *_pn = xr_symbol_get_name_in_table(_st2, prop_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY, "field '%s' not declared in class '%s'",
                                         _pn ? _pn : "?", class_name);
                    }
                }
                vmbreak;
            }

            vmcase(OP_SETPROP) {
                /* === OP_SETPROP R[A].symbol(B) = R[C] ===
                ** Use Symbol direct dispatch, 10x performance improvement */
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);

                XrValue obj = R(a);
                int prop_symbol = PROTO_SYMBOL(cl->proto, b); // Dereference local index → global symbol
                XrValue value = R(c);

                // Json fast path with Shape IC (set existing field)
                if (xr_value_is_json(obj)) {
                    XrJson *json = xr_value_to_json(obj);
                    uint16_t shape_id = xr_gc_get_shape_id(&json->gc);

                    // Lazily ensure the per-ctx IC table for this proto.
                    XrICFieldTable *ic_table_sj =
                        xr_vm_ctx_ensure_ic_fields(vm_ctx, frame->closure->proto);
                    if (!ic_table_sj) {
                        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                                         "OP_SETPROP: failed to allocate IC table");
                    }
                    size_t jic_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
                    XrICField *jic = xr_ic_field_table_get(ic_table_sj, (int)jic_index);

                    // IC hit: shape_id + symbol match → direct field write (in-object only)
                    uint16_t jic_idx;
                    if (jic && xr_ic_json_lookup(jic, shape_id, prop_symbol, &jic_idx)) {
                        json->fields[jic_idx] = value;
                        XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), json);
                        vmbreak;
                    }

                    // IC miss: try inline fast path for existing in-object field
                    XrShape *shape = xr_shape_get_by_id(isolate, shape_id);
                    if (shape && shape->symbol_to_index &&
                        prop_symbol >= (int)shape->min_symbol &&
                        prop_symbol <= (int)shape->max_symbol) {
                        int idx = shape->symbol_to_index[prop_symbol - shape->min_symbol];
                        if (idx >= 0) {
                            if (idx < shape->in_object_capacity) {
                                json->fields[idx] = value;
                                XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), json);
                                if (jic) xr_ic_json_update(jic, shape_id, (uint16_t)idx, prop_symbol);
                                vmbreak;
                            }
                            // Overflow field: fall through to slow path
                        }
                    }

                    // Slow path: overflow field, new field addition
                    xr_json_set(isolate, json, prop_symbol, value);
                    XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), json);
                    vmbreak;
                }

                // Cold path: non-instance type dispatch (Map/Module/Class/null)
                {
                    savepc();
                    int _cr = vm_setprop_type_dispatch(isolate, vm_ctx, obj, prop_symbol, value, base, a, frame, pc);
                    if (_cr == VM_COLD_BREAK) vmbreak;
                    if (_cr == VM_COLD_STARTFUNC) goto startfunc;
                    if (_cr == VM_COLD_ERROR) {
                        if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                        goto startfunc;
                    }
                    // VM_COLD_CONTINUE: fall through to instance path
                }

                XrInstance *inst = xr_value_to_instance(obj);

                // Cold path: setter method lookup
                {
                    int _cr = vm_setprop_instance_setter(isolate, vm_ctx, inst, obj, prop_symbol, value, base, c, frame, pc);
                    if (_cr == VM_COLD_STARTFUNC) goto startfunc;
                    if (_cr == VM_COLD_ERROR) {
                        if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                        goto startfunc;
                    }
                    // VM_COLD_CONTINUE: no setter, fall through to field access
                }

                // No setter: assign as regular field
                XrClass *inst_class = inst->klass;

                // Field access Inline Cache optimization (per-ctx)
                XrICFieldTable *ic_table_s =
                    xr_vm_ctx_ensure_ic_fields(vm_ctx, frame->closure->proto);
                if (!ic_table_s) {
                    VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                                     "OP_SETPROP: failed to allocate IC table");
                }

                size_t cache_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
                XR_VM_IC_ASSERT_INDEX(cache_index, frame->closure->proto);
                XrICField *cache = xr_ic_field_table_get(ic_table_s, cache_index);
                if (cache) { XR_VM_IC_FIELD_BIND(cache, (int)cache_index); }

                int field_index = -1;

                // Fast path 1: Monomorphic IC hit (verify symbol match)
                if (cache && xr_ic_field_lookup_mono(cache, inst_class, prop_symbol, &field_index)) {
                    inst->fields[field_index] = value;
                    VM_BARRIER_VAL(inst, value);
                    vmbreak;
                }

                // Fast path 2: Polymorphic IC hit (verify symbol match)
                if (cache && xr_ic_field_lookup_poly(cache, inst_class, prop_symbol, &field_index)) {
                    inst->fields[field_index] = value;
                    VM_BARRIER_VAL(inst, value);
                    vmbreak;
                }

                // Slow path: Symbol lookup for field index
                field_index = xr_class_lookup_field(inst_class, prop_symbol);

                if (field_index >= 0) {
                    inst->fields[field_index] = value;
                    VM_BARRIER_VAL(inst, value);

                    // Update IC cache (pass symbol)
                    if (cache) {
                        xr_ic_field_update(cache, inst_class, field_index, prop_symbol);
                    }
                } else {
                    // Field not found: generate error message
                    XrSymbolTable *_st2 = (XrSymbolTable*)isolate->symbol_table;
                    const char *_pn = xr_symbol_get_name_in_table(_st2, prop_symbol);
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY, "field '%s' not declared in class '%s'",
                                     _pn ? _pn : "?", inst_class->name);
                }
                vmbreak;
            }

            vmcase(OP_GETSUPER) {
                // GETSUPER: Get superclass method (not implemented)
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "OP_GETSUPER not yet implemented");
            }

            /* Exception opcodes — see xvm_dispatch_exception.inc.c. */
            #include "xvm_dispatch_exception.inc.c"

            /* === Register spill instructions (fully inlined) === */
            vmcase(OP_SPILL) {
                /* ========== OP_SPILL - Register spill to stack frame spill slot (fully inlined) ========== */
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
        } // end switch
        // Next instruction read at loop start: i = *pc++
    } // end for

    #undef vmcase
    #undef vmbreak
#endif // ========== Closure Pending Handler ==========
    /* Jumped to from OP_RETURN/RETURN0/RETURN1 when caller frame has
     * XR_CALL_CLOSURE_PENDING set by xr_yield_call_closure(). */
handle_closure_pending: {
        XrCoroutine *_pcoro = (XrCoroutine *)vm_ctx->current_coro;
        XrContinuation _cont = (XrContinuation)ci->u.c.continuation;
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
