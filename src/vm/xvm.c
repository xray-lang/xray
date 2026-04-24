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
#include "../../stdlib/regex/xregex.h"
#include "../../stdlib/regex/xregex_binding.h"
#include "../coro/xchannel.h"
#include "../coro/xdeep_copy.h"
#include "../runtime/object/xbigint.h"
#include "../runtime/object/xrange.h"
#include "../runtime/object/xutf8.h"       // XR_UNICODE_MAX
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xstruct_layout.h"
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
        xr_vm_add_stacktrace(isolate, _exc); \
        xr_vm_throw_exception(isolate, _exc); \
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

    vm_profiler_init();
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

    // Allocate struct_area for native struct storage
    if (cl->proto->struct_area_size > 0) {
        int _sa_idx = VM_FRAME_COUNT - 1;
        if (!vm_ctx->struct_areas) {
            int cap = vm_ctx->frame_capacity;
            vm_ctx->struct_areas = (uint8_t**)xr_calloc(cap, sizeof(uint8_t*));
            vm_ctx->struct_area_caps = (uint16_t*)xr_calloc(cap, sizeof(uint16_t));
            vm_ctx->struct_areas_cap = cap;
        } else if (_sa_idx >= vm_ctx->struct_areas_cap) {
            int old_cap = vm_ctx->struct_areas_cap;
            int new_cap = vm_ctx->frame_capacity;
            if (new_cap <= _sa_idx) new_cap = _sa_idx + 1;
            vm_ctx->struct_areas = (uint8_t**)xr_realloc(vm_ctx->struct_areas, new_cap * sizeof(uint8_t*));
            vm_ctx->struct_area_caps = (uint16_t*)xr_realloc(vm_ctx->struct_area_caps, new_cap * sizeof(uint16_t));
            memset(vm_ctx->struct_areas + old_cap, 0, (new_cap - old_cap) * sizeof(uint8_t*));
            memset(vm_ctx->struct_area_caps + old_cap, 0, (new_cap - old_cap) * sizeof(uint16_t));
            vm_ctx->struct_areas_cap = new_cap;
        }
        uint16_t need = cl->proto->struct_area_size;
        if (vm_ctx->struct_area_caps[_sa_idx] < need) {
            xr_free(vm_ctx->struct_areas[_sa_idx]);
            vm_ctx->struct_areas[_sa_idx] = (uint8_t*)xr_calloc(1, need);
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

            /* ========================================================
            ** Enum Operation Instructions
            ** ======================================================== */

            vmcase(OP_ENUM_ACCESS) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrEnumType *enum_type = (XrEnumType*)XR_TO_PTR(R(b));
                int member_index = (int)XR_TO_INT(R(c));
                if (member_index < 0 || (uint32_t)member_index >= enum_type->member_count) {
                    VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "enum member index out of bounds");
                }
                R(a) = XR_FROM_PTR(enum_type->members[member_index].instance);
                vmbreak;
            }

            vmcase(OP_ENUM_CONVERT) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrEnumType *enum_type = (XrEnumType*)XR_TO_PTR(R(b));
                XrEnumValue *result = xr_enum_from_value(enum_type, R(c));
                R(a) = result ? XR_FROM_PTR(result) : xr_null();
                vmbreak;
            }

            vmcase(OP_ENUM_NAME) {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue enum_val = R(b);
                if (!XR_IS_PTR(enum_val)) {
                    R(a) = xr_null();
                    vmbreak;
                }
                XrGCHeader *gc = (XrGCHeader*)XR_TO_PTR(enum_val);
                if (XR_GC_GET_TYPE(gc) != XR_TENUM_VALUE) {
                    R(a) = xr_null();
                    vmbreak;
                }
                XrEnumValue *ev = (XrEnumValue*)gc;
                size_t len = strlen(ev->member_name);
                uint32_t hash = xr_string_hash(ev->member_name, len);
                XrString *name_str = xr_string_intern(isolate, ev->member_name, len, hash);
                R(a) = xr_string_value(name_str);
                vmbreak;
            }

            /* ========================================================
            ** Container Creation Instructions
            ** ======================================================== */

            vmcase(OP_NEWARRAY) {
                /* OP_NEWARRAY: create array
                ** A = destination register
                ** B = capacity/initial element count
                ** C = (elem_tid << 2) | storage_mode
                **     storage_mode: bits 0-1 (0=normal, 1=shared)
                **     elem_tid:     bits 2-6 (XrTypeId, 0=any)
                */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c_field = GETARG_C(i);
                int storage_mode = c_field & 0x03;
                uint8_t elem_tid = (uint8_t)((c_field >> 2) & 0x1F);
                uint8_t elem_type = xr_tid_to_elem_type(elem_tid);

                XrArray *array;
                if (storage_mode != 0 && isolate->sys_heap) {
                    // shared: allocate on system heap
                    array = (XrArray*)xr_sysheap_alloc_shared(isolate->sys_heap,
                        sizeof(XrArray), XR_TARRAY);
                    if (array) {
                        xr_array_init_inplace(array, b > 0 ? b : 4, elem_type);
                        // Set storage mode
                        XR_GC_SET_STORAGE(&array->gc, storage_mode);
                        if (storage_mode == XR_GC_STORAGE_SHARED) {
                            xr_shared_set_refc(&array->gc, 1);
                        }
                    }
                } else {
                    // normal: allocate on coroutine heap
                    if (elem_type != XR_ELEM_ANY) {
                        array = xr_array_with_capacity_typed(VM_CURRENT_CORO, b > 0 ? b : 4, elem_type);
                    } else {
                        array = (b > 0) ? xr_array_with_capacity(VM_CURRENT_CORO, b) : xr_array_new(VM_CURRENT_CORO);
                    }
                }

                if (array) {
                    array->elem_tid = elem_tid;
                    for (int j = 0; j < b; j++) {
                        xr_array_push(array, R(a + 1 + j));
                    }
                }
                R(a) = xr_value_from_array(array);
                if (storage_mode == 0) checkGC(base + a + 1);
                vmbreak;
            }

            vmcase(OP_NEWMAP) {
                /* OP_NEWMAP: create Map
                ** A = destination register
                ** B = capacity hint
                ** C = (key_kind << 7) | (value_tid << 2) | flags
                **     flags bit0: shared, bit1: weak
                **     value_tid: bits 2-6 (5 bits, XrTypeId 0-31)
                **     key_kind:  bits 7-8 (2 bits: 0=any, 1=string, 2=int)
                */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                int storage_mode = c & 0x01;
                int is_weak = c & 0x02;
                uint8_t value_tid = (uint8_t)((c >> 2) & 0x1F);
                int key_kind = (c >> 7) & 0x03;
                uint8_t key_tid = (key_kind == 1) ? XR_TID_STRING :
                                  (key_kind == 2) ? XR_TID_INT : 0;

                XrMap *map;
                if (storage_mode != 0 && isolate->sys_heap) {
                    // shared: allocate on system heap
                    map = (XrMap*)xr_sysheap_alloc_shared(isolate->sys_heap,
                        sizeof(XrMap), XR_TMAP);
                    if (map) {
                        xr_map_init_inplace(map, b > 0 ? b : 8);
                        // Set storage mode
                        XR_GC_SET_STORAGE(&map->gc, storage_mode);
                        if (storage_mode == XR_GC_STORAGE_SHARED) {
                            xr_shared_set_refc(&map->gc, 1);
                        }
                    }
                } else {
                    // normal: allocate on coroutine heap
                    map = (b > 0) ? xr_map_with_capacity(VM_CURRENT_CORO, b) : xr_map_new(VM_CURRENT_CORO);
                }

                if (map) {
                    if (is_weak) map->flags |= XR_MAP_FLAG_WEAK;
                    map->key_tid = key_tid;
                    map->value_tid = value_tid;
                }

                R(a) = xr_value_from_map(map);
                if (storage_mode == 0) checkGC(base + a + 1);
                vmbreak;
            }

            vmcase(OP_NEWSET) {
                /* OP_NEWSET: create Set
                ** A = destination register
                ** B = (elem_tid << 2) | flags
                **     flags bit0: shared, bit1: weak
                **     elem_tid: bits 2-6 (XrTypeId, 0=any)
                ** C = init mode (0=empty, 1=from array in R[A+1])
                */
                int a = GETARG_A(i);
                int b_arg = GETARG_B(i);
                int init_mode = GETARG_C(i);
                int storage_mode = b_arg & 0x01;
                int is_weak = b_arg & 0x02;
                uint8_t elem_tid = (uint8_t)((b_arg >> 2) & 0x1F);

                XrSet *set;
                if (init_mode == 1 && XR_IS_ARRAY(R(a + 1)) && storage_mode != 0 && isolate->sys_heap) {
                    // Initialize from array on system heap (shared)
                    set = (XrSet*)xr_sysheap_alloc_shared(isolate->sys_heap,
                        sizeof(XrSet), XR_TSET);
                    if (set) {
                        xr_set_init_inplace(set);
                        XR_GC_SET_STORAGE(&set->gc, storage_mode);
                        xr_shared_set_refc(&set->gc, 1);
                        XrArray *arr = XR_TO_ARRAY(R(a + 1));
                        int32_t len = arr->length;
                        XrValue *elems = (XrValue*)arr->data;
                        for (int32_t j = 0; j < len; j++) xr_set_add(set, elems[j]);
                    }
                } else if (init_mode == 1 && XR_IS_ARRAY(R(a + 1))) {
                    // Initialize from array on coroutine heap
                    XrArray *arr = XR_TO_ARRAY(R(a + 1));
                    set = xr_set_from_array(xr_current_coro(isolate), arr);
                } else if (storage_mode != 0 && isolate->sys_heap) {
                    // shared: allocate on system heap
                    set = (XrSet*)xr_sysheap_alloc_shared(isolate->sys_heap,
                        sizeof(XrSet), XR_TSET);
                    if (set) {
                        xr_set_init_inplace(set);
                        XR_GC_SET_STORAGE(&set->gc, storage_mode);
                        if (storage_mode == XR_GC_STORAGE_SHARED) {
                            xr_shared_set_refc(&set->gc, 1);
                        }
                    }
                } else {
                    // normal: allocate on coroutine heap
                    set = xr_set_new(VM_CURRENT_CORO);
                }

                if (set) {
                    if (is_weak) set->flags |= XR_SET_FLAG_WEAK;
                    set->elem_tid = elem_tid;
                }

                R(a) = xr_value_from_set(set);
                if (storage_mode == 0) checkGC(base + a + 1);
                vmbreak;
            }

            vmcase(OP_NEWRANGE) {
                /* OP_NEWRANGE: create lazy Range object
                ** R[A] = Range(R[B], R[C])
                */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                int64_t start_val = XR_TO_INT(R(b));
                int64_t end_val = XR_TO_INT(R(c));
                XrRange *range = xr_range_new(VM_CURRENT_CORO, start_val, end_val);
                R(a) = xr_value_from_range(range);
                checkGC(base + a + 1);
                vmbreak;
            }

            vmcase(OP_RANGE_UNPACK) {
                /* OP_RANGE_UNPACK: extract Range fields for standard loop
                ** R[A]   = start (loop variable initial value)
                ** R[A+1] = end   (loop termination bound)
                ** R[A+2] = step
                ** B = source register containing Range object
                */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue rv = R(b);
                if (!XR_IS_RANGE(rv)) {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "for-in expected Range object");
                }
                XrRange *rng = xr_value_to_range(rv);
                XR_DCHECK(rng != NULL, "OP_RANGE_UNPACK: Range pointer is NULL");
                XR_DCHECK(rng->step != 0, "OP_RANGE_UNPACK: Range step is zero");
                R(a)     = xr_int(rng->start);              // start (loop variable)
                R(a + 1) = xr_int(rng->end);                // end (bound)
                R(a + 2) = xr_int(rng->step);               // step
                vmbreak;
            }

            vmcase(OP_NEWSTRINGBUILDER) {
                /* OP_NEWSTRINGBUILDER: create StringBuilder
                ** A = destination register
                ** B = storage mode (0=normal, 1=shared)
                */
                int a = GETARG_A(i);
                int storage_mode = GETARG_B(i);

                XrStringBuilder *sb;
                if (storage_mode != 0 && isolate->sys_heap) {
                    // shared: allocate on system heap
                    sb = (XrStringBuilder*)xr_sysheap_alloc_shared(isolate->sys_heap,
                        sizeof(XrStringBuilder), XR_TSTRINGBUILDER);
                    if (sb) {
                        xr_stringbuilder_init_inplace(sb);
                        XR_GC_SET_STORAGE(&sb->gc, storage_mode);
                        if (storage_mode == XR_GC_STORAGE_SHARED) {
                            xr_shared_set_refc(&sb->gc, 1);
                        }
                    }
                } else {
                    // normal: allocate on coroutine heap
                    sb = xr_stringbuilder_new(VM_CURRENT_CORO);
                }

                R(a) = xr_stringbuilder_value(sb);
                if (storage_mode == 0) checkGC(base + a + 1);
                vmbreak;
            }

            /* ========================================================
            ** Array Operation Instructions
            ** ======================================================== */

            vmcase(OP_ARRAY_GET) {
                // OP_ARRAY_GET: array dynamic index read
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue obj_val = R(b);
                // Struct inline fixed array dynamic index
                if (XR_IS_ARRAY_REF(obj_val)) {
                    uint8_t etype = XR_ARRAY_REF_ELEM_TYPE(obj_val);
                    uint16_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
                    int idx = (int)XR_TO_INT(R(c));
                    if ((unsigned)idx < (unsigned)ecount) {
                        uint8_t *bp = (uint8_t*)obj_val.ptr;
                        uint8_t es = xr_native_type_size(etype);
                        uint8_t *ep = bp + idx * es;
                        switch (etype) {
                            case XR_NATIVE_I64:  R(a) = XR_FROM_INT(*(int64_t*)ep); break;
                            case XR_NATIVE_F64:  R(a) = XR_FROM_FLOAT(*(double*)ep); break;
                            case XR_NATIVE_BOOL: R(a) = *(uint8_t*)ep ? XR_TRUE_VAL : XR_FALSE_VAL; break;
                            case XR_NATIVE_I32:  R(a) = XR_FROM_INT((int64_t)*(int32_t*)ep); break;
                            case XR_NATIVE_U32:  R(a) = XR_FROM_INT((int64_t)*(uint32_t*)ep); break;
                            case XR_NATIVE_I16:  R(a) = XR_FROM_INT((int64_t)*(int16_t*)ep); break;
                            case XR_NATIVE_U16:  R(a) = XR_FROM_INT((int64_t)*(uint16_t*)ep); break;
                            case XR_NATIVE_I8:   R(a) = XR_FROM_INT((int64_t)*(int8_t*)ep); break;
                            case XR_NATIVE_U8:   R(a) = XR_FROM_INT((int64_t)*(uint8_t*)ep); break;
                            case XR_NATIVE_F32:  R(a) = XR_FROM_FLOAT((double)*(float*)ep); break;
                            default: R(a) = xr_null(); break;
                        }
                    } else {
                        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                            "fixed array index out of range: %d (length %u)", idx, (unsigned)ecount);
                    }
                    vmbreak;
                }
                if (!XR_IS_ARRAY(obj_val)) {
                    if (xr_value_is_instance(obj_val)) {
                        XrInstance *_inst = xr_value_to_instance(obj_val);
                        XrClass *_cls = xr_instance_get_class(_inst);
                        if (XCLASS_HAS_OP(_cls, XR_OP_INDEX_FLAG)) {
                            XrMethod *_m = xr_class_lookup_method(_cls, SYMBOL_OP_INDEX);
                            if (_m && _m->type == XMETHOD_OPERATOR && _m->as.closure) {
                                XrClosure *_cl = _m->as.closure;
                                XrProto *_p = _cl->proto;
                                R(a + 1) = obj_val;
                                R(a + 2) = R(c);
                                savepc();
                                int _fi = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                                XrBcCallFrame *_nf = &VM_FRAMES[_fi];
                                _nf->closure = _cl;
                                _nf->pc = PROTO_CODE_BASE(_p);
                                _nf->base_offset = (int)((base + a + 1) - VM_STACK);
                                goto startfunc;
                            }
                        }
                    }
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "only Array supports dynamic indexing");
                }
                XrArray *arr = XR_TO_ARRAY(obj_val);
                int idx = (int)XR_TO_INT(R(c));
                if ((unsigned)idx < (unsigned)arr->length) {
                    R(a) = (arr->elem_type == XR_ELEM_ANY)
                        ? ((XrValue*)arr->data)[idx]
                        : xr_array_get_element(arr, idx);
                } else {
                    R(a) = xr_null();
                }
                vmbreak;
            }

            vmcase(OP_ARRAY_GETC) {
                // OP_ARRAY_GETC: array/string constant index read
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue obj_val = R(b);
                // Struct inline fixed array constant index
                if (XR_IS_ARRAY_REF(obj_val)) {
                    uint8_t etype = XR_ARRAY_REF_ELEM_TYPE(obj_val);
                    uint16_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
                    if ((unsigned)c < (unsigned)ecount) {
                        uint8_t *base_ptr = (uint8_t*)obj_val.ptr;
                        uint8_t es = xr_native_type_size(etype);
                        uint8_t *ep = base_ptr + c * es;
                        switch (etype) {
                            case XR_NATIVE_I64:  R(a) = XR_FROM_INT(*(int64_t*)ep); break;
                            case XR_NATIVE_F64:  R(a) = XR_FROM_FLOAT(*(double*)ep); break;
                            case XR_NATIVE_BOOL: R(a) = *(uint8_t*)ep ? XR_TRUE_VAL : XR_FALSE_VAL; break;
                            case XR_NATIVE_I32:  R(a) = XR_FROM_INT((int64_t)*(int32_t*)ep); break;
                            case XR_NATIVE_U32:  R(a) = XR_FROM_INT((int64_t)*(uint32_t*)ep); break;
                            case XR_NATIVE_I16:  R(a) = XR_FROM_INT((int64_t)*(int16_t*)ep); break;
                            case XR_NATIVE_U16:  R(a) = XR_FROM_INT((int64_t)*(uint16_t*)ep); break;
                            case XR_NATIVE_I8:   R(a) = XR_FROM_INT((int64_t)*(int8_t*)ep); break;
                            case XR_NATIVE_U8:   R(a) = XR_FROM_INT((int64_t)*(uint8_t*)ep); break;
                            case XR_NATIVE_F32:  R(a) = XR_FROM_FLOAT((double)*(float*)ep); break;
                            default: R(a) = xr_null(); break;
                        }
                    } else {
                        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                            "fixed array index out of range: %d (length %u)", c, (unsigned)ecount);
                    }
                    vmbreak;
                }
                // String indexing support
                if (XR_IS_STRING(obj_val)) {
                    XrString *str = XR_TO_STRING(obj_val);
                    XrString *ch = xr_string_char_at_unicode(isolate, str, (size_t)c);
                    R(a) = ch ? xr_string_value(ch) : xr_null();
                    vmbreak;
                }
                // Array / ArraySlice indexing
                if (XR_IS_ARRAY_OR_SLICE(obj_val)) {
                    XrArray *arr = XR_TO_ARRAY(obj_val);
                    if (c < arr->length) {
                        R(a) = (arr->elem_type == XR_ELEM_ANY)
                            ? ((XrValue*)arr->data)[c]
                            : xr_array_get_element(arr, c);
                    } else {
                        R(a) = xr_null();
                    }
                    vmbreak;
                }
                // Range constant index
                if (XR_IS_RANGE(obj_val)) {
                    XrRange *rng = xr_value_to_range(obj_val);
                    int64_t len = xr_range_length(rng);
                    if (c >= 0 && c < len) {
                        R(a) = xr_int(rng->start + (int64_t)c * rng->step);
                    } else {
                        R(a) = xr_null();
                    }
                    vmbreak;
                }
                // Operator overload: operator[]
                if (xr_value_is_instance(obj_val)) {
                    XrInstance *_inst = xr_value_to_instance(obj_val);
                    XrClass *_cls = xr_instance_get_class(_inst);
                    if (XCLASS_HAS_OP(_cls, XR_OP_INDEX_FLAG)) {
                        XrMethod *_m = xr_class_lookup_method(_cls, SYMBOL_OP_INDEX);
                        if (_m && _m->type == XMETHOD_OPERATOR && _m->as.closure) {
                            XrClosure *_cl = _m->as.closure;
                            XrProto *_p = _cl->proto;
                            R(a + 1) = obj_val;
                            R(a + 2) = xr_int(c);
                            savepc();
                            int _fi = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                            XrBcCallFrame *_nf = &VM_FRAMES[_fi];
                            _nf->closure = _cl;
                            _nf->pc = PROTO_CODE_BASE(_p);
                            _nf->base_offset = (int)((base + a + 1) - VM_STACK);
                            goto startfunc;
                        }
                    }
                }
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "only Array, String support constant indexing");
            }

            vmcase(OP_ARRAY_SET) {
                // OP_ARRAY_SET: array dynamic index write
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue obj_val = R(a);
                // Struct inline fixed array dynamic index write
                if (XR_IS_ARRAY_REF(obj_val)) {
                    uint8_t etype = XR_ARRAY_REF_ELEM_TYPE(obj_val);
                    uint16_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
                    int idx = (int)XR_TO_INT(R(b));
                    if ((unsigned)idx < (unsigned)ecount) {
                        uint8_t *bp = (uint8_t*)obj_val.ptr;
                        uint8_t es = xr_native_type_size(etype);
                        uint8_t *ep = bp + idx * es;
                        XrValue _av = R(c);
                        switch (etype) {
                            case XR_NATIVE_I64:  *(int64_t*)ep  = XR_TO_INT(_av); break;
                            case XR_NATIVE_F64:  *(double*)ep   = XR_TO_FLOAT(_av); break;
                            case XR_NATIVE_BOOL: *(uint8_t*)ep  = (uint8_t)_av.i; break;
                            case XR_NATIVE_I32:  *(int32_t*)ep  = (int32_t)XR_TO_INT(_av); break;
                            case XR_NATIVE_U32:  *(uint32_t*)ep = (uint32_t)XR_TO_INT(_av); break;
                            case XR_NATIVE_I16:  *(int16_t*)ep  = (int16_t)XR_TO_INT(_av); break;
                            case XR_NATIVE_U16:  *(uint16_t*)ep = (uint16_t)XR_TO_INT(_av); break;
                            case XR_NATIVE_I8:   *(int8_t*)ep   = (int8_t)XR_TO_INT(_av); break;
                            case XR_NATIVE_U8:   *(uint8_t*)ep  = (uint8_t)XR_TO_INT(_av); break;
                            case XR_NATIVE_F32:  *(float*)ep    = (float)XR_TO_FLOAT(_av); break;
                            default: break;
                        }
                    } else {
                        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                            "fixed array index out of range: %d (length %u)", idx, (unsigned)ecount);
                    }
                    vmbreak;
                }
                if (XR_IS_ARRAY_OR_SLICE(obj_val)) {
                    XrArray *arr = XR_TO_ARRAY(obj_val);
                    int idx = (int)XR_TO_INT(R(b));
                    XrValue _av = R(c);
                    if ((unsigned)idx < (unsigned)arr->length) {
                        if (arr->elem_type == XR_ELEM_ANY) {
                            ((XrValue*)arr->data)[idx] = _av;
                            XR_ARRAY_MARK_GC_PTRS(arr, _av);
                            VM_BARRIER_VAL(arr, _av);
                        } else {
                            xr_array_set_element(arr, idx, _av);
                        }
                    }
                    vmbreak;
                }
                if (xr_value_is_instance(obj_val)) {
                    XrInstance *_inst = xr_value_to_instance(obj_val);
                    XrClass *_cls = xr_instance_get_class(_inst);
                    if (XCLASS_HAS_OP(_cls, XR_OP_INDEX_SET_FLAG)) {
                        XrMethod *_m = xr_class_lookup_method(_cls, SYMBOL_OP_INDEX_SET);
                        if (_m && _m->type == XMETHOD_OPERATOR && _m->as.closure) {
                            XrClosure *_cl = _m->as.closure;
                            XrProto *_p = _cl->proto;
                            XrValue _key = R(b), _val = R(c);
                            R(a + 2) = obj_val;
                            R(a + 3) = _key;
                            R(a + 4) = _val;
                            savepc();
                            int _fi = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                            XrBcCallFrame *_nf = &VM_FRAMES[_fi];
                            _nf->closure = _cl;
                            _nf->pc = PROTO_CODE_BASE(_p);
                            _nf->base_offset = (int)((base + a + 2) - VM_STACK);
                            goto startfunc;
                        }
                    }
                }
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "only Array support dynamic index assignment");
            }

            vmcase(OP_ARRAY_SETC) {
                // OP_ARRAY_SETC: array constant index write
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue obj_val = R(a);
                // Struct inline fixed array constant index write
                if (XR_IS_ARRAY_REF(obj_val)) {
                    uint8_t etype = XR_ARRAY_REF_ELEM_TYPE(obj_val);
                    uint16_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
                    if ((unsigned)b < (unsigned)ecount) {
                        uint8_t *bp = (uint8_t*)obj_val.ptr;
                        uint8_t es = xr_native_type_size(etype);
                        uint8_t *ep = bp + b * es;
                        XrValue _acv = R(c);
                        switch (etype) {
                            case XR_NATIVE_I64:  *(int64_t*)ep  = XR_TO_INT(_acv); break;
                            case XR_NATIVE_F64:  *(double*)ep   = XR_TO_FLOAT(_acv); break;
                            case XR_NATIVE_BOOL: *(uint8_t*)ep  = (uint8_t)_acv.i; break;
                            case XR_NATIVE_I32:  *(int32_t*)ep  = (int32_t)XR_TO_INT(_acv); break;
                            case XR_NATIVE_U32:  *(uint32_t*)ep = (uint32_t)XR_TO_INT(_acv); break;
                            case XR_NATIVE_I16:  *(int16_t*)ep  = (int16_t)XR_TO_INT(_acv); break;
                            case XR_NATIVE_U16:  *(uint16_t*)ep = (uint16_t)XR_TO_INT(_acv); break;
                            case XR_NATIVE_I8:   *(int8_t*)ep   = (int8_t)XR_TO_INT(_acv); break;
                            case XR_NATIVE_U8:   *(uint8_t*)ep  = (uint8_t)XR_TO_INT(_acv); break;
                            case XR_NATIVE_F32:  *(float*)ep    = (float)XR_TO_FLOAT(_acv); break;
                            default: break;
                        }
                    } else {
                        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                            "fixed array index out of range: %d (length %u)", b, (unsigned)ecount);
                    }
                    vmbreak;
                }
                if (XR_IS_ARRAY_OR_SLICE(obj_val)) {
                    XrArray *arr = XR_TO_ARRAY(obj_val);
                    XrValue _acv = R(c);
                    if (b < arr->length) {
                        if (arr->elem_type == XR_ELEM_ANY) {
                            ((XrValue*)arr->data)[b] = _acv;
                            XR_ARRAY_MARK_GC_PTRS(arr, _acv);
                            VM_BARRIER_VAL(arr, _acv);
                        } else {
                            xr_array_set_element(arr, b, _acv);
                        }
                    }
                    vmbreak;
                }
                if (xr_value_is_instance(obj_val)) {
                    XrInstance *_inst = xr_value_to_instance(obj_val);
                    XrClass *_cls = xr_instance_get_class(_inst);
                    if (XCLASS_HAS_OP(_cls, XR_OP_INDEX_SET_FLAG)) {
                        XrMethod *_m = xr_class_lookup_method(_cls, SYMBOL_OP_INDEX_SET);
                        if (_m && _m->type == XMETHOD_OPERATOR && _m->as.closure) {
                            XrClosure *_cl = _m->as.closure;
                            XrProto *_p = _cl->proto;
                            XrValue _val = R(c);
                            R(a + 2) = obj_val;
                            R(a + 3) = xr_int(b);
                            R(a + 4) = _val;
                            savepc();
                            int _fi = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                            XrBcCallFrame *_nf = &VM_FRAMES[_fi];
                            _nf->closure = _cl;
                            _nf->pc = PROTO_CODE_BASE(_p);
                            _nf->base_offset = (int)((base + a + 2) - VM_STACK);
                            goto startfunc;
                        }
                    }
                }
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "only Array, Bytes, TypedArray support constant index assignment");
            }

            vmcase(OP_ARRAY_PUSH) {
                // OP_ARRAY_PUSH: array push
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrArray *arr = XR_TO_ARRAY(R(a));
                XrValue val = R(b);
                if (arr->length >= arr->capacity) xr_array_grow(arr);
                if (arr->elem_type == XR_ELEM_ANY) {
                    ((XrValue*)arr->data)[arr->length++] = val;
                    XR_ARRAY_MARK_GC_PTRS(arr, val);
                    XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), arr);
                } else {
                    xr_array_set_element(arr, arr->length++, val);
                }
                vmbreak;
            }

            vmcase(OP_ARRAY_LEN) {
                // OP_ARRAY_LEN: array length
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrArray *arr = XR_TO_ARRAY(R(b));
                R(a) = xr_int((xr_Integer)arr->length);
                vmbreak;
            }

            vmcase(OP_ARRAY_INIT) {
                /* OP_ARRAY_INIT: batch initialization
                ** A = array register, B = element count
                ** Elements are in R(A+1) .. R(A+B)
                ** Precondition: array already has capacity >= B (from OP_NEWARRAY)
                */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrArray *arr = XR_TO_ARRAY(R(a));
                if (arr->elem_type == XR_ELEM_ANY && b > 0) {
                    // Fast path: bulk memcpy for ANY arrays
                    memcpy(arr->data, &R(a + 1), (size_t)b * sizeof(XrValue));
                    arr->length = b;
                    // Scan for GC pointers in the batch
                    XrValue *src = &R(a + 1);
                    for (int j = 0; j < b; j++) {
                        if (XR_VALUE_NEEDS_GC(src[j])) {
                            arr->has_gc_ptrs = 1;
                            break;
                        }
                    }
                    if (arr->has_gc_ptrs)
                        XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), arr);
                } else {
                    // Slow path: typed arrays need per-element unboxing
                    for (int j = 1; j <= b; j++) {
                        xr_array_set(arr, j - 1, R(a + j));
                    }
                }
                vmbreak;
            }

            /* ========================================================
            ** Map Operation Instructions
            ** ======================================================== */

            vmcase(OP_MAP_GET) {
                // OP_MAP_GET: Map dynamic key access
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue map_val = R(b);
                XrValue key_val = R(c);

                if (XR_IS_MAP(map_val)) {
                    XrMap *map = XR_TO_MAP(map_val);
                    bool found;
                    R(a) = xr_map_get(map, key_val, &found);
                    if (!found) R(a) = xr_null();
                    vmbreak;
                }
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "Map.get requires Map type");
            }

            vmcase(OP_MAP_GETK) {
                // OP_MAP_GETK: Map/Json constant key access
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue map_val = R(b);

                if (XR_IS_MAP(map_val)) {
                    XrMap *map = XR_TO_MAP(map_val);
                    XrValue key_val = k[c];
                    XrString *key_str = XR_TO_STRING(key_val);
                    bool found;
                    XR_MAP_GET_STRING_FAST(map, key_str, R(a), found);
                    (void)found;
                    vmbreak;
                }
                // Json object support
                if (xr_value_is_json(map_val)) {
                    XrJson *json = xr_value_to_json(map_val);
                    XrValue key_val = k[c];
                    XrString *key_str = XR_TO_STRING(key_val);
                    R(a) = xr_json_get_by_key(isolate, json, key_str->data);
                    vmbreak;
                }
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "index access requires Map or Json type");
            }

            vmcase(OP_MAP_SET) {
                // OP_MAP_SET: Map dynamic key set
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue map_val = R(a);

                if (XR_IS_MAP(map_val)) {
                    XrMap *map = XR_TO_MAP(map_val);
                    xr_map_set(map, R(b), R(c));
                    VM_BARRIER_BACK(map);
                    vmbreak;
                }
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "Map.set requires Map type");
            }

            vmcase(OP_MAP_SETK) {
                // OP_MAP_SETK: Map/Json constant key set
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue map_val = R(a);

                if (XR_IS_MAP(map_val)) {
                    XrMap *map = XR_TO_MAP(map_val);
                    XrValue key_val = k[b];
                    XrString *key_str = XR_TO_STRING(key_val);
                    XR_MAP_SET_STRING_FAST(map, key_str, key_val, R(c));
                    VM_BARRIER_BACK(map);
                    vmbreak;
                }
                // Json object support
                if (xr_value_is_json(map_val)) {
                    XrJson *json = xr_value_to_json(map_val);
                    XrValue key_val = k[b];
                    XrString *key_str = XR_TO_STRING(key_val);
                    xr_json_set_by_key(isolate, json, key_str->data, R(c));
                    VM_BARRIER_BACK(json);
                    vmbreak;
                }
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "index assignment requires Map or Json type");
            }

            vmcase(OP_MAP_INCREMENT) {
                /* OP_MAP_INCREMENT: Map counter pattern optimization
                ** R[A]:Map[R[B]]++ - if key doesn't exist set to 1, otherwise +1
                ** Replaces: if (map.has(key)) { map[key] = map[key] + 1 } else { map[key] = 1 }
                */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue map_val = R(a);
                XrValue key = R(b);

                if (XR_IS_MAP(map_val)) {
                    XrMap *map = XR_TO_MAP(map_val);
                    bool found = false;
                    XrValue old_val = xr_map_get(map, key, &found);

                    if (found && XR_IS_INT(old_val)) {
                        // Exists and is integer, +1
                        xr_map_set(map, key, xr_int(XR_TO_INT(old_val) + 1));
                    } else {
                        // Doesn't exist or not integer, set to 1
                        xr_map_set(map, key, xr_int(1));
                    }
                    VM_BARRIER_BACK(map);
                    vmbreak;
                }
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "Map increment requires Map type");
            }

            vmcase(OP_SUBSTRING) {
                /* OP_SUBSTRING: inline string substring
                ** R[A] = R[B].substring(R[C], R[C+1])
                **
                ** Layout: B=string (any position), C=start, C+1=end
                **
                ** Optimization: string doesn't need MOVE to contiguous area
                ** Only start/end need to be contiguous, eliminates 1 MOVE
                */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);

                XrValue str_val = R(b);
                if (XR_IS_STRING(str_val)) {
                    XrString *str = XR_TO_STRING(str_val);
                    xr_Integer start = XR_TO_INT(R(c));
                    xr_Integer end = XR_TO_INT(R(c + 1));
                    XrString *result = xr_string_substring(isolate, str, start, end);
                    R(a) = result ? xr_string_value(result) : xr_null();
                    vmbreak;
                }
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "substring requires String type");
            }

            vmcase(OP_STR_REPEAT) {
                /* OP_STR_REPEAT: string repeat
                ** R[A] = R[B] * R[C]
                **
                ** B = string, C = repeat count (integer)
                ** Example: "-" * 50 generates 50 "-"
                */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);

                XrValue str_val = R(b);
                XrValue count_val = R(c);

                if (XR_IS_STRING(str_val) && XR_IS_INT(count_val)) {
                    XrString *str = XR_TO_STRING(str_val);
                    xr_Integer count = XR_TO_INT(count_val);
                    XrString *result = xr_string_repeat(isolate, str, count);
                    R(a) = result ? xr_string_value(result) : xr_null();
                    vmbreak;
                }
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "string repeat requires String * Int");
            }

            /* ========================================================
            ** Generic Index Operations
            ** ======================================================== */

            vmcase(OP_INDEX_GET) {
                // OP_INDEX_GET: generic index read
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue obj_val = R(b);
                XrValue key_val = R(c);

                // Fast path: struct inline fixed array ([N]T)
                if (XR_IS_ARRAY_REF(obj_val) && XR_IS_INT(key_val)) {
                    uint8_t etype = XR_ARRAY_REF_ELEM_TYPE(obj_val);
                    uint16_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
                    int idx = (int)XR_TO_INT(key_val);
                    if ((unsigned)idx < (unsigned)ecount) {
                        uint8_t *base_ptr = (uint8_t*)obj_val.ptr;
                        uint8_t es = xr_native_type_size(etype);
                        uint8_t *ep = base_ptr + idx * es;
                        switch (etype) {
                            case XR_NATIVE_I64:  R(a) = XR_FROM_INT(*(int64_t*)ep); break;
                            case XR_NATIVE_F64:  R(a) = XR_FROM_FLOAT(*(double*)ep); break;
                            case XR_NATIVE_BOOL: R(a) = *(uint8_t*)ep ? XR_TRUE_VAL : XR_FALSE_VAL; break;
                            case XR_NATIVE_I32:  R(a) = XR_FROM_INT((int64_t)*(int32_t*)ep); break;
                            case XR_NATIVE_U32:  R(a) = XR_FROM_INT((int64_t)*(uint32_t*)ep); break;
                            case XR_NATIVE_I16:  R(a) = XR_FROM_INT((int64_t)*(int16_t*)ep); break;
                            case XR_NATIVE_U16:  R(a) = XR_FROM_INT((int64_t)*(uint16_t*)ep); break;
                            case XR_NATIVE_I8:   R(a) = XR_FROM_INT((int64_t)*(int8_t*)ep); break;
                            case XR_NATIVE_U8:   R(a) = XR_FROM_INT((int64_t)*(uint8_t*)ep); break;
                            case XR_NATIVE_F32:  R(a) = XR_FROM_FLOAT((double)*(float*)ep); break;
                            default: R(a) = xr_null(); break;
                        }
                    } else {
                        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                            "fixed array index out of range: %d (length %u)", idx, (unsigned)ecount);
                    }
                    vmbreak;
                }
                // Fast path: String (Unicode character index)
                if (XR_IS_STRING(obj_val) && XR_IS_INT(key_val)) {
                    XrString *str = XR_TO_STRING(obj_val);
                    size_t idx = (size_t)XR_TO_INT(key_val);
                    XrString *ch = xr_string_char_at_unicode(isolate, str, idx);
                    R(a) = ch ? xr_string_value(ch) : xr_null();
                    vmbreak;
                }
                // Fast path: Array
                if (XR_IS_ARRAY(obj_val)) {
                    XrArray *arr = XR_TO_ARRAY(obj_val);
                    int idx = (int)XR_TO_INT(key_val);
                    if ((unsigned)idx < (unsigned)arr->length) {
                        R(a) = (arr->elem_type == XR_ELEM_ANY)
                            ? ((XrValue*)arr->data)[idx]
                            : xr_array_get_element(arr, idx);
                    } else {
                        R(a) = xr_null();
                    }
                    vmbreak;
                }
                // Fast path: Range (lazy element access)
                if (XR_IS_RANGE(obj_val) && XR_IS_INT(key_val)) {
                    XrRange *rng = xr_value_to_range(obj_val);
                    int64_t idx = XR_TO_INT(key_val);
                    int64_t len = xr_range_length(rng);
                    if (idx >= 0 && idx < len) {
                        R(a) = xr_int(rng->start + idx * rng->step);
                    } else {
                        R(a) = xr_null();
                    }
                    vmbreak;
                }
                // Fast path: Map
                if (XR_IS_MAP(obj_val)) {
                    XrMap *map = XR_TO_MAP(obj_val);
                    bool found;
                    R(a) = xr_map_get(map, key_val, &found);
                    if (!found) R(a) = xr_null();
                    vmbreak;
                }
                // Fast path: Json object (string keys only)
                if (xr_value_is_json(obj_val)) {
                    if (XR_IS_STRING(key_val)) {
                        XrJson *json = xr_value_to_json(obj_val);
                        XrString *key_str = XR_TO_STRING(key_val);
                        R(a) = xr_json_get_by_key(isolate, json, key_str->data);
                        vmbreak;
                    }
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "Json object only supports string keys");
                }
                // Operator overload
                if (xr_value_is_instance(obj_val)) {
                    XrInstance *_inst = xr_value_to_instance(obj_val);
                    XrClass *_cls = xr_instance_get_class(_inst);
                    if (XCLASS_HAS_OP(_cls, XR_OP_INDEX_FLAG)) {
                        XrMethod *_m = xr_class_lookup_method(_cls, SYMBOL_OP_INDEX);
                        if (_m && _m->type == XMETHOD_OPERATOR && _m->as.closure) {
                            XrClosure *_cl = _m->as.closure;
                            XrProto *_p = _cl->proto;
                            R(a + 1) = obj_val;
                            R(a + 2) = key_val;
                            savepc();
                            int _fi = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                            XrBcCallFrame *_nf = &VM_FRAMES[_fi];
                            _nf->closure = _cl;
                            _nf->pc = PROTO_CODE_BASE(_p);
                            _nf->base_offset = (int)((base + a + 1) - VM_STACK);
                            goto startfunc;
                        }
                    }
                }
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "only Array, Map, Json, String, Bytes, TypedArray support indexing");
            }

            vmcase(OP_INDEX_SET) {
                // OP_INDEX_SET: generic index write
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue obj_val = R(a);
                XrValue key_val = R(b);
                XrValue val = R(c);

                // Fast path: struct inline fixed array ([N]T)
                if (XR_IS_ARRAY_REF(obj_val) && XR_IS_INT(key_val)) {
                    uint8_t etype = XR_ARRAY_REF_ELEM_TYPE(obj_val);
                    uint16_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
                    int idx = (int)XR_TO_INT(key_val);
                    if ((unsigned)idx < (unsigned)ecount) {
                        uint8_t *base_ptr = (uint8_t*)obj_val.ptr;
                        uint8_t es = xr_native_type_size(etype);
                        uint8_t *ep = base_ptr + idx * es;
                        switch (etype) {
                            case XR_NATIVE_I64:  *(int64_t*)ep  = XR_TO_INT(val); break;
                            case XR_NATIVE_F64:  *(double*)ep   = XR_TO_FLOAT(val); break;
                            case XR_NATIVE_BOOL: *(uint8_t*)ep  = (uint8_t)val.i; break;
                            case XR_NATIVE_I32:  *(int32_t*)ep  = (int32_t)XR_TO_INT(val); break;
                            case XR_NATIVE_U32:  *(uint32_t*)ep = (uint32_t)XR_TO_INT(val); break;
                            case XR_NATIVE_I16:  *(int16_t*)ep  = (int16_t)XR_TO_INT(val); break;
                            case XR_NATIVE_U16:  *(uint16_t*)ep = (uint16_t)XR_TO_INT(val); break;
                            case XR_NATIVE_I8:   *(int8_t*)ep   = (int8_t)XR_TO_INT(val); break;
                            case XR_NATIVE_U8:   *(uint8_t*)ep  = (uint8_t)XR_TO_INT(val); break;
                            case XR_NATIVE_F32:  *(float*)ep    = (float)XR_TO_FLOAT(val); break;
                            default: break;
                        }
                    } else {
                        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                            "fixed array index out of range: %d (length %u)", idx, (unsigned)ecount);
                    }
                    vmbreak;
                }
                // Fast path: Array
                if (XR_IS_ARRAY(obj_val)) {
                    XrArray *arr = XR_TO_ARRAY(obj_val);
                    int idx = (int)XR_TO_INT(key_val);
                    if ((unsigned)idx < (unsigned)arr->length) {
                        if (arr->elem_type == XR_ELEM_ANY) {
                            ((XrValue*)arr->data)[idx] = val;
                            XR_ARRAY_MARK_GC_PTRS(arr, val);
                            VM_BARRIER_VAL(arr, val);
                        } else {
                            xr_array_set_element(arr, idx, val);
                        }
                    }
                    vmbreak;
                }
                // Fast path: Map
                if (XR_IS_MAP(obj_val)) {
                    XrMap *map = XR_TO_MAP(obj_val);
                    xr_map_set(map, key_val, val);
                    VM_BARRIER_BACK(map);
                    vmbreak;
                }
                // Fast path: Json object (string keys only)
                if (xr_value_is_json(obj_val)) {
                    if (XR_IS_STRING(key_val)) {
                        XrJson *json = xr_value_to_json(obj_val);
                        XrString *key_str = XR_TO_STRING(key_val);
                        xr_json_set_by_key(isolate, json, key_str->data, val);
                        VM_BARRIER_BACK(json);
                        vmbreak;
                    }
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "Json object only supports string keys");
                }
                // Operator overload
                if (xr_value_is_instance(obj_val)) {
                    XrInstance *_inst = xr_value_to_instance(obj_val);
                    XrClass *_cls = xr_instance_get_class(_inst);
                    if (XCLASS_HAS_OP(_cls, XR_OP_INDEX_SET_FLAG)) {
                        XrMethod *_m = xr_class_lookup_method(_cls, SYMBOL_OP_INDEX_SET);
                        if (_m && _m->type == XMETHOD_OPERATOR && _m->as.closure) {
                            XrClosure *_cl = _m->as.closure;
                            XrProto *_p = _cl->proto;
                            R(a + 2) = obj_val;
                            R(a + 3) = key_val;
                            R(a + 4) = val;
                            savepc();
                            int _fi = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                            XrBcCallFrame *_nf = &VM_FRAMES[_fi];
                            _nf->closure = _cl;
                            _nf->pc = PROTO_CODE_BASE(_p);
                            _nf->base_offset = (int)((base + a + 2) - VM_STACK);
                            goto startfunc;
                        }
                    }
                }
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "only Array, Map, Json, Bytes, TypedArray support index assignment");
            }

            vmcase(OP_SLICE) {
                /* OP_SLICE: slice operation
                ** R[A] = R[B][R[C]:R[C+1]]
                ** - R[B]: source object (Array/String/Bytes)
                ** - R[C]: start index (0 = from beginning)
                ** - R[C+1]: end index (-1 = to end)
                */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue source = R(b);
                int64_t start = XR_TO_INT(R(c));
                int64_t end = XR_TO_INT(R(c + 1));

                // Array slice: zero-copy, shared data
                if (XR_IS_ARRAY(source)) {
                    XrArray *arr = XR_TO_ARRAY(source);

                    // Use slice function
                    XrArray *slice = xr_array_slice(VM_CURRENT_CORO, arr, (int32_t)start, (int32_t)end);
                    R(a) = slice ? XR_FROM_PTR(slice) : xr_null();
                    vmbreak;
                }

                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "this type does not support slicing");
            }

            /* ========================================================
            ** Function Call Instructions
            ** ======================================================== */

            vmcase(OP_CALL) {
                /* Unified callable object handling
                ** Principle: no backward compatibility, use best design
                ** Strategy: distinguish two most common types for performance
                */
                TRACE_EXECUTION();

                /* GC safe point: function call boundary is ideal for GC.
                ** Stack is consistent, all locals are valid.
                ** Reductions check removed (Phase 4): only OP_JMP backward
                ** jumps check reductions. This reduces overhead from <3% to <1%.
                ** Preemption for pure call chains relies on sysmon + handoff. */
                VM_GC_SAFEPOINT();

                int a = GETARG_A(i);
                int nargs = GETARG_B(i);

                XrValue func_val = R(a);

                // Fast dispatch: skip slow paths for most common call types
                if (likely(XR_IS_FUNCTION(func_val))) goto op_call_closure;
                if (XR_IS_CFUNCTION(func_val)) goto op_call_cfunc;
                // Enum conversion: Status(200) -> Status.Success
                // Check if this is a call on enum type object
                if (XR_IS_PTR(func_val)) {
                    XrGCHeader *gc = (XrGCHeader*)XR_TO_PTR(func_val);

                    // Use dedicated enum type tag
                    if (XR_GC_GET_TYPE(gc) == XR_TENUM_TYPE && nargs == 1) {
                        // Enum conversion: Status(200)
                        XrValue value = R(a + 1); // First argument
                        XrEnumValue *result = xr_enum_from_value((XrEnumType*)gc, value);

                        if (result) {
                            R(a) = XR_FROM_PTR(result);
                            vmbreak; // Successfully converted as enum
                        } else {
                            // Conversion failed, return Null
                            R(a) = xr_null();
                            vmbreak;
                        }
                    }

                    // Class call: Array() or User() -> invoke constructor
                    if (XR_GC_GET_TYPE(gc) == XR_TCLASS) {
                        XrClass *klass = (XrClass*)gc;
                        XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
                        XrMethod *constructor = NULL;

                        // First look for "call" static method (native class)
                        int call_symbol = xr_symbol_lookup_in_table(sym_table, "call");
                        if (call_symbol >= 0) {
                            constructor = xr_class_lookup_method(klass, call_symbol);
                        }

                        if (constructor && constructor->type == XMETHOD_PRIMITIVE) {
                            // Native class constructor (GC safe zone)
                            XrCFunctionPtr func = constructor->as.primitive;
                            GC_SAFE_ENTER(isolate);
                            XrValue result = func(isolate, &R(a + 1), nargs);
                            GC_SAFE_LEAVE(isolate);
                            R(a) = result;
                            vmbreak;
                        }

                        // Look for constructor method (user defined class)
                        int ctor_symbol = xr_symbol_lookup_in_table(sym_table, XR_KEYWORD_CONSTRUCTOR);
                        if (ctor_symbol >= 0) {
                            constructor = xr_class_lookup_method(klass, ctor_symbol);
                        }

                        // Create instance (allocation based on storage mode context)
                        XrInstance *instance;
                        uint8_t storage_mode = isolate->current_storage_mode;
                        isolate->current_storage_mode = 0; // Reset context

                        if (storage_mode != 0 && isolate->sys_heap) {
                            // shared: allocate on system heap
                            size_t size = xr_instance_size(klass);
                            instance = (XrInstance*)xr_sysheap_alloc_shared(isolate->sys_heap, size, XR_TINSTANCE);
                            if (instance) {
                                xr_instance_init_inplace(instance, klass);
                                XR_GC_SET_STORAGE(&instance->gc, storage_mode);
                                if (storage_mode == XR_GC_STORAGE_SHARED) {
                                    xr_shared_set_refc(&instance->gc, 1);
                                }
                            }
                        } else {
                            // normal: allocate on coroutine heap
                            instance = xr_instance_new(isolate, klass);
                        }

                        if (!instance) {
                            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_CALL, "failed to create instance: '%s'", klass->name);
                        }
                        XrValue inst_val = XR_FROM_PTR(instance);

                        if (constructor && constructor->type == XMETHOD_CLOSURE) {
                            // User class constructor: create instance and call constructor
                            XrClosure *closure = constructor->as.closure;
                            XrProto *proto = closure->proto;

                            // Check stack space
                            if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
                            }

                            VM_STACK_CHECK(a + 1 + nargs + 1);

                            // Save current frame's pc
                            savepc();

                            /* Shift user args right by 1: R[a+1..a+nargs] -> R[a+2..a+nargs+1]
                             * This makes room for 'this' at R[a+1], matching standard call convention
                             * where base_offset = base+a+1, so return goes to R[a] */
                            for (int j = nargs; j > 0; j--) {
                                R(a + 1 + j) = R(a + j);
                            }
                            R(a + 1) = inst_val;

                            // Create new call frame (standard convention)
                            int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                            XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                            new_frame->closure = closure;
                            new_frame->pc = PROTO_CODE_BASE(proto);
                            new_frame->base_offset = (int)((base + a + 1) - VM_STACK);

                            // Jump to constructor
                            goto startfunc;
                        }

                        // No constructor, return instance directly
                        R(a) = inst_val;
                        vmbreak;
                    }
                }

                // Bound method call - must be before closure check!
                if (xr_value_is_bound_method(func_val)) {
                    XrBoundMethod *bm = xr_value_to_bound_method(func_val);
                    XrValue result = bm->handler(isolate, bm->receiver, &R(a + 1), nargs);
                    if (unlikely(XR_IS_NOTFOUND(result))) {
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "bound method call failed: method not found");
                    }
                    R(a) = result;
                    vmbreak;
                }

                // C function call
                op_call_cfunc:
                if (xr_value_is_cfunction(func_val)) {
                    XrCFunction *cfunc = xr_value_to_cfunction(func_val);

                    // Phase 3: Track current C function for sysmon auto-upgrade
                    {
                        XrWorker *_w = xr_current_worker();
                        if (_w && _w->m) _w->m->current_cfunc = cfunc;
                    }

                    /* Phase 3: SLOW C functions release P before execution.
                    ** This prevents long-running C code from blocking the
                    ** scheduler. Other coroutines continue on other M's. */
                    bool is_slow = (cfunc->cfunc_class == XR_CFUNC_SLOW);
                    if (is_slow) {
                        xr_worker_entersyscall();
                    }

                    if (cfunc->is_yieldable) {
                        // Yieldable C function (GC safe zone)
                        // Set to current frame before call
                        ci->u.c.result_slot = (int16_t)(GETARG_A(i));
                        ci->u.c.has_cfunc_result = false;

                        XrValue result;
                        GC_SAFE_ENTER(isolate);
                        XrCFuncResult status = cfunc->as.yieldable(isolate, &R(a + 1), nargs, &result);
                        GC_SAFE_LEAVE(isolate);

                        if (is_slow) {
                            xr_worker_exitsyscall();
                        }

                        switch (status) {
                            case XR_CFUNC_DONE:
                                R(a) = result;
                                vmbreak;

                            case XR_CFUNC_BLOCKED:
                                // Coroutine needs to block, save state and yield
                                savepc();
                                XR_DBG_CORO("VM BLOCKED: result_slot=%d, frame_idx=%d",
                                            (int)(GETARG_A(i)), VM_FRAME_COUNT - 1);
                                return XR_VM_BLOCKED;

                            case XR_CFUNC_YIELD:
                                // Active yield, continue next time
                                savepc();
                                return XR_VM_YIELD;

                            case XR_CFUNC_CALL_CLOSURE:
                                /* Closure frame pushed by xr_yield_call_closure,
                                 * execute it via normal VM path */
                                goto startfunc;

                            case XR_CFUNC_ERROR:
                                return XR_VM_RUNTIME_ERROR;
                        }
                    } else {
                        // Normal C function (GC safe zone)
                        GC_SAFE_ENTER(isolate);
                        XrValue result = cfunc->as.func(isolate, &R(a + 1), nargs);
                        GC_SAFE_LEAVE(isolate);

                        if (is_slow) {
                            xr_worker_exitsyscall();
                        }

                        R(a) = result;
                        vmbreak;
                    }
                }

                // Xray closure call (most common)
                op_call_closure:
                if (XR_IS_FUNCTION(func_val)) {
                    XrClosure *closure = xr_value_to_closure(func_val);
                    XrProto *proto = closure->proto;

                    // Argument count check
                    if (proto->is_vararg) {
                        // vararg function: check minimum required args
                        if (unlikely(nargs < proto->min_params)) {
                            VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "expected at least %d arguments, got %d",
                                             proto->min_params, nargs);
                        }

                        if (unlikely(VM_FRAME_COUNT >= XR_FRAMES_MAX)) {
                            VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW,
                                "stack overflow: recursion exceeds %d levels", XR_FRAMES_MAX);
                        }

                        // Stack boundary check: ensure new frame has enough stack space
                        VM_STACK_CHECK(a + 1 + proto->maxstacksize);

                        // Fill missing optional fixed params with null
                        for (int j = nargs; j < proto->numparams; j++) {
                            R(a + 1 + j) = xr_null();
                        }

                        // Collect extra arguments into rest array
                        int extra_args = nargs > proto->numparams ? nargs - proto->numparams : 0;
                        XrArray *rest_array = xr_array_new(VM_CURRENT_CORO);
                        if (extra_args > 0) {
                            XrValue *arg_base = &R(a + 1 + proto->numparams);
                            for (int j = 0; j < extra_args; j++) {
                                xr_array_push(rest_array, arg_base[j]);
                            }
                        }

                        // Rest parameter at numparams position (after all fixed params)
                        R(a + 1 + proto->numparams) = xr_value_from_array(rest_array);

                        savepc();

                        int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                        XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                        new_frame->closure = closure;
                        new_frame->pc = PROTO_CODE_BASE(proto);
                        new_frame->base_offset = (int)((base + a + 1) - VM_STACK);

                        goto startfunc;
                    } else if (likely(nargs >= proto->min_params && nargs <= proto->numparams)) {
                        // Non vararg: argument count in valid range (supports default params)

                        // Type feedback: collect argument types for profile-guided compilation
                        if (proto->type_feedback) {
                            XirTypeFeedback *fb = proto->type_feedback;
                            for (int fi = 0; fi < nargs && fi < XFB_MAX_PARAMS; fi++)
                                xfb_record_arg(fb, fi, R(a + 1 + fi));
                        } else if (atomic_load_explicit(&proto->call_count, memory_order_relaxed) >= XFB_ALLOC_THRESHOLD && !proto->param_types) {
                            proto->type_feedback = xfb_create();
                        }

#ifndef XRAY_HAS_JIT
                        /* AOT fast path: call pre-compiled native code directly.
                        ** Used by --native build mode: AOT thunks are registered
                        ** into proto->jit_entry before execution begins.
                        ** Calling convention: int64_t fn(intptr_t coro, int64_t *raw_args) */
                        if (proto->jit_entry) {
                            typedef int64_t (*AotThunkFn)(intptr_t, int64_t*);
                            XrCoroutine *_aot_coro = VM_CURRENT_CORO;
                            int64_t raw_args[16];  // 16 slots: tagged params use 2 each
                            int ai = 0;
                            for (int ri = 0; ri < nargs && ri < 8 && ai < 16; ri++) {
                                uint8_t gc = 0;
                                if (proto->param_types && ri < proto->param_types_count && proto->param_types[ri])
                                    gc = xr_type_to_slot_type(proto->param_types[ri]);
                                bool is_f = XR_SLOT_IS_FLOAT(gc);
                                bool is_tagged = (gc == XR_SLOT_PTR || gc == XR_SLOT_ANY);
                                if (is_f) {
                                    memcpy(&raw_args[ai], &R(a + 1 + ri).f, sizeof(double));
                                    ai++;
                                } else if (is_tagged) {
                                    // Pack full XrValue (16 bytes = 2 slots)
                                    memcpy(&raw_args[ai], &R(a + 1 + ri), sizeof(XrValue));
                                    ai += 2;
                                } else {
                                    raw_args[ai] = R(a + 1 + ri).i;
                                    ai++;
                                }
                            }
                            if (_aot_coro) _aot_coro->jit_ctx->call_closure = closure;
                            int64_t ret = ((AotThunkFn)proto->jit_entry)(
                                (intptr_t)_aot_coro, raw_args);
                            uint8_t rtype = proto->return_type_info
                                ? xr_type_to_slot_type(proto->return_type_info) : XR_SLOT_ANY;
                            if (XR_SLOT_IS_FLOAT(rtype)) {
                                memcpy(&R(a).f, &ret, sizeof(double));
                                R(a).tag = XR_TAG_F64;
                            } else if (rtype == XR_SLOT_BOOL) {
                                R(a).i = ret ? 1 : 0;
                                R(a).tag = XR_TAG_BOOL;
                            } else {
                                R(a).i = ret;
                                R(a).tag = XR_TAG_I64;
                            }
                            R(a).heap_type = 0;
                            vmbreak;
                        }
#endif

#ifdef XRAY_HAS_JIT
                        // Install pending background JIT compilation
                        if (!proto->jit_entry) {
                            void *pending = atomic_load_explicit(
                                &proto->jit_entry_pending, memory_order_acquire);
                            if (pending && (uintptr_t)pending > 1) {
                                xir_jit_install_bg_result(proto);
                                // Promote feedback to param_types so entry guards
                                // match the compiled code's type specialization.
                                if (proto->numparams > 0 && proto->type_feedback &&
                                    proto->type_feedback->stable) {
                                    if (!proto->param_types) {
                                        proto->param_types = (struct XrType **)xr_calloc(
                                            proto->numparams, sizeof(struct XrType *));
                                        if (proto->param_types)
                                            proto->param_types_count = proto->numparams;
                                    }
                                    if (proto->param_types) {
                                        for (int pi = 0; pi < proto->numparams && pi < 8; pi++) {
                                            if (pi < proto->param_types_count && !proto->param_types[pi] &&
                                                pi < XFB_MAX_PARAMS &&
                                                xfb_is_monomorphic(proto->type_feedback->arg_types[pi])) {
                                                uint8_t st = xfb_to_slot_type(proto->type_feedback->arg_types[pi]);
                                                proto->param_types[pi] = xr_slot_type_to_type(NULL, st);
                                            }
                                        }
                                    }
                                    if (!proto->return_type_info) {
                                        uint8_t fb_ret = xfb_to_slot_type(proto->type_feedback->return_type);
                                        if (fb_ret != XR_SLOT_ANY)
                                            proto->return_type_info = xr_slot_type_to_type(NULL, fb_ret);
                                    }
                                }
                            }
                        }
                        // JIT fast path: call compiled code directly
                        if (proto->jit_entry) {
                            XrValue jit_result;
                            XrCoroutine *_jit_coro = (XrCoroutine *)vm_ctx->current_coro;
                            _jit_coro->jit_ctx->call_proto = proto;
                            _jit_coro->jit_ctx->call_closure = closure;
                            _jit_coro->jit_ctx->call_base_offset = (int32_t)((base + a + 1) - VM_STACK);
                            int _jrc1 = xir_jit_call(proto->jit_entry, _jit_coro, &R(a + 1), nargs,
                                             proto->return_type_info, &jit_result);
                            if (_jrc1 == XIR_JIT_OK) {
                                R(a) = jit_result;
                                // Multi-return: fill R[a+1..] from jit_ctx->ret_vals[]
                                if (_jit_coro->jit_ctx->ret_count > 1)
                                    xir_jit_read_multi_ret(_jit_coro, &R(a), _jit_coro->jit_ctx->ret_count);
                                vmbreak;
                            }
                            if (_jrc1 == XIR_JIT_SUSPEND) {
                                if (proto->nosr > 0) proto->osr_pending = true;
                                savepc();
                                return XR_VM_BLOCKED;
                            }
                            // JIT exception: skip deopt recovery, let VM handle
                            if (!XR_IS_NULL(VM_EXCEPTION)) {
                                if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                                goto startfunc;
                            }
                            /* Deopt: try mid-function recovery first.
                             * Restores VM slots from JIT register state and resumes
                             * the interpreter at the deopt bytecode PC. */
                            proto->deopt_count++;
                            if (proto->deopt_count <= 3) {
                                XrCoroutine *_dc = (XrCoroutine *)vm_ctx->current_coro;
                                int32_t recover_pc = xir_jit_deopt_recover(
                                    _dc, &R(a + 1), proto->maxstacksize);
                                if (recover_pc >= 0) {
                                    if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                                        VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW,
                                            "stack overflow: recursion exceeds %d levels", XR_FRAMES_MAX);
                                    }
                                    VM_STACK_CHECK(a + 1 + proto->maxstacksize);
                                    savepc();
                                    int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                                    XrBcCallFrame *_nf = &VM_FRAMES[_fidx];
                                    _nf->closure = closure;
                                    _nf->pc = PROTO_CODE_BASE(proto) + recover_pc;
                                    _nf->base_offset = (int)((base + a + 1) - VM_STACK);
                                    if (proto->nosr > 0) proto->osr_pending = true;
                                    goto startfunc;
                                }
                            }
                            // Full deopt: invalidate JIT entry after threshold
                            if (proto->deopt_count > 3) {
                                proto->jit_entry = NULL;
                            }
                        }

                        // Hot function detection: try JIT compilation
                        if (isolate->vm.jit && !proto->jit_entry &&
                            atomic_fetch_add_explicit(&proto->call_count, 1, memory_order_relaxed) + 1 == (uint32_t)isolate->vm.jit_threshold) {
                            xir_jit_try_compile(isolate->vm.jit, proto);
                            if (proto->jit_entry) {
                                XrValue jit_result;
                                XrCoroutine *_jit_coro = (XrCoroutine *)vm_ctx->current_coro;
                                _jit_coro->jit_ctx->call_proto = proto;
                                _jit_coro->jit_ctx->call_closure = closure;
                                _jit_coro->jit_ctx->call_base_offset = (int32_t)((base + a + 1) - VM_STACK);
                                int _jrc2 = xir_jit_call(proto->jit_entry, _jit_coro, &R(a + 1), nargs,
                                                 proto->return_type_info, &jit_result);
                                if (_jrc2 == XIR_JIT_OK) {
                                    R(a) = jit_result;
                                    if (_jit_coro->jit_ctx->ret_count > 1)
                                        xir_jit_read_multi_ret(_jit_coro, &R(a), _jit_coro->jit_ctx->ret_count);
                                    vmbreak;
                                }
                                if (_jrc2 == XIR_JIT_SUSPEND) {
                                    if (proto->nosr > 0) proto->osr_pending = true;
                                    savepc();
                                    return XR_VM_BLOCKED;
                                }
                                // JIT exception: skip deopt recovery, let VM handle
                                if (!XR_IS_NULL(VM_EXCEPTION)) {
                                    if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                                    goto startfunc;
                                }
                                // Deopt: try mid-function recovery
                                proto->deopt_count++;
                                if (proto->deopt_count <= 3) {
                                    XrCoroutine *_dc2 = (XrCoroutine *)vm_ctx->current_coro;
                                    int32_t rpc2 = xir_jit_deopt_recover(
                                        _dc2, &R(a + 1), proto->maxstacksize);
                                    if (rpc2 >= 0) {
                                        if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                                            VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW,
                                                "stack overflow: recursion exceeds %d levels", XR_FRAMES_MAX);
                                        }
                                        VM_STACK_CHECK(a + 1 + proto->maxstacksize);
                                        savepc();
                                        int _fi2 = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                                        XrBcCallFrame *_nf2 = &VM_FRAMES[_fi2];
                                        _nf2->closure = closure;
                                        _nf2->pc = PROTO_CODE_BASE(proto) + rpc2;
                                        _nf2->base_offset = (int)((base + a + 1) - VM_STACK);
                                        if (proto->nosr > 0) proto->osr_pending = true;
                                        goto startfunc;
                                    }
                                }
                            }
                        }
#endif

                        if (unlikely(VM_FRAME_COUNT >= XR_FRAMES_MAX)) {
                            VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW,
                                "stack overflow: recursion exceeds %d levels", XR_FRAMES_MAX);
                        }

                        // Stack boundary check: ensure new frame has enough stack space
                        VM_STACK_CHECK(a + 1 + proto->maxstacksize);

                        /* Fill missing optional arguments with null.
                        ** NORMAL functions: nargs == numparams always (checked above),
                        ** skip the loop entirely via entry_type fast path. */
                        if (proto->entry_type != XR_ENTRY_NORMAL) {
                            for (int j = nargs; j < proto->numparams; j++) {
                                R(a + 1 + j) = xr_null();
                            }
                        }

                        // Key: save current frame's pc for return continuation
                        savepc();

                        // Create new call frame (zero overhead)
                        int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                        XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                        new_frame->closure = closure;
                        new_frame->pc = PROTO_CODE_BASE(proto);
                        new_frame->base_offset = (int)((base + a + 1) - VM_STACK);

                        // Jump directly to new function
                        goto startfunc;
                    } else {
                        if (proto->min_params == proto->numparams) {
                            VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "expected %d arguments, got %d",
                                             proto->numparams, nargs);
                        } else {
                            VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "expected %d-%d arguments, got %d",
                                             proto->min_params, proto->numparams, nargs);
                        }
                    }
                }

                // Error: non-callable value (catchable by try/catch)
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_CALL, "attempt to call a non-function value");
            }

            vmcase(OP_CALL_KEEP) {
                /* OP_CALL_KEEP A B C: call R[A] with B args, result to R[C], R[A] preserved
                ** Used by higher-order function inline (map/filter/reduce/forEach)
                ** to avoid callback register being overwritten by return value
                */
                int a = GETARG_A(i);
                int nargs = GETARG_B(i);
                int result_reg = GETARG_C(i);
                XrValue func_val = R(a);

                if (unlikely(!XR_IS_FUNCTION(func_val))) {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_CALL, "attempt to call a non-function value");
                }

                XrClosure *closure = xr_value_to_closure(func_val);
                XrProto *proto = closure->proto;

                // Argument count: silently truncate extra args
                int effective_nargs = nargs;
                if (!proto->is_vararg && nargs > proto->numparams) {
                    effective_nargs = proto->numparams;
                }

                if (unlikely(effective_nargs < proto->min_params)) {
                    VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "expected %d arguments, got %d",
                                     proto->min_params, effective_nargs);
                }

                if (unlikely(VM_FRAME_COUNT >= XR_FRAMES_MAX)) {
                    VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW,
                        "stack overflow: recursion exceeds %d levels", XR_FRAMES_MAX);
                }

                VM_STACK_CHECK(a + 1 + proto->maxstacksize);

                // Fill missing optional arguments with null
                for (int j = effective_nargs; j < proto->numparams; j++) {
                    R(a + 1 + j) = xr_null();
                }

                savepc();

                int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                new_frame->closure = closure;
                new_frame->pc = PROTO_CODE_BASE(proto);
                new_frame->base_offset = (int)((base + a + 1) - VM_STACK);
                new_frame->result_offset = (int)((base + result_reg) - VM_STACK);
                new_frame->call_status = XR_CALL_KEEP_FUNC;

                goto startfunc;
            }

            vmcase(OP_CALL_STATIC) {
                /* OP_CALL_STATIC A B: call R[A] with B args, result to R[A]
                ** Emitted when codegen knows R[A] is a plain closure (no class call,
                ** no C function, no enum). Falls through to OP_CALL which handles
                ** the fast-path XR_IS_FUNCTION branch first.
                */
                goto L_OP_CALL;
            }

            vmcase(OP_LOOP_BACK) {
                /* Tail recursion → loop: single instruction replaces
                 * CLOSE + N×MOVE + backward JMP.
                 * A=func_reg, B=nargs, C=skip (0=regular/static, 1=instance method).
                 * R[skip..skip+B-1] = R[A+1..A+B]; PC = function entry.
                 * When C=1, R[0]=this is preserved.
                 */
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int nargs = GETARG_B(i);
                int skip = GETARG_C(i);

                // Copy args to param positions (skip=1 preserves R[0]=this)
                if (nargs > 0) {
                    memmove(base + skip, &R(a + 1), sizeof(XrValue) * nargs);
                }

                // Reset PC to function entry
                pc = PROTO_CODE_BASE(frame->closure->proto);

                // GC safe point + reduction check (same as backward JMP)
                if (vm_ctx && vm_ctx->current_coro) {
                    XrCoroutine *coro = (XrCoroutine *)vm_ctx->current_coro;
                    VM_GC_SAFEPOINT();
                    if (--coro->reductions <= 0) {
                        if (xr_coro_flags_has(coro, XR_CORO_FLG_CANCEL_REQUESTED)) {
                            return XR_VM_CANCELLED;
                        }
                        coro->reductions = XR_CORO_REDUCTIONS;
                        frame->pc = pc;
                        return XR_VM_YIELD;
                    }
                }

                vmbreak;
            }

            vmcase(OP_CALLSELF) {
                /* Optimization: recursive self-call without GETGLOBAL
                 * Supports both normal recursive calls and tail recursive calls
                 */
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int nargs = GETARG_B(i);
                int nresults = GETARG_C(i); // Check return count, 0=tail call

                // Called function is current function
                XrClosure *closure = frame->closure;

                // Argument count check
                if (unlikely(nargs != closure->proto->numparams)) {
                    VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "expected %d arguments, got %d",
                                     closure->proto->numparams, nargs);
                }

#ifdef XRAY_HAS_JIT
                {
                    XrProto *proto = closure->proto;
                    // JIT fast path: call compiled code directly
                    if (proto->jit_entry) {
                        XrValue jit_result;
                        XrCoroutine *_jit_coro = (XrCoroutine *)vm_ctx->current_coro;
                        _jit_coro->jit_ctx->call_proto = proto;
                        _jit_coro->jit_ctx->call_closure = closure;
                        _jit_coro->jit_ctx->call_base_offset = (int32_t)((base + a + 1) - VM_STACK);
                        int _jrc3 = xir_jit_call(proto->jit_entry, _jit_coro, &R(a + 1), nargs,
                                         proto->return_type_info, &jit_result);
                        if (_jrc3 == XIR_JIT_OK) {
                            R(a) = jit_result;
                            if (_jit_coro->jit_ctx->ret_count > 1)
                                xir_jit_read_multi_ret(_jit_coro, &R(a), _jit_coro->jit_ctx->ret_count);
                            vmbreak;
                        }
                        if (_jrc3 == XIR_JIT_SUSPEND) {
                            if (proto->nosr > 0) proto->osr_pending = true;
                            savepc();
                            return XR_VM_BLOCKED;
                        }
                        // JIT exception: skip deopt recovery, let VM handle
                        if (!XR_IS_NULL(VM_EXCEPTION)) {
                            if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                            goto startfunc;
                        }
                        // Deopt: try mid-function recovery
                        proto->deopt_count++;
                        if (proto->deopt_count <= 3) {
                            XrCoroutine *_dc = (XrCoroutine *)vm_ctx->current_coro;
                            int32_t recover_pc = xir_jit_deopt_recover(
                                _dc, &R(a + 1), proto->maxstacksize);
                            if (recover_pc >= 0) {
                                if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                                    VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW,
                                        "stack overflow: recursion exceeds %d levels", XR_FRAMES_MAX);
                                }
                                VM_STACK_CHECK(a + 1 + proto->maxstacksize);
                                savepc();
                                int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                                XrBcCallFrame *_nf = &VM_FRAMES[_fidx];
                                _nf->closure = closure;
                                _nf->pc = PROTO_CODE_BASE(proto) + recover_pc;
                                _nf->base_offset = (int)((base + a + 1) - VM_STACK);
                                if (proto->nosr > 0) proto->osr_pending = true;
                                goto startfunc;
                            }
                        }
                        if (proto->deopt_count > 3) {
                            proto->jit_entry = NULL;
                        }
                    }

                    // Hot function detection: try JIT compilation
                    if (isolate->vm.jit && !proto->jit_entry &&
                        atomic_fetch_add_explicit(&proto->call_count, 1, memory_order_relaxed) + 1 == (uint32_t)isolate->vm.jit_threshold) {
                        xir_jit_try_compile(isolate->vm.jit, proto);
                        if (proto->jit_entry) {
                            XrValue jit_result;
                            XrCoroutine *_jit_coro = (XrCoroutine *)vm_ctx->current_coro;
                            _jit_coro->jit_ctx->call_proto = proto;
                            _jit_coro->jit_ctx->call_closure = closure;
                            _jit_coro->jit_ctx->call_base_offset = (int32_t)((base + a + 1) - VM_STACK);
                            int _jrc4 = xir_jit_call(proto->jit_entry, _jit_coro, &R(a + 1), nargs,
                                             proto->return_type_info, &jit_result);
                            if (_jrc4 == XIR_JIT_OK) {
                                R(a) = jit_result;
                                if (_jit_coro->jit_ctx->ret_count > 1)
                                    xir_jit_read_multi_ret(_jit_coro, &R(a), _jit_coro->jit_ctx->ret_count);
                                vmbreak;
                            }
                            if (_jrc4 == XIR_JIT_SUSPEND) {
                                if (proto->nosr > 0) proto->osr_pending = true;
                                savepc();
                                return XR_VM_BLOCKED;
                            }
                        }
                    }
                }
#endif // Check if tail call (C = 0)
                if (nresults == 0) {
                    // Recursive tail call: reuse current stack frame, no depth increase

                    // Move arguments to base position (overwrite current args)
                    if (nargs > 0) {
                        memmove(base, &R(a + 1), sizeof(XrValue) * nargs);
                    }

                    // Reset PC to function start
                    ci->pc = PROTO_CODE_BASE(closure->proto);

                    // Jump to function start
                    goto startfunc;
                } else {
                    // Recursive normal call: create new call frame
                    if (unlikely(VM_FRAME_COUNT >= XR_FRAMES_MAX)) {
                        VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
                    }

                    // Stack boundary check: ensure new frame has enough stack space
                    VM_STACK_CHECK(a + 1 + closure->proto->maxstacksize);

                    // Save current frame's pc
                    savepc();

                    // Create new call frame
                    int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                    XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                    new_frame->closure = closure; // Use same closure
                    new_frame->pc = PROTO_CODE_BASE(closure->proto);
                    new_frame->base_offset = (int)((base + a + 1) - VM_STACK); // Args from R[a+1]

                    // Jump directly to startfunc
                    goto startfunc;
                }
            }

            vmcase(OP_TAILCALL) {
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int nargs = GETARG_B(i);

                /* Tail call optimization
                ** Key: reuse current stack frame, no call depth increase
                ** This allows infinite tail recursion without stack overflow
                */

                // Get the called function
                XrValue func_val = R(a);

                // Type check (class calls have tail call disabled at compile time)
                if (!XR_IS_FUNCTION(func_val)) {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_CALL, "attempt to call a non-function value");
                }

                XrClosure *new_closure = xr_value_to_closure(func_val);

                // Argument count check
                if (unlikely(nargs != new_closure->proto->numparams)) {
                    VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "expected %d arguments, got %d",
                                     new_closure->proto->numparams, nargs);
                }

                // Check stack space (multi-core mode uses worker private stack)
                XrValue *stack_base_check = VM_STACK;
                (void)stack_base_check;
                size_t current_stack_offset = frame->base_offset;
                size_t required_stack_size = current_stack_offset + new_closure->proto->maxstacksize;
                size_t stack_max = vm_ctx->stack_capacity;

                if (required_stack_size > stack_max) {
                    VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow: need %zu, max %zu",
                                       required_stack_size, stack_max);
                }

                /* Step 1: move arguments to base position
                ** Use memmove to handle potentially overlapping memory
                */
                if (nargs > 0) {
                    memmove(base, &R(a + 1), sizeof(XrValue) * nargs);
                }

                /* Step 3: update ci's closure and PC
                ** Note: don't change frame_count, reuse current stack frame!
                */
                ci->closure = new_closure;
                ci->pc = PROTO_CODE_BASE(new_closure->proto);

                /* Step 4: update stack top pointer
                ** Should not accumulate! Keep at base + maxstacksize
                */
                VM_SET_STACK_TOP(base + new_closure->proto->maxstacksize);

                /* Step 5: jump directly to startfunc
                ** No call depth increase, true tail call optimization
                */
                goto startfunc;
            }

            vmcase(OP_RETURN) {
                // OP_RETURN: function return (multi-value support)

                /* GC safe point: function exit is ideal for GC
                ** Stack frame is about to be popped, good time to collect */
                VM_GC_SAFEPOINT();

                // Clean up exception handlers belonging to current frame
                while (VM_HANDLER_COUNT > 0 &&
                       VM_HANDLERS[VM_HANDLER_COUNT - 1].frame_count >= VM_FRAME_COUNT) {
                    VM_DEC_HANDLER_COUNT;
                }

                int a = GETARG_A(i);
                int nret = GETARG_B(i); // Return value count

                // Save return count for caller
                vm_ctx->last_nret = nret;

            return_with_defer: ; // Label for RETURN0/RETURN1 fallback when defer exists
                /* Re-read a and nret: OP_RETURN0/OP_RETURN1 goto here
                 * skipping the local variable initialization above */
                a = GETARG_A(i);
                nret = vm_ctx->last_nret;
                // Get first return value (for defer/toString compatibility)
                XrValue ret_result = (nret > 0) ? R(a) : xr_null();

                /* Execute current frame's defer (LIFO order)
                 * Only execute defer belonging to current frame
                 *
                 * defer stack format: [closure, arg_count, arg1, arg2, ...]
                 * Read from back: pop args, then arg count, then closure
                 */
                if (isolate->vm.defer_count > 0 && isolate->vm.defer_frame_marks) {
                    // Get current frame's defer start position
                    int frame_defer_start = isolate->vm.defer_frame_marks[VM_FRAME_COUNT - 1];

                    // Only execute defer registered by current frame
                    if (isolate->vm.defer_count > frame_defer_start) {
                        // Save current frame state
                        ci->pc = pc;

                        // Execute current frame's defer from stack top (LIFO)
                        while (isolate->vm.defer_count > frame_defer_start) {
                            // Temporary array for args (bounded)
                            XrValue defer_args[XR_DEFER_ARGS_MAX];

                            // Simplified implementation: iterate all entries and execute
                            int pos = frame_defer_start;
                            int entries[XR_DEFER_ENTRIES_MAX]; // Start position of each defer entry
                            int entry_count = 0;
                            int end = isolate->vm.defer_count;

                            // Collect all defer entry positions (with bounds check)
                            while (pos < end && entry_count < XR_DEFER_ENTRIES_MAX) {
                                entries[entry_count++] = pos;
                                // Skip: closure + arg count + args
                                int nargs = (int)XR_TO_INT(isolate->vm.defer_stack[pos + 1]);
                                pos += 2 + nargs;
                            }

                            // Error if defer entries exceed limit
                            if (pos < end) {
                                VM_RUNTIME_ERROR(XR_ERR_OVERFLOW,
                                    "defer: too many entries (%d), max=%d",
                                    entry_count + (end - pos), XR_DEFER_ENTRIES_MAX);
                            }

                            // LIFO execution: from back to front
                            for (int e = entry_count - 1; e >= 0; e--) {
                                int start = entries[e];
                                XrValue closure_val = isolate->vm.defer_stack[start];
                                int nargs = (int)XR_TO_INT(isolate->vm.defer_stack[start + 1]);

                                // Error if defer args exceed limit
                                if (nargs > XR_DEFER_ARGS_MAX) {
                                    VM_RUNTIME_ERROR(XR_ERR_OVERFLOW,
                                        "defer: too many arguments (%d), max=%d",
                                        nargs, XR_DEFER_ARGS_MAX);
                                }

                                // Collect args
                                for (int j = 0; j < nargs; j++) {
                                    defer_args[j] = isolate->vm.defer_stack[start + 2 + j];
                                }

                                // Execute
                                if (xr_value_is_closure(closure_val)) {
                                    struct XrClosure *closure = xr_value_to_closure(closure_val);
                                    xr_vm_call_closure(isolate, closure, defer_args, nargs);
                                }
                            }

                            // Clear current frame's defer
                            isolate->vm.defer_count = frame_defer_start;
                            break;
                        }
                    }
                }

                // Save toString print flags (before popping frame)
                uint8_t tostring_flags = ci->flags;
                ci->flags = 0; // Clear flags

                /* Calculate return position and write multiple return values
                ** Key: when base_offset is 0, base_offset - 1 would overflow!
                ** This is top-level frame (main/module), no need to write return value
                */
                XrValue *return_slot = NULL;
                if (ci->base_offset > 0) {
                    if (unlikely(ci->call_status & XR_CALL_KEEP_FUNC)) {
                        return_slot = VM_STACK + ci->result_offset;
                    } else {
                        return_slot = VM_STACK + ci->base_offset - 1;
                    }
                    for (int j = 0; j < nret; j++) {
                        return_slot[j] = R(a + j);
                    }
                    // Write null when no return value
                    if (nret == 0) {
                        *return_slot = xr_null();
                    }
                }

                // Pop call frame
                VM_DEC_FRAME_COUNT;

                // Constructor call stack management
                if (isolate->vm.ctor_call_depth > 0) {
                    int expected_frame_count = isolate->vm.ctor_call_stack[isolate->vm.ctor_call_depth - 1].frame_count;
                    if (VM_FRAME_COUNT == expected_frame_count) {
                        isolate->vm.ctor_call_depth--;
                    }
                }

                // Handle toString print: if toString call returned, print result
                if (tostring_flags) {
                    XrString *ts = xr_value_to_string(isolate, ret_result);
                    printf("%s", ts->data);
                    if (tostring_flags & 0x02) printf("\n");
                }

                if (VM_MODULE_BASE >= 0 &&
                    VM_FRAME_COUNT == VM_MODULE_BASE) {
                    // Module execution complete, return to caller
                    return XR_VM_OK;
                }

                // Restore caller frame
                ci = &VM_FRAMES[VM_FRAME_COUNT - 1];

                // Closure called via xr_yield_call_closure returned
                if (unlikely(ci->call_status & XR_CALL_CLOSURE_PENDING)) {
                    XrCoroutine *_pcoro = (XrCoroutine *)vm_ctx->current_coro;
                    _pcoro->pending_closure_result = return_slot ? *return_slot : xr_null();
                    goto handle_closure_pending;
                }

                if (!ci->closure || !ci->closure->proto) {
                    // Defensive check: closure invalid, return directly
                    return XR_VM_OK;
                }
                VM_SET_STACK_TOP(VM_STACK + ci->base_offset + ci->closure->proto->maxstacksize);

                // Handle operator overload conditional jump
                if (return_slot && ci->u.l.pending_operator_check) {
                    bool op_result = XR_TO_BOOL(*return_slot);
                    if (op_result != ci->u.l.operator_check_k) {
                        ci->pc++;
                    }
                    ci->u.l.pending_operator_check = false;
                }

                goto startfunc;
            }

            vmcase(OP_RETURN0) {
                /* OP_RETURN0: fast return with no values
                ** Optimized path: skip defer/upvalue checks when not needed
                */

                // Clean up exception handlers belonging to current frame
                while (VM_HANDLER_COUNT > 0 &&
                       VM_HANDLERS[VM_HANDLER_COUNT - 1].frame_count >= VM_FRAME_COUNT) {
                    VM_DEC_HANDLER_COUNT;
                }

                // Check if we have defer to execute
                if (isolate->vm.defer_count > 0 && isolate->vm.defer_frame_marks) {
                    int frame_defer_start = isolate->vm.defer_frame_marks[VM_FRAME_COUNT - 1];
                    if (isolate->vm.defer_count > frame_defer_start) {
                        // Has defer, fall back to full RETURN
                        vm_ctx->last_nret = 0;
                        goto return_with_defer;
                    }
                }

                // Fast path: no defer
                vm_ctx->last_nret = 0;

                // Write null to return slot
                if (ci->base_offset > 0) {
                    if (unlikely(ci->call_status & XR_CALL_KEEP_FUNC)) {
                        VM_STACK[ci->result_offset] = xr_null();
                    } else {
                        VM_STACK[ci->base_offset - 1] = xr_null();
                    }
                }

                // Pop call frame
                VM_DEC_FRAME_COUNT;

                // Constructor call stack management
                if (isolate->vm.ctor_call_depth > 0) {
                    int expected = isolate->vm.ctor_call_stack[isolate->vm.ctor_call_depth - 1].frame_count;
                    if (VM_FRAME_COUNT == expected) {
                        isolate->vm.ctor_call_depth--;
                    }
                }

                // Check module boundary
                if (VM_MODULE_BASE >= 0 && VM_FRAME_COUNT == VM_MODULE_BASE) {
                    return XR_VM_OK;
                }

                // Restore caller frame
                ci = &VM_FRAMES[VM_FRAME_COUNT - 1];

                // Closure called via xr_yield_call_closure returned
                if (unlikely(ci->call_status & XR_CALL_CLOSURE_PENDING)) {
                    XrCoroutine *_pcoro = (XrCoroutine *)vm_ctx->current_coro;
                    _pcoro->pending_closure_result = xr_null();
                    goto handle_closure_pending;
                }

                if (!ci->closure || !ci->closure->proto) {
                    return XR_VM_OK;
                }
                VM_SET_STACK_TOP(VM_STACK + ci->base_offset + ci->closure->proto->maxstacksize);

                goto startfunc;
            }

            vmcase(OP_RETURN1) {
                /* OP_RETURN1: fast return with single value
                ** Optimized path for the most common case
                */
                int a = GETARG_A(i);
                XrValue ret_val = R(a);

                // Clean up exception handlers belonging to current frame
                while (VM_HANDLER_COUNT > 0 &&
                       VM_HANDLERS[VM_HANDLER_COUNT - 1].frame_count >= VM_FRAME_COUNT) {
                    VM_DEC_HANDLER_COUNT;
                }

                // Type feedback: record return value type
                if (ci->closure && ci->closure->proto && ci->closure->proto->type_feedback) {
                    xfb_record_return(ci->closure->proto->type_feedback, ret_val);
                }

                // Check if we have defer to execute
                if (isolate->vm.defer_count > 0 && isolate->vm.defer_frame_marks) {
                    int frame_defer_start = isolate->vm.defer_frame_marks[VM_FRAME_COUNT - 1];
                    if (isolate->vm.defer_count > frame_defer_start) {
                        // Has defer, fall back to full RETURN
                        vm_ctx->last_nret = 1;
                        goto return_with_defer;
                    }
                }

                // Fast path: no defer
                vm_ctx->last_nret = 1;

                // Save toString print flags
                uint8_t tostring_flags = ci->flags;
                ci->flags = 0;

                // Write return value
                XrValue *return_slot = NULL;
                if (ci->base_offset > 0) {
                    if (unlikely(ci->call_status & XR_CALL_KEEP_FUNC)) {
                        return_slot = &VM_STACK[ci->result_offset];
                    } else {
                        return_slot = &VM_STACK[ci->base_offset - 1];
                    }
                    *return_slot = ret_val;
                }

                /* Rescue struct_ref pointing to callee's struct_area:
                 * append to struct_ret_arena so it survives frame reuse */
                if (return_slot && XR_IS_STRUCT_REF(ret_val)) {
                    int sa_idx = VM_FRAME_COUNT - 1;
                    if (vm_ctx->struct_areas && sa_idx < vm_ctx->struct_areas_cap) {
                        uint8_t *sa = vm_ctx->struct_areas[sa_idx];
                        uint8_t *sptr = (uint8_t*)ret_val.ptr;
                        uint16_t sa_cap = vm_ctx->struct_area_caps[sa_idx];
                        if (sa && sptr >= sa && sptr < sa + sa_cap) {
                            XrClass *rcls = *(XrClass**)sptr;
                            if (rcls && rcls->struct_layout) {
                                uint32_t total = 8 + rcls->struct_layout->total_size;
                                // Align to 16 bytes
                                total = (total + 15) & ~15u;
                                uint32_t need = vm_ctx->struct_ret_arena_used + total;
                                if (need > vm_ctx->struct_ret_arena_cap) {
                                    uint32_t new_cap = vm_ctx->struct_ret_arena_cap;
                                    if (new_cap < 512) new_cap = 512;
                                    while (new_cap < need) new_cap *= 2;
                                    vm_ctx->struct_ret_arena = (uint8_t*)xr_realloc(
                                        vm_ctx->struct_ret_arena, new_cap);
                                    vm_ctx->struct_ret_arena_cap = new_cap;
                                }
                                uint8_t *dst = vm_ctx->struct_ret_arena + vm_ctx->struct_ret_arena_used;
                                memcpy(dst, sptr, 8 + rcls->struct_layout->total_size);
                                vm_ctx->struct_ret_arena_used = need;
                                return_slot->ptr = dst;
                            }
                        }
                    }
                }

                // Pop call frame
                VM_DEC_FRAME_COUNT;

                // Constructor call stack management
                if (isolate->vm.ctor_call_depth > 0) {
                    int expected = isolate->vm.ctor_call_stack[isolate->vm.ctor_call_depth - 1].frame_count;
                    if (VM_FRAME_COUNT == expected) {
                        isolate->vm.ctor_call_depth--;
                    }
                }

                // Handle toString print
                if (tostring_flags) {
                    XrString *ts = xr_value_to_string(isolate, ret_val);
                    printf("%s", ts->data);
                    if (tostring_flags & 0x02) printf("\n");
                }

                // Check module boundary
                if (VM_MODULE_BASE >= 0 && VM_FRAME_COUNT == VM_MODULE_BASE) {
                    return XR_VM_OK;
                }

                // Restore caller frame
                ci = &VM_FRAMES[VM_FRAME_COUNT - 1];

                // Closure called via xr_yield_call_closure returned
                if (unlikely(ci->call_status & XR_CALL_CLOSURE_PENDING)) {
                    XrCoroutine *_pcoro = (XrCoroutine *)vm_ctx->current_coro;
                    _pcoro->pending_closure_result = ret_val;
                    goto handle_closure_pending;
                }

                if (!ci->closure || !ci->closure->proto) {
                    return XR_VM_OK;
                }
                VM_SET_STACK_TOP(VM_STACK + ci->base_offset + ci->closure->proto->maxstacksize);

                // Handle operator overload conditional jump
                if (return_slot && ci->u.l.pending_operator_check) {
                    bool op_result = XR_TO_BOOL(*return_slot);
                    if (op_result != ci->u.l.operator_check_k) {
                        ci->pc++;
                    }
                    ci->u.l.pending_operator_check = false;
                }

                goto startfunc;
            }

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

                // Create field IC table on demand
                if (frame->closure->proto->ic_fields == NULL) {
                    int cache_count = PROTO_CODE_COUNT(frame->closure->proto);
                    frame->closure->proto->ic_fields = xr_ic_field_table_new(cache_count);
                    for (int ic_i = 0; ic_i < cache_count; ic_i++) {
                        xr_ic_field_table_alloc(frame->closure->proto->ic_fields);
                    }
                }

                size_t cache_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
                XR_VM_IC_ASSERT_INDEX(cache_index, frame->closure->proto);
                XrICField *cache = xr_ic_field_table_get(frame->closure->proto->ic_fields, (int)cache_index);
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

                /* Reductions check removed (Phase 4): only OP_JMP checks.
                ** GC safepoint handled at startfunc label. */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int nargs = GETARG_C(i);

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
                    XrMap *map = XR_TO_MAP(receiver);
                    R(a) = map_method_call_by_symbol(isolate, map, method_symbol, &R(a + 2), nargs);
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
                    XrJson *json = xr_value_to_json(receiver);
                    R(a) = json_method_call_by_symbol(isolate, json, method_symbol, &R(a + 2), nargs);
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
                    XrString *str = xr_value_to_string(isolate, receiver);
                    R(a) = string_method_call_by_symbol(isolate, str, method_symbol, &R(a + 2), nargs);
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
                    XrArray *array = XR_TO_ARRAY(receiver);
                    R(a) = array_method_call_by_symbol(isolate, array, method_symbol, &R(a + 2), nargs);
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
                    XrSet *set = XR_TO_SET(receiver);
                    R(a) = set_method_call_by_symbol(isolate, set, method_symbol, &R(a + 2), nargs);
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
                    R(a) = int_method_call_by_symbol(isolate, XR_TO_INT(receiver), method_symbol, &R(a + 2), nargs);
                    if (unlikely(XR_IS_NOTFOUND(R(a)))) {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "int has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === Float builtin methods === */
                invoke_float:
                if (XR_IS_FLOAT(receiver)) {
                    R(a) = float_method_call_by_symbol(isolate, XR_TO_FLOAT(receiver), method_symbol, &R(a + 2), nargs);
                    if (unlikely(XR_IS_NOTFOUND(R(a)))) {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "float has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === Bool builtin methods === */
                invoke_bool:
                if (XR_IS_BOOL(receiver)) {
                    R(a) = bool_method_call_by_symbol(isolate, XR_TO_BOOL(receiver), method_symbol);
                    if (unlikely(XR_IS_NOTFOUND(R(a)))) {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "bool has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === BigInt builtin methods === */
                invoke_bigint:
                if (XR_IS_BIGINT(receiver)) {
                    XrBigInt *bigint = (XrBigInt*)XR_TO_PTR(receiver);
                    R(a) = bigint_method_call_by_symbol(isolate, bigint, method_symbol, &R(a + 2), nargs);
                    if (unlikely(XR_IS_NOTFOUND(R(a)))) {
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

                // Create inline cache table on demand (only for class methods)
                if (frame->closure->proto->ic_methods == NULL) {
                    int cache_count = PROTO_CODE_COUNT(frame->closure->proto);
                    XrICMethodTable *ict = xr_ic_method_table_new(cache_count);
                    ict->count = cache_count; // new() already zeroed all entries
                    frame->closure->proto->ic_methods = ict;
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
                    XrICMethod *cache = xr_ic_method_table_get(frame->closure->proto->ic_methods, cache_index);
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
                /* ========== OP_INVOKE_BUILTIN - Builtin type method call (direct dispatch) ==========
                 *
                 * Format: OP_INVOKE_BUILTIN A B C
                 *   A = base register (return value in R[A], this in R[A+1])
                 *   B = method symbol
                 *   C = nargs (argument count, excluding this)
                 *   D (high 8 bits) = type hint (BUILTIN_TYPE_*)
                 *
                 * Type determined at compile time, dispatch directly to handler, no runtime type check!
                 */
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int method_symbol = PROTO_SYMBOL(cl->proto, GETARG_B(i));
                int nargs = GETARG_C(i);

                /* Get receiver (pass register pointer directly, zero-copy)
                 * Note: xr_vm_call_closure uses safe starting point, won't overwrite current frame registers */
                XrValue receiver = R(a + 1);
                XrValue *args = &R(a + 2); // Pass pointer directly, no copy

                // Builtin type fast dispatch (compile-time determined as builtin type)
                if (XR_IS_MAP(receiver)) {
                    XrMap *map = XR_TO_MAP(receiver);
                    R(a) = map_method_call_by_symbol(isolate, map, method_symbol, args, nargs);
                } else if (XR_IS_ARRAY(receiver)) {
                    XrArray *array = XR_TO_ARRAY(receiver);
                    R(a) = array_method_call_by_symbol(isolate, array, method_symbol, args, nargs);
                } else if (XR_IS_STRING(receiver)) {
                    XrString *str = xr_value_to_string(isolate, receiver);
                    R(a) = string_method_call_by_symbol(isolate, str, method_symbol, args, nargs);
                } else if (XR_IS_SET(receiver)) {
                    XrSet *set = XR_TO_SET(receiver);
                    R(a) = set_method_call_by_symbol(isolate, set, method_symbol, args, nargs);
                } else if (xr_value_is_json(receiver)) {
                    XrJson *json = xr_value_to_json(receiver);
                    R(a) = json_method_call_by_symbol(isolate, json, method_symbol, args, nargs);
                } else if (XR_IS_INT(receiver)) {
                    xr_Integer value = XR_TO_INT(receiver);
                    R(a) = int_method_call_by_symbol(isolate, value, method_symbol, args, nargs);
                } else if (XR_IS_FLOAT(receiver)) {
                    xr_Number value = XR_TO_FLOAT(receiver);
                    R(a) = float_method_call_by_symbol(isolate, value, method_symbol, args, nargs);
                } else if (XR_IS_BOOL(receiver)) {
                    R(a) = bool_method_call_by_symbol(isolate, XR_TO_BOOL(receiver), method_symbol);
                    if (unlikely(XR_IS_NOTFOUND(R(a)))) {
                        XrSymbolTable *st = (XrSymbolTable*)isolate->symbol_table;
                        const char *name = xr_symbol_get_name_in_table(st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD,
                            "bool type has no method '%s'", name ? name : "?");
                    }
                } else if (XR_IS_BIGINT(receiver)) {
                    XrBigInt *bigint = (XrBigInt*)XR_TO_PTR(receiver);
                    R(a) = bigint_method_call_by_symbol(isolate, bigint, method_symbol, args, nargs);
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

                    // Ensure IC table exists (shared with Instance path)
                    if (frame->closure->proto->ic_fields == NULL) {
                        int cache_count = PROTO_CODE_COUNT(frame->closure->proto);
                        frame->closure->proto->ic_fields = xr_ic_field_table_new(cache_count);
                        for (int ji = 0; ji < cache_count; ji++) {
                            xr_ic_field_table_alloc(frame->closure->proto->ic_fields);
                        }
                    }
                    size_t jic_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
                    XrICField *jic = xr_ic_field_table_get(frame->closure->proto->ic_fields, (int)jic_index);

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

                // Field access Inline Cache optimization
                // Create field IC table on demand
                if (frame->closure->proto->ic_fields == NULL) {
                    int cache_count = PROTO_CODE_COUNT(frame->closure->proto);
                    frame->closure->proto->ic_fields = xr_ic_field_table_new(cache_count);

                    // Pre-allocate all cache entries
                    for (int i = 0; i < cache_count; i++) {
                        xr_ic_field_table_alloc(frame->closure->proto->ic_fields);
                    }
                }

                // Get IC for current instruction
                size_t cache_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
                XR_VM_IC_ASSERT_INDEX(cache_index, frame->closure->proto);
                XrICField *cache = xr_ic_field_table_get(frame->closure->proto->ic_fields, cache_index);
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

                    // Ensure IC table exists
                    if (frame->closure->proto->ic_fields == NULL) {
                        int cache_count = PROTO_CODE_COUNT(frame->closure->proto);
                        frame->closure->proto->ic_fields = xr_ic_field_table_new(cache_count);
                        for (int ji = 0; ji < cache_count; ji++) {
                            xr_ic_field_table_alloc(frame->closure->proto->ic_fields);
                        }
                    }
                    size_t jic_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
                    XrICField *jic = xr_ic_field_table_get(frame->closure->proto->ic_fields, (int)jic_index);

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

                // IC cache optimization: check or create field access cache
                if (!frame->closure->proto->ic_fields) {
                    int cache_count = PROTO_CODE_COUNT(frame->closure->proto);
                    frame->closure->proto->ic_fields = xr_ic_field_table_new(cache_count);
                    for (int j = 0; j < cache_count; j++) {
                        xr_ic_field_table_alloc(frame->closure->proto->ic_fields);
                    }
                }

                size_t cache_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
                XR_VM_IC_ASSERT_INDEX(cache_index, frame->closure->proto);
                XrICField *cache = xr_ic_field_table_get(frame->closure->proto->ic_fields, cache_index);
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

            /* ========================================================
            ** Exception handling instructions
            ** ======================================================== */

            vmcase(OP_TRY) {
                // Set exception handler
                TRACE_EXECUTION();
                int catch_offset = GETARG_Bx(i);

                // Read next instruction to get finally offset
                XrInstruction next_i = *pc++;
                int finally_offset = GETARG_Bx(next_i);

                // Lazy allocate / grow exception handler array
                if (VM_HANDLER_COUNT >= vm_ctx->handler_capacity) {
                    int new_cap = vm_ctx->handler_capacity == 0 ? 8 : vm_ctx->handler_capacity * 2;
                    if (new_cap > XR_EXCEPTION_HANDLERS_MAX) new_cap = XR_EXCEPTION_HANDLERS_MAX;
                    if (VM_HANDLER_COUNT >= new_cap) {
                        VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "exception handler nesting too deep");
                    }
                    XrExceptionHandler *new_h = (XrExceptionHandler *)xr_realloc(
                        vm_ctx->handlers, sizeof(XrExceptionHandler) * new_cap);
                    if (!new_h) {
                        VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "failed to allocate exception handlers");
                    }
                    vm_ctx->handlers = new_h;
                    vm_ctx->handler_capacity = new_cap;
                }

                int _hidx = VM_HANDLER_COUNT; VM_INC_HANDLER_COUNT;
                XrExceptionHandler *handler = &VM_HANDLERS[_hidx];
                handler->catch_offset = catch_offset;
                handler->finally_offset = finally_offset;
                handler->stack_size = (int)(VM_STACK_TOP - VM_STACK);
                handler->frame_count = VM_FRAME_COUNT;
                handler->exception = xr_null();
                handler->caught = false;
                handler->in_finally = false;
                handler->try_pc = pc - 2; // Save try instruction position

                vmbreak;
            }

            vmcase(OP_CATCH) {
                // Catch exception: R[A] = originally thrown value (or exception object)
                TRACE_EXECUTION();
                int a = GETARG_A(i);

                // Get current handler
                if (VM_HANDLER_COUNT > 0) {
                    XrExceptionHandler *handler = &VM_HANDLERS[VM_HANDLER_COUNT - 1];

                    // Store exception value in specified register
                    if (!XR_IS_NULL(handler->exception)) {
                        XrValue exc = handler->exception;

                        // If wrapped exception, return original value (userData)
                        if (XR_IS_EXCEPTION(exc)) {
                            XrException *ex = XR_AS_EXCEPTION(exc);
                            if (!XR_IS_NULL(ex->userData)) {
                                R(a) = ex->userData;
                            } else {
                                R(a) = exc;
                            }
                        } else {
                            R(a) = exc;
                        }
                        handler->caught = true;
                        // Clear consumed exception; if catch rethrows,
                        // xr_vm_throw_exception will set a new one.
                        handler->exception = xr_null();
                    }
                }

                vmbreak;
            }

            vmcase(OP_FINALLY) {
                // finally block start - mark handler so re-throw propagates outward
                TRACE_EXECUTION();
                if (VM_HANDLER_COUNT > 0) {
                    VM_HANDLERS[VM_HANDLER_COUNT - 1].in_finally = true;
                }
                vmbreak;
            }

            vmcase(OP_END_TRY) {
                // End try-catch-finally block
                TRACE_EXECUTION();

                if (VM_HANDLER_COUNT > 0) {
                    XrExceptionHandler *handler = &VM_HANDLERS[VM_HANDLER_COUNT - 1];

                    // Check for pending exception that needs re-throw:
                    // 1. Uncaught exception (try-finally without catch)
                    // 2. Exception thrown during catch, finally just finished
                    bool has_pending = !XR_IS_NULL(handler->exception) &&
                                       (!handler->caught || handler->in_finally);
                    if (has_pending) {
                        XrValue exc = handler->exception;
                        VM_DEC_HANDLER_COUNT; // Pop handler
                        xr_vm_throw_exception(isolate, exc);

                        // Check if there are upper handlers
                        if (VM_HANDLER_COUNT == 0) {
                            return XR_VM_RUNTIME_ERROR;
                        }
                        // Jump to upper handler
                        goto startfunc;
                    } else {
                        // Normal end, pop handler
                        VM_DEC_HANDLER_COUNT;
                    }
                }

                vmbreak;
            }

            vmcase(OP_THROW) {
                // Throw exception: throw R[A]
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                XrValue exception = R(a);

                // If not an exception object, wrap automatically
                if (!xr_is_exception(exception)) {
                    exception = xr_exception_from_value(isolate, exception);
                }

                // Add current position to stack trace
                xr_vm_add_stacktrace(isolate, exception);

                // Throw exception (stack unwinding)
                xr_vm_throw_exception(isolate, exception);

                // Check if uncaught exception
                if (VM_HANDLER_COUNT == 0) {
                    // Uncaught exception, return error
                    return XR_VM_RUNTIME_ERROR;
                }

                // Jump to catch or finally, continue execution
                goto startfunc;
            }

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

            /* ========================================================
            ** Module system instructions
            ** ======================================================== */

            vmcase(OP_IMPORT) {
                // R[A] = import(K[Bx]) - Import module
                int reg = GETARG_A(i);
                int bx = GETARG_Bx(i);
                XrValue module_name_val = K(bx);

                if (!XR_IS_STRING(module_name_val)) {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "import: module name must be a string");
                }

                // Ensure stack has space for result register
                VM_STACK_CHECK(reg + 1);
                // After stack check, base/ci/frame may have changed due to realloc
                frame = ci;

                /*
                 * Update stack_top to point to current frame's stack top
                 * This ensures module execution uses stack space that doesn't overlap with current frame's local variables
                 */
                VM_SET_STACK_TOP(base + frame->closure->proto->maxstacksize);

                /*
                 * SAFETY NOTE: module_name_val is from constant pool (K[bx]), not from stack.
                 * Stack reallocation (VM_STACK_CHECK) only affects stack pointers, not proto->constants.
                 * The XrString pointer remains valid across xr_module_import call.
                 */
                XrString *module_name = XR_TO_STRING(module_name_val);
                XrValue module_val = xr_module_import(isolate, module_name->data);

                /*
                 * Module import may cause stack reallocation (nested imports).
                 * Must refresh base pointer after import.
                 */
                base = VM_STACK + frame->base_offset;

                if (XR_IS_NULL(module_val)) {
                    VM_RUNTIME_ERROR(XR_ERR_MOD_LOAD_FAILED, "import: failed to load module '%s'", module_name->data);
                }

                R(reg) = module_val;
                vmbreak;
            }

            vmcase(OP_EXPORT) {
                // export(K[A], R[B], C) - Export value to current module, C=1 means constant
                int name_idx = GETARG_A(i);
                int value_reg = GETARG_B(i);
                int is_const = GETARG_C(i);

                XrValue name_val = K(name_idx);
                if (!XR_IS_STRING(name_val)) {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "export: name must be a string");
                }

                XrString *name = XR_TO_STRING(name_val);
                XrValue value = R(value_reg);

                // Add to current module export table (pass const flag)
                xr_module_add_current_export(isolate, name->data, value, is_const != 0);
                vmbreak;
            }

            vmcase(OP_EXPORT_ALL) {
                // export * from R[A] - Export all members from module to current module
                int module_reg = GETARG_A(i);
                XrValue module_val = R(module_reg);

                XrModule *src_module = xr_value_to_module(module_val);
                if (!src_module || src_module->export_count == 0) {
                    vmbreak;
                }

                // Iterate source module's flat export arrays
                XrModule *dst_module = isolate->current_module;
                if (dst_module) {
                    for (uint16_t idx = 0; idx < src_module->export_count; idx++) {
                        xr_module_add_export_sym(isolate, dst_module,
                            src_module->export_symbols[idx],
                            src_module->export_values[idx], false);
                    }
                }
                vmbreak;
            }

            /* === Assertion instructions (test framework) === */

            vmcase(OP_ASSERT) {
                /* if !R[A] then throw AssertError(K[B])
                 * A = condition register
                 * B = location info constant index (string)
                 * C = 0: assert/assert_true, 1: assert_false (negate)
                 */
                int cond_reg = GETARG_A(i);
                int loc_idx = GETARG_B(i);
                int negate = GETARG_C(i);

                XrValue cond = R(cond_reg);
                bool truthy = xr_vm_is_truthy(cond);

                // C=1: assert_false — fail if truthy
                bool failed = negate ? truthy : !truthy;

                if (failed) {
                    XrValue loc_val = K(loc_idx);
                    const char *loc_str = XR_IS_STRING(loc_val) ? XR_TO_STRING(loc_val)->data : "unknown";
                    const char *fn_name = negate ? "assert_false" : "assert";

                    if (!isolate->suppress_exception_print) {
                        fprintf(stderr, "\n");
                        fprintf(stderr, "ASSERTION FAILED at %s\n", loc_str);
                        fprintf(stderr, "  %s() condition is %s\n", fn_name, negate ? "true" : "false");
                        fprintf(stderr, "\n");
                    }
                    VM_RUNTIME_ERROR(0, "assertion failed at %s", loc_str);
                }
                vmbreak;
            }

            vmcase(OP_ASSERT_EQ) {
                /* if R[A] != R[B] then throw AssertError(K[C])
                 * A = actual value register
                 * B = expected value register
                 * C = location info constant index
                 */
                int actual_reg = GETARG_A(i);
                int expect_reg = GETARG_B(i);
                int loc_idx = GETARG_C(i);

                XrValue actual = R(actual_reg);
                XrValue expect = R(expect_reg);

                if (!xr_value_deep_eq(actual, expect)) {
                    XrValue loc_val = K(loc_idx);
                    const char *loc_str = XR_IS_STRING(loc_val) ? XR_TO_STRING(loc_val)->data : "unknown";
                    if (!isolate->suppress_exception_print) {
                        fprintf(stderr, "\n");
                        fprintf(stderr, "ASSERTION FAILED at %s\n", loc_str);
                        fprintf(stderr, "  assert_eq() values are not equal\n");
                        fprintf(stderr, "    actual:   %s\n", xr_value_to_string(isolate, actual)->data);
                        fprintf(stderr, "    expected: %s\n\n", xr_value_to_string(isolate, expect)->data);
                    }
                    VM_RUNTIME_ERROR(0, "assertion failed at %s: values not equal", loc_str);
                }
                vmbreak;
            }

            vmcase(OP_ASSERT_NE) {
                /* if R[A] == R[B] then throw AssertError(K[C])
                 * A = actual value register
                 * B = unexpected value register
                 * C = location info constant index
                 */
                int actual_reg = GETARG_A(i);
                int unexpected_reg = GETARG_B(i);
                int loc_idx = GETARG_C(i);

                XrValue actual = R(actual_reg);
                XrValue unexpected = R(unexpected_reg);

                if (xr_value_deep_eq(actual, unexpected)) {
                    XrValue loc_val = K(loc_idx);
                    const char *loc_str = XR_IS_STRING(loc_val) ? XR_TO_STRING(loc_val)->data : "unknown";

                    if (!isolate->suppress_exception_print) {
                        fprintf(stderr, "\n");
                        fprintf(stderr, "ASSERTION FAILED at %s\n", loc_str);
                        fprintf(stderr, "  assert_ne() values should not be equal\n");
                        fprintf(stderr, "    value: %s\n\n", xr_value_to_string(isolate, actual)->data);
                    }
                    VM_RUNTIME_ERROR(0, "assertion failed at %s: values should not be equal", loc_str);
                }
                vmbreak;
            }

            /* === Regex Literal === */
            vmcase(OP_REGEX_COMPILE) {
                /* R[A] = regex.compile(K[B], K[C])
                 * A = destination register
                 * B = pattern constant index
                 * C = flags constant index
                 */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);

                XrValue pattern_val = K(b);
                XrValue flags_val = K(c);

                // Get pattern and flags strings
                XrString *pattern_str = xr_value_to_string(isolate, pattern_val);
                XrString *flags_str = xr_value_to_string(isolate, flags_val);
                const char *pattern = pattern_str->data;
                const char *flags = flags_str->data;

                // Parse flags into XrRegexFlags
                XrRegexFlags regex_flags = 0;
                for (const char *p = flags; *p; p++) {
                    switch (*p) {
                        case 'i': regex_flags |= XR_RE_IGNORECASE; break;
                        case 'm': regex_flags |= XR_RE_MULTILINE; break;
                        case 's': regex_flags |= XR_RE_DOTALL; break;
                        // 'g' global flag not yet supported in xray
                    }
                }

                // Compile regex
                XrRegexError error;
                XrRegex *re = xr_regex_compile(pattern, regex_flags, &error);

                if (re) {
                    // Success: wrap as XrValue
                    R(a) = xr_regex_wrap(isolate, re);
                } else {
                    // Failure: return null
                    R(a) = xr_null();
                }
                vmbreak;
            }

            /* ========================================================
            ** Coroutine Instructions
            ** ======================================================== */

            vmcase(OP_GO) {
                TRACE_EXECUTION();
                ci->pc = pc;
                int _go_cr = vm_go(isolate, vm_ctx, i, base, ci);
                pc = ci->pc;
                VM_DISPATCH_COLD(_go_cr);
            }

            vmcase(OP_GO_INVOKE) {
                TRACE_EXECUTION();
                VM_DISPATCH_COLD(vm_go_invoke(isolate, vm_ctx, i, base, ci, pc));
            }

            vmcase(OP_SPAWN_CONT) {
                TRACE_EXECUTION();
                ci->pc = pc;
                int _sc_cr = vm_spawn_cont(isolate, vm_ctx, i, base, ci);
                pc = ci->pc;
                VM_DISPATCH_COLD(_sc_cr);
            }

            vmcase(OP_AWAIT) {
                TRACE_EXECUTION();
                /* Inline fast path: task completed with immediate value.
                 * Avoids noinline cold path call and defers executor recycle
                 * to next pool_get — matching channel recv hot path perf. */
                {
                    int _aw_a = GETARG_A(i);
                    XrValue _aw_tv = base[GETARG_B(i)];
                    if (xr_value_is_task(_aw_tv)) {
                        XrTask *_aw_task = xr_value_to_task(_aw_tv);
                        uint8_t _aw_st = atomic_load_explicit(
                            &_aw_task->state, memory_order_acquire);
                        if (_aw_st == XR_TASK_COMPLETED) {
                            XrValue _aw_res = _aw_task->result;
                            if (!XR_IS_PTR(_aw_res)) {
                                // Immediate value: no deep copy
                                base[_aw_a] = GETARG_C(i)
                                    ? xr_null() : _aw_res;
                                /* Detach executor only — do NOT recycle.
                                 * Task lives on executor's Immix heap;
                                 * parent's tasks array still references it.
                                 * Recycling frees the Immix block, causing
                                 * use-after-free when parent's GC scans
                                 * the dangling Task pointer. */
                                XrCoroutine *_aw_exec = _aw_task->coro;
                                if (_aw_exec) {
                                    _aw_task->coro = NULL;
                                    _aw_exec->task = NULL;
                                }
                                vmbreak;
                            }
                        }
                    }
                }
                VM_DISPATCH_COLD(vm_await(isolate, vm_ctx, i, base, ci, pc));
            }

            vmcase(OP_AWAIT_TIMEOUT) {
                TRACE_EXECUTION();
                VM_DISPATCH_COLD(vm_await_timeout(isolate, vm_ctx, i, base, ci, pc));
            }

            vmcase(OP_AWAIT_ALL) {
                TRACE_EXECUTION();
                VM_DISPATCH_COLD(vm_await_all(isolate, vm_ctx, i, base, ci, pc));
            }

            vmcase(OP_AWAIT_ANY) {
                TRACE_EXECUTION();
                VM_DISPATCH_COLD(vm_await_any(isolate, vm_ctx, i, base, ci, pc));
            }

            vmcase(OP_YIELD) {
                /* yield - cooperatively yield execution to the scheduler
                 *
                 * A=0: immediate yield (user explicit `yield` statement)
                 * A>0: hint yield (compiler-inserted, e.g. select default path)
                 *      Deducts A from reductions; only yields when reductions <= 0.
                 *      This avoids context-switch storms while still ensuring
                 *      fairness within a bounded number of iterations.
                 */
                XrCoroutine *current = (XrCoroutine *)VM_CURRENT_CORO;
                if (current != NULL) {
                    int hint = GETARG_A(i);
                    if (hint == 0) {
                        // Immediate yield
                        frame->pc = pc;
                        return XR_VM_YIELD;
                    }
                    // Hint yield: accelerate next scheduling point
                    current->reductions -= hint;
                    if (current->reductions <= 0) {
                        current->reductions = XR_CORO_REDUCTIONS;
                        frame->pc = pc;
                        return XR_VM_YIELD;
                    }
                }
                vmbreak;
            }

            vmcase(OP_CANCELLED) {
                // R[A] = cancelled() - check if cancelled
                int a = GETARG_A(i);
                R(a) = xr_bool(false); // Default: not cancelled
                vmbreak;
            }

            vmcase(OP_LOCK_THREAD) {
                // Coro.lockThread() - pin coro to current worker
                XrCoroutine *coro = (XrCoroutine *)VM_CURRENT_CORO;
                if (coro) {
                    XrCoroExt *lext = xr_coro_ensure_ext(coro);
                    if (lext) {
                        int old_count = atomic_fetch_add(&lext->lock_count, 1);
                        if (old_count == 0) {
                            XrWorker *worker = xr_current_worker();
                            lext->locked_worker = worker ? worker->p.id : 0;
                        }
                    }
                }
                vmbreak;
            }

            vmcase(OP_UNLOCK_THREAD) {
                // Coro.unlockThread() - unpin coro
                XrCoroutine *coro = (XrCoroutine *)VM_CURRENT_CORO;
                if (coro && coro->ext) {
                    int old_count = atomic_fetch_sub(&coro->ext->lock_count, 1);
                    if (old_count <= 1) {
                        atomic_store(&coro->ext->lock_count, 0);
                        coro->ext->locked_worker = -1;
                    }
                }
                vmbreak;
            }

            vmcase(OP_SET_LOCAL) {
                // Coro.setLocal(R[A], R[B])
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue key = R(a);
                XrValue value = R(b);
                XrCoroutine *current = (XrCoroutine *)VM_CURRENT_CORO;
                if (!current) {
                    if (!isolate->vm.main_locals) {
                        isolate->vm.main_locals = xr_map_new(VM_CURRENT_CORO);
                    }
                    xr_map_set(isolate->vm.main_locals, key, value);
                } else {
                    XrCoroExt *lext = xr_coro_ensure_ext(current);
                    if (lext) {
                        if (!lext->locals) {
                            lext->locals = xr_map_new(VM_CURRENT_CORO);
                        }
                        xr_map_set(lext->locals, key, value);
                    }
                }
                vmbreak;
            }

            vmcase(OP_GET_LOCAL) {
                // R[A] = Coro.getLocal(R[B])
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue key = R(b);
                XrCoroutine *current = (XrCoroutine *)VM_CURRENT_CORO;
                XrMap *locals = NULL;
                if (!current) {
                    locals = isolate->vm.main_locals;
                } else {
                    locals = current->ext ? current->ext->locals : NULL;
                }
                if (locals) {
                    bool found;
                    XrValue result = xr_map_get(locals, key, &found);
                    R(a) = found ? result : xr_null();
                } else {
                    R(a) = xr_null();
                }
                vmbreak;
            }

            vmcase(OP_SET_PRIORITY) {
                // Coro.setPriority(R[A], R[B])
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue task_val = R(a);
                XrValue prio_val = R(b);
                XrCoroutine *coro = NULL;
                if (xr_value_is_task(task_val)) {
                    XrTask *_t = xr_value_to_task(task_val);
                    coro = _t->coro;
                } else if (xr_value_is_coro(task_val)) {
                    coro = xr_value_to_coro(task_val);
                }
                if (!coro) {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "setPriority: task or coroutine object required");
                }
                XrCoroPriority new_prio = CORO_PRIORITY_NORMAL;
                if (XR_IS_INT(prio_val)) {
                    int prio_int = (int)XR_TO_INT(prio_val);
                    if (prio_int >= 0 && prio_int < XR_CORO_PRIORITY_COUNT) {
                        new_prio = (XrCoroPriority)prio_int;
                    }
                }
                int old_prio = xr_coro_get_priority(xr_coro_flags_load(coro));
                if (xr_coro_flags_has(coro, XR_CORO_FLG_READY) && old_prio != (int)new_prio) {
                    XrScheduler *sched = (XrScheduler *)isolate->vm.scheduler;
                    if (sched) {
                        xr_sched_remove(sched, coro);
                        uint32_t old_flags = xr_coro_flags_load(coro);
                        atomic_store(&coro->flags, xr_coro_set_priority_flags(old_flags, new_prio));
                        xr_sched_enqueue(sched, coro);
                    }
                } else {
                    uint32_t old_flags = xr_coro_flags_load(coro);
                    atomic_store(&coro->flags, xr_coro_set_priority_flags(old_flags, new_prio));
                }
                vmbreak;
            }

            vmcase(OP_CORO_CTRL) {
                // Cold path: all coro monitoring/diagnostics sub-operations
                vm_coro_ctrl(isolate, vm_ctx, i, base);
                vmbreak;
            }

            /* ========================================================
            ** Channel Instructions
            ** ======================================================== */

            vmcase(OP_CHAN_NEW) {
                /* R[A] = Channel(Bx) - create Channel (GC managed)
                 * Bx = buffer size (18 bits, supports 0~262143)
                 */
                int a = GETARG_A(i);
                int buffer_size = GETARG_Bx(i);

                // Create GC-managed Channel
                XrChannel *ch = xr_channel_new(isolate, (uint32_t)buffer_size);
                if (!ch) {
                    VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "Channel creation failed");
                }

                // Store directly as Channel value
                R(a) = xr_value_from_channel(ch);
                vmbreak;
            }

            vmcase(OP_CHAN_NEW_NAMED) {
                /* R[A] = Channel(R[B], R[C]) - Named Channel
                 * R[B] = buffer size (int)
                 * R[C] = channel name (string)
                 * If cluster is running, registers as Named Channel.
                 * Otherwise creates a normal local channel.
                 */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);

                uint32_t buf_size = 0;
                if (XR_IS_INT(R(b))) {
                    int64_t v = XR_TO_INT(R(b));
                    if (v > 0 && v <= 262143) buf_size = (uint32_t)v;
                }

                // Check for existing Named Channel (e.g. Proxy from CHANNEL_SYNC)
#ifdef XR_HAS_CLUSTER
                if (XR_IS_STRING(R(c))) {
                    if (xr_cluster_is_running()) {
                        XrChannel *existing_ch = xr_cluster_find_channel_local(
                            XR_TO_STRING(R(c))->data);
                        if (existing_ch) {
                            R(a) = xr_value_from_channel(existing_ch);
                            vmbreak;
                        }
                    }
                }
#endif

                XrChannel *ch = xr_channel_new(isolate, buf_size);
                if (!ch) {
                    VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "Channel creation failed");
                }

                // Register as Named Channel if cluster is running and name is string
                if (XR_IS_STRING(R(c))) {
                    XrString *name_str = XR_TO_STRING(R(c));
#ifdef XR_HAS_CLUSTER
                    if (xr_cluster_is_running()) {
                        xr_cluster_register_channel(name_str->data, ch);
                    }
#else
                    (void)name_str;
#endif
                }

                R(a) = xr_value_from_channel(ch);
                vmbreak;
            }

            vmcase(OP_CHAN_SEND) {
                /* R[B].send(R[C]) - blocking send to Channel
                 * A = result register (null on success)
                 * B = Channel
                 * C = value to send
                 */
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);

                // Check if resumed from blocking
                XrCoroutine *current = (XrCoroutine *)VM_CURRENT_CORO;
                if (current && xr_coro_resume_load(current) == XR_RESUME_CHANNEL) {
                    xr_coro_resume_store(current, XR_RESUME_OK);
                    R(a) = xr_null();
                    vmbreak;
                }

                // Get Channel
                XrValue ch_val = R(b);
                if (!xr_value_is_channel(ch_val)) {
                    R(a) = xr_null();
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "send: expected Channel");
                }
                XrChannel *ch = xr_value_to_channel(ch_val);

                // Deep copy mutable values for buffer safety
                XrValue send_v = vm_chan_copy_send(isolate, R(c));

                // Pre-save frame
                if (current) current->send_value = send_v;
                savepc();
                frame->pc = pc - 1;
                frame->call_status |= XR_CALL_YIELDED;
                // Blocking send
                XrChanResult result = xr_channel_send(ch, send_v, current);
                if (result == XR_CHAN_OK) {
                    frame->call_status &= ~XR_CALL_YIELDED;
                    R(a) = xr_null();
                    vmbreak;
                } else if (result == XR_CHAN_CLOSED) {
                    frame->call_status &= ~XR_CALL_YIELDED;
                    VM_RUNTIME_ERROR(XR_ERR_CORO_DEAD, "Channel is closed");
                } else if (result == XR_CHAN_BLOCK) {
                    return XR_VM_BLOCKED;
                } else {
                    frame->call_status &= ~XR_CALL_YIELDED;
                    VM_RUNTIME_ERROR(XR_ERR_CORO_DEAD, "Channel send failed");
                }
            }

            vmcase(OP_CHAN_RECV) {
                /* R[A], R[A+1] = R[B].recv() - receive from Channel (blocking), returns multi-value
                 * R[A] = received value
                 * R[A+1] = success (bool)
                 * B = Channel
                 */
                int a = GETARG_A(i);
                int b = GETARG_B(i);

                // Check if resumed from blocking (cache resume_load: 1 atomic instead of 2)
                XrCoroutine *current = (XrCoroutine *)VM_CURRENT_CORO;
                if (current) {
                    int _rs = xr_coro_resume_load(current);
                    if (_rs == XR_RESUME_CHANNEL) {
                        xr_coro_resume_store(current, XR_RESUME_OK);
                        current->wait_channel = NULL;
                        R(a) = vm_chan_copy_recv(isolate, R(a), vm_ctx);
                        R(a + 1) = xr_bool(true);
                        vmbreak;
                    }
                    if (_rs == XR_RESUME_CHANNEL_CLOSED) {
                        xr_coro_resume_store(current, XR_RESUME_OK);
                        current->wait_channel = NULL;
                    }
                }

                // Get Channel directly
                XrValue ch_val = R(b);
                if (!xr_value_is_channel(ch_val)) {
                    R(a) = xr_null();
                    R(a + 1) = xr_bool(false);
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "recv: expected Channel");
                }
                XrChannel *ch = xr_value_to_channel(ch_val);

                // Set recv_slot before recv — see hot path comment for rationale
                if (current) current->recv_slot = &R(a);
                // Pre-save frame
                savepc();
                frame->pc = pc - 1;
                frame->call_status |= XR_CALL_YIELDED;
                XrValue value;
                XrChanResult result = xr_channel_recv(ch, &value, current);
                if (result == XR_CHAN_OK) {
                    frame->call_status &= ~XR_CALL_YIELDED;
                    R(a) = vm_chan_copy_recv(isolate, value, vm_ctx);
                    R(a + 1) = xr_bool(true);
                    vmbreak;
                } else if (result == XR_CHAN_CLOSED) {
                    frame->call_status &= ~XR_CALL_YIELDED;
                    R(a) = xr_null();
                    R(a + 1) = xr_bool(false);
                    vmbreak;
                } else if (result == XR_CHAN_BLOCK) {
                    return XR_VM_BLOCKED;
                } else {
                    frame->call_status &= ~XR_CALL_YIELDED;
                    R(a) = xr_null();
                    R(a + 1) = xr_bool(false);
                    VM_RUNTIME_ERROR(XR_ERR_CORO_DEAD, "recv: need to use blocking recv in coroutine");
                }
            }

            vmcase(OP_CHAN_TRY_SEND) {
                /* R[A] = R[B].trySend(R[C]) - non-blocking send
                 * A = result (bool)
                 * B = Channel
                 * C = value to send
                 */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);

                // Get Channel directly
                XrValue ch_val = R(b);
                if (!xr_value_is_channel(ch_val)) {
                    R(a) = xr_bool(false);
                    vmbreak;
                }
                XrChannel *ch = xr_value_to_channel(ch_val);

                // Non-blocking send (deep copy mutable values for buffer safety)
                XrValue _send_v = vm_chan_copy_send(isolate, R(c));
                bool success = xr_channel_try_send(ch, _send_v);
                R(a) = xr_bool(success);

                // Send succeeded, wake waiting receivers
                if (success) {
                    xr_runtime_wake_channel(isolate, ch, false); // Wake receivers
                }
                vmbreak;
            }

            vmcase(OP_CHAN_TRY_RECV) {
                /* R[A], R[A+1] = R[B].tryRecv() - non-blocking receive, returns multi-value
                 * R[A] = received value (null on failure)
                 * R[A+1] = success (bool)
                 * B = Channel
                 */
                int a = GETARG_A(i);
                int b = GETARG_B(i);

                // Get Channel directly
                XrValue ch_val = R(b);
                if (!xr_value_is_channel(ch_val)) {
                    R(a) = xr_null();
                    R(a + 1) = xr_bool(false);
                    vmbreak;
                }
                XrChannel *ch = xr_value_to_channel(ch_val);

                // Timer Channel special handling
                if (xr_channel_timer_ready(ch)) {
                    // Timer triggered, read from buffer
                    bool ok;
                    XrValue value = xr_channel_try_recv(ch, &ok);
                    R(a) = ok ? vm_chan_copy_recv(isolate, value, vm_ctx) : xr_null();
                    R(a + 1) = xr_bool(ok);
                    vmbreak;
                }

                // Non-blocking receive
                bool ok;
                XrValue value = xr_channel_try_recv(ch, &ok);

                // Unbuffered Channel rendezvous: try to wake sender from Runtime queue
                if (!ok) {
                    XrCoroutine *sender = xr_runtime_wake_channel(isolate, ch, true);
                    if (sender) {
                        value = sender->send_value;
                        ok = true;
                    }
                }

                // Return multi-value: value and ok
                R(a) = ok ? vm_chan_copy_recv(isolate, value, vm_ctx) : xr_null();
                R(a + 1) = xr_bool(ok);

                // Receive succeeded, wake waiting senders
                if (ok) {
                    xr_runtime_wake_channel(isolate, ch, true); // Wake senders
                }
                vmbreak;
            }

            vmcase(OP_CHAN_SEND_TIMEOUT) {
                TRACE_EXECUTION();
                VM_DISPATCH_COLD(vm_chan_send_timeout(isolate, vm_ctx, i, base, ci, pc));
            }

            vmcase(OP_CHAN_RECV_TIMEOUT) {
                TRACE_EXECUTION();
                VM_DISPATCH_COLD(vm_chan_recv_timeout(isolate, vm_ctx, i, base, ci, pc));
            }

            vmcase(OP_CHAN_CLOSE) {
                // R[A].close() - close Channel
                int a = GETARG_A(i);

                // Get Channel directly
                XrValue ch_val = R(a);
                if (!xr_value_is_channel(ch_val)) {
                    vmbreak; // Silently ignore non-Channel
                }
                XrChannel *ch = xr_value_to_channel(ch_val);

                // Close Channel
                xr_channel_close(ch);

                // Wake all waiting coroutines
                xr_runtime_wake_channel_all(isolate, ch);
                vmbreak;
            }

            vmcase(OP_CHAN_IS_CLOSED) {
                // R[A] = R[B].isClosed() - check if Channel is closed
                int a = GETARG_A(i);
                int b = GETARG_B(i);

                XrValue ch_val = R(b);
                if (!xr_value_is_channel(ch_val)) {
                    R(a) = xr_bool(false);
                    vmbreak;
                }
                XrChannel *ch = xr_value_to_channel(ch_val);

                R(a) = xr_bool(xr_channel_is_closed(ch));
                vmbreak;
            }

            /* === Select multiplexing === */

            vmcase(OP_SELECT_START) {
                // Start select block
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_CALL, "select not yet implemented");
            }

            vmcase(OP_SELECT_CASE) {
                // Add select case
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_CALL, "select case not yet implemented");
            }

            vmcase(OP_SELECT_END) {
                // Execute select
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_CALL, "select end not yet implemented");
            }

            /* ========================================================
            ** Defer delayed execution instructions
            ** ======================================================== */

            vmcase(OP_DEFER) {
                /* OP_DEFER A B - push closure and args to defer stack
                 * A = closure register
                 * B = argument count (args at R[A+1]..R[A+B])
                 *
                 * defer stack storage format (each entry):
                 *   [0] = closure
                 *   [1] = argument count (integer)
                 *   [2..n+1] = argument values
                 */
                int a = GETARG_A(i);
                int b = GETARG_B(i); // Argument count
                XrValue closure_val = R(a);

                // Required stack space: closure + arg count + arg values
                int needed = 2 + b;

                // Lazy allocate defer stack
                if (isolate->vm.defer_stack == NULL) {
                    isolate->vm.defer_capacity = XR_DEFER_ENTRIES_MAX;
                    isolate->vm.defer_stack = xr_malloc(sizeof(XrValue) * isolate->vm.defer_capacity);
                    isolate->vm.defer_frame_marks = xr_malloc(sizeof(int) * XR_FRAMES_MAX);
                    for (int j = 0; j < XR_FRAMES_MAX; j++) {
                        isolate->vm.defer_frame_marks[j] = 0;
                    }
                }

                // Capacity expansion check
                while (isolate->vm.defer_count + needed > isolate->vm.defer_capacity) {
                    isolate->vm.defer_capacity *= 2;
                    XR_REALLOC_OR_ABORT(isolate->vm.defer_stack,
                        sizeof(XrValue) * (size_t)isolate->vm.defer_capacity,
                        "vm defer_stack grow");
                }

                // Push to defer stack: closure + arg count + args
                isolate->vm.defer_stack[isolate->vm.defer_count++] = closure_val;
                isolate->vm.defer_stack[isolate->vm.defer_count++] = xr_int(b);
                for (int j = 0; j < b; j++) {
                    isolate->vm.defer_stack[isolate->vm.defer_count++] = R(a + 1 + j);
                }
                vmbreak;
            }

            /* ========================================================
            ** Bytes array instructions
            ** ======================================================== */

            vmcase(OP_BYTES_NEW) {
                /* R[A] = Bytes(R[A+1..A+B]) - create Array<uint8>
                 * A = result register
                 * B = argument count
                 * C = storage_mode (0=normal, 1=shared)
                 */
                int a = GETARG_A(i);
                int nargs = GETARG_B(i);
                int storage_mode = GETARG_C(i);

                int32_t len = 0;
                uint8_t fill_val = 0;
                bool has_fill = false;
                XrArray *src_arr = NULL;

                if (nargs == 0) {
                    len = 0;
                } else if (nargs == 1) {
                    XrValue arg = R(a + 1);
                    if (XR_IS_INT(arg)) {
                        len = (int32_t)XR_TO_INT(arg);
                        if (len < 0) len = 0;
                        has_fill = true;
                        fill_val = 0;
                    } else if (XR_IS_ARRAY(arg)) {
                        src_arr = XR_TO_ARRAY(arg);
                        len = src_arr->length;
                    } else {
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Bytes(n): n must be integer or array");
                    }
                } else if (nargs == 2) {
                    XrValue arg1 = R(a + 1);
                    XrValue arg2 = R(a + 2);
                    if (!XR_IS_INT(arg1) || !XR_IS_INT(arg2)) {
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Bytes(n, value): both args must be integers");
                    }
                    len = (int32_t)XR_TO_INT(arg1);
                    if (len < 0) len = 0;
                    fill_val = (uint8_t)(XR_TO_INT(arg2) & 0xFF);
                    has_fill = true;
                } else {
                    VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "Bytes() requires 0, 1 or 2 arguments");
                }

                XrArray *arr = NULL;
                if (storage_mode != 0 && isolate->sys_heap) {
                    // Shared: allocate on system heap
                    arr = (XrArray*)xr_sysheap_alloc_shared(isolate->sys_heap, sizeof(XrArray), XR_TARRAY);
                    if (arr) {
                        xr_array_init_inplace(arr, len > 0 ? len : 4, XR_ELEM_U8);
                        XR_GC_SET_STORAGE(&arr->gc, XR_GC_STORAGE_SHARED);
                        xr_shared_set_refc(&arr->gc, 1);
                    }
                } else {
                    arr = xr_array_with_capacity_typed(VM_CURRENT_CORO, len > 0 ? len : 0, XR_ELEM_U8);
                }

                if (arr) {
                    if (src_arr) {
                        // Copy from source array
                        uint8_t *dst = (uint8_t*)arr->data;
                        for (int32_t j = 0; j < len; j++) {
                            XrValue elem = ((XrValue*)src_arr->data)[j];
                            dst[j] = XR_IS_INT(elem) ? (uint8_t)(XR_TO_INT(elem) & 0xFF) : 0;
                        }
                        arr->length = len;
                    } else if (has_fill && len > 0) {
                        memset(arr->data, fill_val, len);
                        arr->length = len;
                    }
                }

                R(a) = arr ? xr_value_from_array(arr) : xr_null();
                if (storage_mode == 0) checkGC(base + a + 1);
                vmbreak;
            }

            /* ========================================================
            ** Scope structured concurrency instructions
            ** ======================================================== */

            vmcase(OP_SCOPE_ENTER) {
                // Enter structured concurrency scope
                XrCoroutine *current = (XrCoroutine *)VM_CURRENT_CORO;
                if (current) {
                    atomic_store(&current->wait_count, 0);
                    atomic_store(&current->any_done, false);
                }

                // Create scope context — per-coroutine tracking
                int scope_mode = GETARG_A(i);
                XrScopeContext *scope = (XrScopeContext *)xr_malloc(sizeof(XrScopeContext));
                if (scope) {
                    atomic_store(&scope->count, 0);
                    scope->mode = (uint8_t)scope_mode;
                    atomic_store(&scope->cancel_requested, false);
                    scope->first_error = xr_null();
                    scope->errors = NULL;
                    scope->first_child = NULL;
                    if (current) {
                        scope->parent = current->current_scope;
                        current->current_scope = scope;
                    } else {
                        // Main thread fallback: use scheduler global
                        XrScheduler *sched = (XrScheduler *)isolate->vm.scheduler;
                        if (sched) {
                            scope->parent = sched->current_scope;
                            sched->current_scope = scope;
                        }
                    }
                }
                vmbreak;
            }

            vmcase(OP_SCOPE_EXIT) {
                /* Exit structured concurrency scope.
                 * A = scope_mode, B = result_reg (supervisor: errors[]) */
                int scope_mode = GETARG_A(i);
                int result_reg = GETARG_B(i);
                XrCoroutine *current = (XrCoroutine *)VM_CURRENT_CORO;

                if (current) {
                    XrScopeContext *scope = current->current_scope;
                    if (!scope) vmbreak;

                    if (atomic_load(&current->wait_count) > 0) {
                        // Children still running — block and re-execute on resume
                        frame->pc = pc - 1;
                        uint32_t old_flags = xr_coro_flags_load(current);
                        atomic_store(&current->flags,
                            xr_coro_set_wait_reason_flags(old_flags, XR_CORO_WAIT_SCOPE >> XR_CORO_WAIT_SHIFT));
                        return XR_VM_BLOCKED;
                    }

                    // All children done
                    if (scope_mode == XR_SCOPE_LINKED && !XR_IS_NULL(scope->first_error)) {
                        // linked scope: throw first error
                        XrValue err = scope->first_error;
                        current->current_scope = scope->parent;
                        xr_free(scope);
                        XrValue exc = err;
                        if (!xr_is_exception(exc)) {
                            exc = xr_exception_from_value(isolate, exc);
                        }
                        savepc();
                        xr_vm_add_stacktrace(isolate, exc);
                        xr_vm_throw_exception(isolate, exc);
                        if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                        goto startfunc;
                    }
                    if (scope_mode == XR_SCOPE_SUPERVISOR) {
                        // supervisor scope: write collected errors[] to result_reg
                        if (scope->errors && scope->errors->length > 0) {
                            base[result_reg] = xr_value_from_array(scope->errors);
                        } else {
                            XrArray *empty = xr_array_new(current);
                            base[result_reg] = empty ? xr_value_from_array(empty)
                                                     : xr_null();
                        }
                    }
                    current->current_scope = scope->parent;
                    xr_free(scope);
                } else {
                    // Main thread fallback
                    XrScheduler *sched = (XrScheduler *)isolate->vm.scheduler;
                    if (!sched || !sched->current_scope) vmbreak;

                    XrScopeContext *scope = sched->current_scope;
                    int spin = 0;
                    while (atomic_load(&scope->count) > 0) {
                        if (++spin > 1000) { spin = 0; sched_yield(); }
                    }
                    if (scope_mode == XR_SCOPE_SUPERVISOR) {
                        // Main thread: no coro for array alloc, use null
                        if (scope->errors && scope->errors->length > 0) {
                            base[result_reg] = xr_value_from_array(scope->errors);
                        } else {
                            base[result_reg] = xr_null();
                        }
                    }
                    sched->current_scope = scope->parent;
                    xr_free(scope);
                }
                vmbreak;
            }

            /* ========================================================
            ** Other auxiliary instructions
            ** ======================================================== */

            vmcase(OP_TIME_AFTER) {
                /* R[A] = time.after(R[B]) - create Timer Channel
                 * A = target register
                 * B = timeout register (milliseconds)
                 */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue timeout_val = R(b);

                int64_t timeout_ms = 0;
                if (XR_IS_INT(timeout_val)) {
                    timeout_ms = XR_TO_INT(timeout_val);
                } else if (XR_IS_FLOAT(timeout_val)) {
                    timeout_ms = (int64_t)XR_TO_FLOAT(timeout_val);
                }

                if (timeout_ms < 0) timeout_ms = 0;

                // Create Timer Channel
                XrChannel *timer_ch = xr_channel_new_timer(isolate, timeout_ms);
                if (!timer_ch) {
                    VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "time.after: out of memory");
                }

                R(a) = xr_value_from_channel(timer_ch);
                vmbreak;
            }

            vmcase(OP_SLEEP) {
                /* time.sleep(R[A]) - coroutine-friendly sleep
                 * A = sleep time (milliseconds, int)
                 *
                 * Coroutine mode: set wake time, yield CPU
                 * Non-coroutine mode: blocking sleep
                 */
                int a = GETARG_A(i);
                XrValue val = R(a);

                int64_t milliseconds = 0;
                if (XR_IS_INT(val)) {
                    milliseconds = XR_TO_INT(val);
                } else if (XR_IS_FLOAT(val)) {
                    milliseconds = (int64_t)XR_TO_FLOAT(val);
                }

                if (milliseconds <= 0) {
                    vmbreak;
                }

                // Check if in coroutine
                XrCoroutine *coro = (XrCoroutine *)VM_CURRENT_CORO;
                if (coro) {
                    // Coroutine mode: use Timer Wheel for precise timed wake
                    XrRuntime *rt = (XrRuntime *)isolate->vm.runtime;
                    (void)rt;

                    // First set as pure sleep (no fd)
                    XrCoroExt *sleep_ext = xr_coro_ensure_ext(coro);
                    if (sleep_ext) {
                        sleep_ext->yield_info.wait_fd = -1;
                        sleep_ext->yield_info.wait_events = 0;
                    }
                    // Set wait reason in flags
                    uint32_t old_flags = xr_coro_flags_load(coro);
                    uint32_t new_flags = xr_coro_set_wait_reason_flags(old_flags, XR_CORO_WAIT_SLEEP >> XR_CORO_WAIT_SHIFT);
                    atomic_store(&coro->flags, new_flags);

                    // Add timer to current Worker's Timer Wheel (Per-Worker lock-free)
                    XrWorker *worker = xr_current_worker();
                    XrTimerWheel *tw = worker ? worker->p.timer_wheel : NULL;

                    if (tw) {
                        // Use Per-Worker Timer Wheel (lock-free)
                        XR_DBG_TIMER("Add timer: coro=%d, ms=%lld, worker=%d, tw=%p",
                                     coro->id, (long long)milliseconds, worker->p.id, (void*)tw);
                        xr_worker_add_sleep_timer(worker, coro, milliseconds);
                    } else {
                        /* Timer wheel must exist for all workers.
                         * If missing, fall back to blocking sleep to avoid
                         * setting timer_active without timer wheel registration. */
                        usleep((useconds_t)(milliseconds * 1000));
                        vmbreak;
                    }

                    // Mark as pure sleep (no fd) - already set above

                    // Save current instruction address, re-execute on resume
                    frame->pc = pc;

                    // Return BLOCKED, let scheduler switch to other coroutines
                    return XR_VM_BLOCKED;
                }

                // Non-coroutine mode: blocking sleep - milliseconds to microseconds
                usleep((useconds_t)(milliseconds * 1000));
                vmbreak;
            }

            vmcase(OP_SELECT_BLOCK) {
                TRACE_EXECUTION();
                VM_DISPATCH_COLD(vm_select_block(isolate, vm_ctx, i, base, ci, pc));
            }

            /* === Typed Compact Storage (raw in, raw out, zero BOX/UNBOX) === */

            vmcase(OP_TARRAY_GET) {
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrArray *arr = XR_TO_ARRAY(R(b));
                int32_t idx = (int32_t)R(c).i;
                if (unlikely(idx < 0 || idx >= arr->length)) {
                    VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                        "typed array index %d out of bounds [0, %d)", idx, arr->length);
                }
                switch (arr->elem_type) {
                case XR_ELEM_I64:  XR_SET_INT(R(a), ((int64_t*)arr->data)[idx]); break;
                case XR_ELEM_F64:  XR_SET_FLOAT(R(a), ((double*)arr->data)[idx]); break;
                case XR_ELEM_I32:  XR_SET_INT(R(a), (int64_t)((int32_t*)arr->data)[idx]); break;
                case XR_ELEM_U8:   XR_SET_INT(R(a), (int64_t)((uint8_t*)arr->data)[idx]); break;
                case XR_ELEM_I8:   XR_SET_INT(R(a), (int64_t)((int8_t*)arr->data)[idx]); break;
                case XR_ELEM_U16:  XR_SET_INT(R(a), (int64_t)((uint16_t*)arr->data)[idx]); break;
                case XR_ELEM_I16:  XR_SET_INT(R(a), (int64_t)((int16_t*)arr->data)[idx]); break;
                case XR_ELEM_U32:  XR_SET_INT(R(a), (int64_t)((uint32_t*)arr->data)[idx]); break;
                case XR_ELEM_U64:  XR_SET_INT(R(a), (int64_t)((uint64_t*)arr->data)[idx]); break;
                case XR_ELEM_F32:  XR_SET_FLOAT(R(a), (double)((float*)arr->data)[idx]); break;
                case XR_ELEM_BOOL: XR_SET_INT(R(a), (int64_t)((uint8_t*)arr->data)[idx]); break;
                default:
                    R(a) = ((XrValue*)arr->data)[idx];
                    break;
                }
                vmbreak;
            }

            vmcase(OP_TARRAY_GETC) {
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrArray *arr = XR_TO_ARRAY(R(b));
                if (unlikely(c < 0 || c >= arr->length)) {
                    VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                        "typed array index %d out of bounds [0, %d)", c, arr->length);
                }
                switch (arr->elem_type) {
                case XR_ELEM_I64:  XR_SET_INT(R(a), ((int64_t*)arr->data)[c]); break;
                case XR_ELEM_F64:  XR_SET_FLOAT(R(a), ((double*)arr->data)[c]); break;
                case XR_ELEM_I32:  XR_SET_INT(R(a), (int64_t)((int32_t*)arr->data)[c]); break;
                case XR_ELEM_U8:   XR_SET_INT(R(a), (int64_t)((uint8_t*)arr->data)[c]); break;
                case XR_ELEM_I8:   XR_SET_INT(R(a), (int64_t)((int8_t*)arr->data)[c]); break;
                case XR_ELEM_U16:  XR_SET_INT(R(a), (int64_t)((uint16_t*)arr->data)[c]); break;
                case XR_ELEM_I16:  XR_SET_INT(R(a), (int64_t)((int16_t*)arr->data)[c]); break;
                case XR_ELEM_U32:  XR_SET_INT(R(a), (int64_t)((uint32_t*)arr->data)[c]); break;
                case XR_ELEM_U64:  XR_SET_INT(R(a), (int64_t)((uint64_t*)arr->data)[c]); break;
                case XR_ELEM_F32:  XR_SET_FLOAT(R(a), (double)((float*)arr->data)[c]); break;
                case XR_ELEM_BOOL: XR_SET_INT(R(a), (int64_t)((uint8_t*)arr->data)[c]); break;
                default:
                    R(a) = ((XrValue*)arr->data)[c];
                    break;
                }
                vmbreak;
            }

            vmcase(OP_TARRAY_SET) {
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrArray *arr = XR_TO_ARRAY(R(a));
                int32_t idx = (int32_t)R(b).i;
                if (unlikely(idx < 0 || idx >= arr->length)) {
                    VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                        "typed array index %d out of bounds [0, %d)", idx, arr->length);
                }
                switch (arr->elem_type) {
                case XR_ELEM_I64:  ((int64_t*)arr->data)[idx] = R(c).i; break;
                case XR_ELEM_F64:  ((double*)arr->data)[idx] = R(c).f; break;
                case XR_ELEM_I32:  ((int32_t*)arr->data)[idx] = (int32_t)R(c).i; break;
                case XR_ELEM_U8:   ((uint8_t*)arr->data)[idx] = (uint8_t)R(c).i; break;
                case XR_ELEM_I8:   ((int8_t*)arr->data)[idx] = (int8_t)R(c).i; break;
                case XR_ELEM_U16:  ((uint16_t*)arr->data)[idx] = (uint16_t)R(c).i; break;
                case XR_ELEM_I16:  ((int16_t*)arr->data)[idx] = (int16_t)R(c).i; break;
                case XR_ELEM_U32:  ((uint32_t*)arr->data)[idx] = (uint32_t)R(c).i; break;
                case XR_ELEM_U64:  ((uint64_t*)arr->data)[idx] = (uint64_t)R(c).i; break;
                case XR_ELEM_F32:  ((float*)arr->data)[idx] = (float)R(c).f; break;
                case XR_ELEM_BOOL: ((uint8_t*)arr->data)[idx] = (uint8_t)R(c).i; break;
                default:
                    // ANY array: BOX raw int64 to tagged XrValue
                    ((XrValue*)arr->data)[idx] = xr_int(R(c).i);
                    break;
                }
                vmbreak;
            }

            vmcase(OP_TARRAY_PUSH) {
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrArray *arr = XR_TO_ARRAY(R(a));
                if (unlikely(xr_array_is_slice(arr))) {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "cannot push to array slice");
                }
                if (arr->length >= arr->capacity) {
                    xr_array_grow(arr);
                }
                int32_t idx = arr->length++;
                switch (arr->elem_type) {
                case XR_ELEM_I64:  ((int64_t*)arr->data)[idx] = R(b).i; break;
                case XR_ELEM_F64:  ((double*)arr->data)[idx] = R(b).f; break;
                case XR_ELEM_I32:  ((int32_t*)arr->data)[idx] = (int32_t)R(b).i; break;
                case XR_ELEM_U8:   ((uint8_t*)arr->data)[idx] = (uint8_t)R(b).i; break;
                case XR_ELEM_I8:   ((int8_t*)arr->data)[idx] = (int8_t)R(b).i; break;
                case XR_ELEM_U16:  ((uint16_t*)arr->data)[idx] = (uint16_t)R(b).i; break;
                case XR_ELEM_I16:  ((int16_t*)arr->data)[idx] = (int16_t)R(b).i; break;
                case XR_ELEM_U32:  ((uint32_t*)arr->data)[idx] = (uint32_t)R(b).i; break;
                case XR_ELEM_U64:  ((uint64_t*)arr->data)[idx] = (uint64_t)R(b).i; break;
                case XR_ELEM_F32:  ((float*)arr->data)[idx] = (float)R(b).f; break;
                case XR_ELEM_BOOL: ((uint8_t*)arr->data)[idx] = (uint8_t)R(b).i; break;
                default:
                    // ANY array: BOX raw int64 to tagged XrValue
                    ((XrValue*)arr->data)[idx] = xr_int(R(b).i);
                    break;
                }
                vmbreak;
            }

            vmcase(OP_TFIELD_GET) {
                TRACE_EXECUTION();
                XrJson *json = (XrJson*)XR_TO_PTR(R(GETARG_B(i)));
                R(GETARG_A(i)) = json->fields[GETARG_C(i)];
                vmbreak;
            }

            vmcase(OP_TFIELD_SET) {
                TRACE_EXECUTION();
                XrJson *json = (XrJson*)XR_TO_PTR(R(GETARG_A(i)));
                XrValue _tfv = R(GETARG_C(i));
                json->fields[GETARG_B(i)] = _tfv;
                VM_BARRIER_VAL(json, _tfv);
                vmbreak;
            }

            vmcase(OP_INST_TYPE_ARGS) {
                /* OP_INST_TYPE_ARGS: set reified type args on instance
                ** A = instance register
                ** Bx = packed: [tid1:5 << 7] | [tid0:5 << 2] | [argc:2]
                */
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int bx = GETARG_Bx(i);
                int argc = bx & 0x03;
                int tid0 = (bx >> 2) & 0x1F;
                int tid1 = (bx >> 7) & 0x1F;
                XrValue val = R(a);
                if (XR_IS_PTR(val) && XR_HEAP_TYPE(val) == XR_TINSTANCE) {
                    XrGCHeader *gc = &((XrInstance*)XR_VALUE_GCPTR(val))->gc;
                    XR_INST_SET_TYPE_ARGS(gc, argc, tid0, tid1);
                }
                vmbreak;
            }

            /* === Struct Native Storage === */
            vmcase(OP_NEW_STRUCT) {
                /* A = dest reg, B = class reg, C = struct_area slot offset
                 * Allocate struct in per-frame struct_area (zero heap allocation).
                 * Layout: [XrClass* 8B][field data...] */
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrValue class_val = R(b);
                XrClass *cls = xr_value_to_class(class_val);
                XrStructLayout *layout = cls->struct_layout;
                XR_DCHECK(layout != NULL, "OP_NEW_STRUCT requires struct_layout");
                XR_DCHECK(vm_ctx->struct_areas && vm_ctx->struct_areas[VM_FRAME_COUNT - 1],
                          "OP_NEW_STRUCT requires allocated struct_area");

                uint8_t *struct_ptr = vm_ctx->struct_areas[VM_FRAME_COUNT - 1] + c * 16;
                *(XrClass**)struct_ptr = cls;
                memset(struct_ptr + 8, 0, layout->total_size);

                // Apply field default values from class descriptor
                if (cls->field_default_values) {
                    int fc = layout->field_count < cls->field_count
                           ? layout->field_count : cls->field_count;
                    for (int fi = 0; fi < fc; fi++) {
                        XrValue dv = cls->field_default_values[fi];
                        if (dv.tag == XR_TAG_NULL) continue;
                        XrStructFieldLayout *fl = &layout->fields[fi];
                        uint8_t *fp = struct_ptr + 8 + fl->offset;
                        switch (fl->native_type) {
                            case XR_NATIVE_I64:  *(int64_t*)fp  = XR_TO_INT(dv); break;
                            case XR_NATIVE_U64:  *(uint64_t*)fp = (uint64_t)XR_TO_INT(dv); break;
                            case XR_NATIVE_F64:  *(double*)fp   = XR_TO_FLOAT(dv); break;
                            case XR_NATIVE_BOOL: *(uint8_t*)fp  = (uint8_t)dv.i; break;
                            case XR_NATIVE_I32:  *(int32_t*)fp  = (int32_t)XR_TO_INT(dv); break;
                            case XR_NATIVE_U32:  *(uint32_t*)fp = (uint32_t)XR_TO_INT(dv); break;
                            case XR_NATIVE_I16:  *(int16_t*)fp  = (int16_t)XR_TO_INT(dv); break;
                            case XR_NATIVE_U16:  *(uint16_t*)fp = (uint16_t)XR_TO_INT(dv); break;
                            case XR_NATIVE_I8:   *(int8_t*)fp   = (int8_t)XR_TO_INT(dv); break;
                            case XR_NATIVE_U8:   *(uint8_t*)fp  = (uint8_t)XR_TO_INT(dv); break;
                            case XR_NATIVE_F32:  *(float*)fp    = (float)XR_TO_FLOAT(dv); break;
                            case XR_NATIVE_STRING: *(XrString**)fp = (XrString*)dv.ptr; break;
                            default: break;
                        }
                    }
                }

                R(a) = xr_struct_ref(struct_ptr, 0);
                vmbreak;
            }

            vmcase(OP_STRUCT_GET) {
                /* R[A] = struct(R[B]).field[C]
                 * Read native field from stack-allocated struct, box to XrValue */
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);

                uint8_t *struct_ptr = (uint8_t*)xr_to_struct_ptr(R(b));
                XrClass *cls = *(XrClass**)struct_ptr;
                XrStructLayout *layout = cls->struct_layout;
                XrStructFieldLayout *field = &layout->fields[c];
                uint8_t *fp = struct_ptr + 8 + field->offset;

                switch (field->native_type) {
                    case XR_NATIVE_I64:  R(a) = XR_FROM_INT(*(int64_t*)fp); break;
                    case XR_NATIVE_U64:  R(a) = XR_FROM_INT((int64_t)*(uint64_t*)fp); break;
                    case XR_NATIVE_F64:  R(a) = XR_FROM_FLOAT(*(double*)fp); break;
                    case XR_NATIVE_BOOL: R(a).descriptor = 0; R(a).i = *(uint8_t*)fp ? 1 : 0; R(a).tag = XR_TAG_BOOL; break;
                    case XR_NATIVE_I32:  R(a) = XR_FROM_INT((int64_t)*(int32_t*)fp); break;
                    case XR_NATIVE_U32:  R(a) = XR_FROM_INT((int64_t)*(uint32_t*)fp); break;
                    case XR_NATIVE_I16:  R(a) = XR_FROM_INT((int64_t)*(int16_t*)fp); break;
                    case XR_NATIVE_U16:  R(a) = XR_FROM_INT((int64_t)*(uint16_t*)fp); break;
                    case XR_NATIVE_I8:   R(a) = XR_FROM_INT((int64_t)*(int8_t*)fp); break;
                    case XR_NATIVE_U8:   R(a) = XR_FROM_INT((int64_t)*(uint8_t*)fp); break;
                    case XR_NATIVE_F32:  R(a) = XR_FROM_FLOAT((double)*(float*)fp); break;
                    case XR_NATIVE_STRING: {
                        XrString *s = *(XrString**)fp;
                        R(a) = s ? XR_FROM_STR(s) : xr_null();
                        break;
                    }
                    case XR_NATIVE_STRUCT: R(a) = xr_struct_ref(fp, field->sub_layout_id); break;
                    case XR_NATIVE_ARRAY:
                        R(a) = xr_array_ref(fp, field->elem_native_type, field->elem_count);
                        break;
                    default: R(a) = xr_null(); break;
                }
                vmbreak;
            }

            vmcase(OP_STRUCT_SET) {
                /* struct(R[A]).field[B] = R[C]
                 * Unbox XrValue and write native field to stack-allocated struct */
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);

                uint8_t *struct_ptr = (uint8_t*)xr_to_struct_ptr(R(a));
                XrClass *cls = *(XrClass**)struct_ptr;
                XrStructLayout *layout = cls->struct_layout;
                XrStructFieldLayout *field = &layout->fields[b];
                uint8_t *fp = struct_ptr + 8 + field->offset;
                XrValue src = R(c);

                switch (field->native_type) {
                    case XR_NATIVE_I64:  *(int64_t*)fp  = XR_TO_INT(src); break;
                    case XR_NATIVE_F64:  *(double*)fp   = XR_TO_FLOAT(src); break;
                    case XR_NATIVE_BOOL: *(uint8_t*)fp  = (uint8_t)src.i; break;
                    case XR_NATIVE_I32:  *(int32_t*)fp  = (int32_t)XR_TO_INT(src); break;
                    case XR_NATIVE_U32:  *(uint32_t*)fp = (uint32_t)XR_TO_INT(src); break;
                    case XR_NATIVE_I16:  *(int16_t*)fp  = (int16_t)XR_TO_INT(src); break;
                    case XR_NATIVE_U16:  *(uint16_t*)fp = (uint16_t)XR_TO_INT(src); break;
                    case XR_NATIVE_I8:   *(int8_t*)fp   = (int8_t)XR_TO_INT(src); break;
                    case XR_NATIVE_U8:   *(uint8_t*)fp  = (uint8_t)XR_TO_INT(src); break;
                    case XR_NATIVE_F32:  *(float*)fp    = (float)XR_TO_FLOAT(src); break;
                    case XR_NATIVE_STRING: {
                        *(XrString**)fp = (XrString*)src.ptr;
                        break;
                    }
                    case XR_NATIVE_STRUCT: {
                        uint8_t *src_ptr = (uint8_t*)xr_to_struct_ptr(src);
                        memcpy(fp, src_ptr, field->size);
                        break;
                    }
                    case XR_NATIVE_ARRAY: {
                        // Copy from heap Array into inline storage
                        if (XR_IS_ARRAY(src)) {
                            XrArray *arr = (XrArray*)src.ptr;
                            int count = arr->length < field->elem_count ? arr->length : field->elem_count;
                            uint8_t es = xr_native_type_size(field->elem_native_type);
                            for (int idx = 0; idx < count; idx++) {
                                XrValue elem = xr_array_get(arr, idx);
                                uint8_t *ep = fp + idx * es;
                                switch (field->elem_native_type) {
                                    case XR_NATIVE_I64:  *(int64_t*)ep  = XR_TO_INT(elem); break;
                                    case XR_NATIVE_F64:  *(double*)ep   = XR_TO_FLOAT(elem); break;
                                    case XR_NATIVE_BOOL: *(uint8_t*)ep  = (uint8_t)elem.i; break;
                                    case XR_NATIVE_I32:  *(int32_t*)ep  = (int32_t)XR_TO_INT(elem); break;
                                    case XR_NATIVE_U32:  *(uint32_t*)ep = (uint32_t)XR_TO_INT(elem); break;
                                    case XR_NATIVE_I16:  *(int16_t*)ep  = (int16_t)XR_TO_INT(elem); break;
                                    case XR_NATIVE_U16:  *(uint16_t*)ep = (uint16_t)XR_TO_INT(elem); break;
                                    case XR_NATIVE_I8:   *(int8_t*)ep   = (int8_t)XR_TO_INT(elem); break;
                                    case XR_NATIVE_U8:   *(uint8_t*)ep  = (uint8_t)XR_TO_INT(elem); break;
                                    case XR_NATIVE_F32:  *(float*)ep    = (float)XR_TO_FLOAT(elem); break;
                                    default: break;
                                }
                            }
                            // Zero remaining elements if array shorter than field
                            if (count < field->elem_count) {
                                memset(fp + count * es, 0, (field->elem_count - count) * es);
                            }
                        } else if (XR_IS_ARRAY_REF(src)) {
                            // Copy from another struct's array_ref
                            memcpy(fp, src.ptr, field->size);
                        }
                        break;
                    }
                    default: break;
                }
                vmbreak;
            }

            vmcase(OP_STRUCT_COPY) {
                /* R[A] = deep copy of struct R[B], placed at struct_area slot C
                 * memcpy entire struct (class ptr + field data) */
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);

                uint8_t *src_ptr = (uint8_t*)xr_to_struct_ptr(R(b));
                XrClass *cls = *(XrClass**)src_ptr;
                XrStructLayout *layout = cls->struct_layout;

                uint8_t *dst_ptr = vm_ctx->struct_areas[VM_FRAME_COUNT - 1] + c * 16;

                memcpy(dst_ptr, src_ptr, 8 + layout->total_size);
                R(a) = xr_struct_ref(dst_ptr, 0);
                vmbreak;
            }

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
