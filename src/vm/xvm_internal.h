/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_internal.h - Internal shared declarations for VM modules
 */

#ifndef XVM_INTERNAL_H
#define XVM_INTERNAL_H

#include "xvm.h"
#include "../runtime/gc/xgc_header.h"
#include <stdatomic.h>
#include "../runtime/class/xclass.h"
#include "../runtime/class/xmethod.h"
#include "xic_method.h"
#include "xic_field_table.h"
#include "xic_builtin.h"
#include "../runtime/value/xvalue_print.h"
#include "../runtime/value/xvalue_format.h"
#include "../base/xmalloc.h"
#include "../runtime/object/xstring.h"
#include "../runtime/object/xarray.h"
#include "../runtime/object/xmap.h"
#include "../runtime/object/xset.h"
#include "../runtime/object/xjson.h"
#include "../runtime/class/xinstance.h"
#include "../runtime/xisolate_internal.h"
#include "../runtime/object/xiterator.h"
#include "../runtime/class/xclass_system.h"
#include "../runtime/class/xenum.h"
#include "../runtime/object/xexception.h"
#include "../runtime/object/xbigint.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../base/xglobal_indices.h"
#include "../coro/xcoroutine.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include "../base/xdefs.h"

/* ========== Inline Helper Functions ========== */

/*
 * Falsy values in xray:
 *   - null
 *   - false
 *   - 0 (integer)
 *   - 0.0 (float)
 *   - "" (empty string)
 *   - [] (empty array)
 *   - #{} (empty map)
 *   - #[] (empty set)
 */
static inline bool vm_is_falsey(XrValue value) {
    // null is falsy
    if (XR_IS_NULL(value)) return true;

    // bool: payload 0=false (falsy), 1=true (truthy)
    if (XR_IS_BOOL(value)) return value.i == 0;

    // 0 is falsy
    if (XR_IS_INT(value)) return XR_TO_INT(value) == 0;

    // 0.0 is falsy
    if (XR_IS_FLOAT(value)) return XR_TO_FLOAT(value) == 0.0;

    // Empty string is falsy
    if (XR_IS_STRING(value)) {
        XrString *str = XR_TO_STRING(value);
        return str->length == 0;
    }

    // Empty array is falsy
    if (XR_IS_ARRAY(value)) {
        XrArray *arr = XR_TO_ARRAY(value);
        return arr->length == 0;
    }

    // Empty map is falsy
    if (XR_IS_MAP(value)) {
        XrMap *map = XR_TO_MAP(value);
        return map->count == 0;
    }

    // Empty set is falsy
    if (XR_IS_SET(value)) {
        XrSet *set = XR_TO_SET(value);
        return set->count == 0;
    }

    // Everything else is truthy
    return false;
}

static inline bool vm_is_truthy(XrValue value) {
    return !vm_is_falsey(value);
}

/* ========== Register Limits ========== */
#ifndef MAXREGS
#define MAXREGS 250
#endif // ========== Debug Macros ==========

// #define XR_DEBUG_VM

#ifdef XR_DEBUG_VM
    #define VM_DEBUG_PRINT(...) printf("[VM DEBUG] " __VA_ARGS__)
#else
    #define VM_DEBUG_PRINT(...) ((void)0)
#endif // ========== Register Bounds Checking (Debug Mode) ==========

/*
** XR_DEBUG_REGS - Enable register bounds checking
**
** Enable in debug builds to catch register overflow issues early:
**   - New frame base must be outside current frame
**   - Register access must be within current frame bounds
**
** Enable via: cmake -DXR_DEBUG_REGS=1 or uncomment below
*/
// #define XR_DEBUG_REGS

#ifdef XR_DEBUG_REGS

// Check new frame base doesn't overlap current frame
#define CHECK_FRAME_BOUNDARY(isolate, ci, new_base) do { \
    if ((ci) && (ci)->closure && (ci)->closure->proto) { \
        XrValue *_frame_end = (ci)->base + (ci)->closure->proto->maxstacksize; \
        if ((new_base) < _frame_end) { \
            fprintf(stderr, "\n[REG ERROR] Frame overlap detected!\n"); \
            fprintf(stderr, "  Current frame: base=%p, end=%p, maxstacksize=%d\n", \
                    (void*)(ci)->base, (void*)_frame_end, \
                    (ci)->closure->proto->maxstacksize); \
            fprintf(stderr, "  New frame base: %p (overlap by %ld slots)\n", \
                    (void*)(new_base), (long)(_frame_end - (new_base))); \
            if ((ci)->closure->proto->name) { \
                fprintf(stderr, "  Current function: %s\n", \
                        XR_STRING_CHARS((ci)->closure->proto->name)); \
            } \
            XR_CHECK(0, "New frame overlaps current frame"); \
        } \
    } \
} while(0)

// Check register index is within current frame bounds
#define CHECK_REG_INDEX(ci, reg_idx) do { \
    if ((ci) && (ci)->closure && (ci)->closure->proto) { \
        int _maxstack = (ci)->closure->proto->maxstacksize; \
        if ((reg_idx) < 0 || (reg_idx) >= _maxstack) { \
            fprintf(stderr, "\n[REG ERROR] Register index out of bounds!\n"); \
            fprintf(stderr, "  Register: R[%d], maxstacksize: %d\n", \
                    (reg_idx), _maxstack); \
            if ((ci)->closure->proto->name) { \
                fprintf(stderr, "  Function: %s\n", \
                        (ci)->closure->proto->name->chars); \
            } \
            XR_CHECK(0, "Register index out of frame bounds"); \
        } \
    } \
} while(0)

// Check call stack won't overflow
#define CHECK_FRAME_OVERFLOW(isolate) do { \
    if ((isolate)->vm.frame_count >= XR_FRAMES_MAX) { \
        fprintf(stderr, "\n[REG ERROR] Frame stack overflow!\n"); \
        fprintf(stderr, "  frame_count: %d, XR_FRAMES_MAX: %d\n", \
                (isolate)->vm.frame_count, XR_FRAMES_MAX); \
        XR_CHECK(0, "Frame stack overflow"); \
    } \
} while(0)

#else
// Release mode: these checks are no-ops with zero overhead
#define CHECK_FRAME_BOUNDARY(isolate, ci, new_base) ((void)(isolate), (void)(ci), (void)(new_base))
#define CHECK_REG_INDEX(ci, reg_idx) ((void)(ci), (void)(reg_idx))
#define CHECK_FRAME_OVERFLOW(isolate) ((void)(isolate))
#endif // ========== Object Types ==========

#define OBJ_CLOSURE 1

// VM constants now in core/xconstants.h (included via xvm.h)


/* Builtin method dispatch & bound method definitions:
 * see runtime/closure/xbound_method.h (included below via the runtime closure layer).
 */
#include "../runtime/closure/xbound_method.h"
/* Symbol -> MethodHandler bridges (xr_*_get_handler,
 * xr_enum_get_member_handler) are declared in xbound_method.h above
 * and implemented under runtime/closure. */

/* ========== Helper Functions (in xvm_helpers.c) ========== */
XR_FUNC void xr_runtime_error(XrayIsolate *isolate, const char *format, ...);

// Debug info: find local variable name by register and PC
XR_FUNC const char* xr_vm_get_local_name(XrProto *proto, int reg, int pc);

// C function operations
XR_FUNC void xr_vm_cfunction_free(XrCFunction *cfunc);

// Closure operations: see runtime/closure/xclosure.h

// VM initialization and cleanup
XR_FUNC void xr_vm_vm_init(XrayIsolate *isolate);
XR_FUNC void xr_vm_vm_free(XrayIsolate *isolate);

// Value operation helpers
XR_FUNC bool xr_vm_is_truthy(XrValue value);

/* ========== API Functions (in xvm_api.c) ========== */

/*
** Single authoritative VM context resolver.
**
** Resolution order:
**   1. Current worker's M.vm_ctx.current_coro -> coro->vm_ctx
**   2. Current worker's M.vm_ctx (no active coro on this M)
**   3. isolate->main_coro->vm_ctx (fallback during bootstrap / non-worker thread)
**   4. &isolate->vm_ctx (static fallback)
**
** This is the ONLY supported way to obtain the live execution context inside
** the VM module. Internal helpers must accept ctx as a parameter; they MUST
** NOT re-resolve via isolate->vm.* fields.
*/
XR_FUNC XrVMContext* xr_vm_current_ctx(XrayIsolate *isolate);

/*
** Ensure the context can host a new entry frame with the given prototype.
**
** Reserves stack capacity for `extra_stack` slots above current stack_top
** (typically proto->maxstacksize) and frame capacity for one additional
** call frame. Grows backing storage when ctx is coroutine-backed; returns
** false on growth failure or when ctx is the static fallback that cannot
** grow. Public entries (xr_vm_call_closure / xr_vm_interpret_proto /
** xr_vm_execute_module) must call this before installing a frame.
**
** Maintains the invariant stack <= stack_top <= stack + capacity across
** the call (re-derives stack_top via offset since xr_coro_grow_stack may
** relocate the backing buffer).
*/
XR_FUNC bool xr_vm_prepare_entry(XrVMContext *ctx, int extra_stack);

// Call closure from C code (coroutine-aware, unified implementation)
XR_FUNC XrValue xr_vm_call_closure(XrayIsolate *isolate, XrClosure *closure, XrValue *args, int nargs);

// VM execution
XR_FUNC XrVMResult xr_vm_interpret_proto(XrayIsolate *isolate, XrProto *proto);
XR_FUNC XrVMResult xr_vm_interpret(const char *source);
XR_FUNC XrVMResult xr_vm_interpret_proto_isolate(XrayIsolate *isolate, XrProto *proto);

// Exception handling
XR_FUNC void xr_vm_add_stacktrace(XrayIsolate *isolate, XrValue exception);
XR_FUNC void xr_vm_throw_exception(XrayIsolate *isolate, XrValue exception);

/*
 * Single-call throw helper: records the full call chain into
 * exc.stackTrace then performs the unwind. New code should prefer
 * this over the add_stacktrace + throw_exception pair so the
 * trace mechanism stays in one place. Defined in xvm_exception.c.
 */
XR_FUNC void xr_vm_unwind_with_trace(XrayIsolate *isolate, XrValue exception);

/* ========== Per-coroutine Inline Caches (in xvm_ic.c) ========== */

/*
** Lazily allocate a per-(ctx, proto) field IC table sized to the proto's
** instruction count. Returns NULL on OOM. All slots are pre-allocated so
** cache_index = pc - PROTO_CODE_BASE remains a valid lookup index.
*/
XR_FUNC struct XrICFieldTable *xr_vm_ctx_ensure_ic_fields(XrVMContext *ctx,
                                                          XrProto *proto);

/*
** Lazily allocate a per-(ctx, proto) method IC table. Slots are zeroed
** by xr_ic_method_table_new and the count is set to PROTO_CODE_COUNT.
*/
XR_FUNC struct XrICMethodTable *xr_vm_ctx_ensure_ic_methods(XrVMContext *ctx,
                                                            XrProto *proto);

/*
** Lazily allocate a per-(ctx, proto) builtin-invoke IC table. Slots are
** zeroed and pre-allocated up to PROTO_CODE_COUNT so that
** cache_index = pc - PROTO_CODE_BASE is always a valid lookup index.
*/
XR_FUNC struct XrICBuiltinTable *xr_vm_ctx_ensure_ic_builtin(XrVMContext *ctx,
                                                             XrProto *proto);

/*
** Read-only IC table accessors. Return NULL when no IC has been recorded
** for this proto in this ctx. All three are safe to call before the IC
** table has been lazily allocated.
*/
XR_FUNC struct XrICFieldTable *xr_vm_ctx_get_ic_fields(const XrVMContext *ctx,
                                                       const XrProto *proto);
XR_FUNC struct XrICMethodTable *xr_vm_ctx_get_ic_methods(const XrVMContext *ctx,
                                                         const XrProto *proto);
XR_FUNC struct XrICBuiltinTable *xr_vm_ctx_get_ic_builtin(const XrVMContext *ctx,
                                                          const XrProto *proto);

/*
** Deep-copy snapshot of the current IC state for `proto` in `ctx`. The
** returned table is independently owned by the caller; concurrent ctx
** mutation cannot tear the snapshot. Caller must release via
** xr_ic_field_table_free / xr_ic_method_table_free /
** xr_ic_builtin_table_free. Returns NULL when no IC has been recorded.
*/
XR_FUNC struct XrICFieldTable *xr_vm_ic_fields_snapshot(XrVMContext *ctx,
                                                        XrProto *proto);
XR_FUNC struct XrICMethodTable *xr_vm_ic_methods_snapshot(XrVMContext *ctx,
                                                          XrProto *proto);
XR_FUNC struct XrICBuiltinTable *xr_vm_ic_builtin_snapshot(XrVMContext *ctx,
                                                           XrProto *proto);

/*
** Free every IC table currently held by `ctx` and reset capacity. Called
** on coroutine teardown and isolate cleanup.
*/
XR_FUNC void xr_vm_ctx_free_ic_tables(XrVMContext *ctx);

/* ========== Instruction Operation Helpers (in xvm_ops.c) ========== */

/*
** Frame-creation helpers (vm_push_frame, vm_create_method_frame,
** vm_create_function_frame, vm_get_safe_stack_start) were removed.
**
** They read/wrote isolate->vm.* (the legacy embedded XrVMState), but live
** execution state belongs to XrVMContext (per-coroutine). Frame creation
** in the dispatch loop is performed inline via VM_FRAMES / VM_FRAME_COUNT
** macros that operate on vm_ctx. Public C-callers must use
** xr_vm_call_closure / xr_vm_interpret_proto / xr_vm_execute_module.
*/

// Type conversion: xr_value_to_string declared in runtime/value/xvalue_format.h.

// Arithmetic operations
XR_FUNC XrValue vm_add_operation(XrayIsolate *isolate, XrValue left, XrValue right);
XR_FUNC XrValue vm_numeric_sub(XrValue left, XrValue right);
XR_FUNC XrValue vm_numeric_mul(XrValue left, XrValue right);
XR_FUNC XrValue vm_numeric_div(XrayIsolate *isolate, XrValue left, XrValue right);
XR_FUNC XrValue vm_numeric_mod(XrayIsolate *isolate, XrValue left, XrValue right);

/* ========== BigInt Mixed Operations Helper ========== */

// Check if either value is BigInt and the other is int/BigInt
static inline bool vm_is_bigint_mixed(XrValue left, XrValue right) {
    bool left_bigint = XR_IS_BIGINT(left);
    bool right_bigint = XR_IS_BIGINT(right);
    bool left_int = XR_IS_INT(left);
    bool right_int = XR_IS_INT(right);
    // At least one BigInt, and the other is int or BigInt
    return (left_bigint && (right_bigint || right_int)) ||
           (right_bigint && (left_bigint || left_int));
}

// Comparison operations
XR_FUNC bool vm_values_equal(XrValue a, XrValue b);
XR_FUNC bool vm_values_equal_deep(XrayIsolate *isolate, XrValue a, XrValue b);
XR_FUNC bool vm_numeric_less(XrValue left, XrValue right);
XR_FUNC bool vm_numeric_less_equal(XrValue left, XrValue right);
XR_FUNC bool vm_numeric_greater(XrValue left, XrValue right);
XR_FUNC bool vm_numeric_greater_equal(XrValue left, XrValue right);

/* ========== VM Execution Loop (in xvm.c) ========== */
XR_FUNC XrVMResult run(XrayIsolate *isolate, XrVMContext *vm_ctx);

/* ========== VM Coroutine ========== */
// Type definitions moved to runtime/coroutine/xcoroutine.h

// VM Coroutine API
XR_FUNC XrCoroutine *xr_coro_create(XrayIsolate *X, XrClosure *closure,
                            XrValue *args, int arg_count,
                            const char *name, const char *file, int line);

XR_FUNC void xr_coro_free(XrCoroutine *coro);
XR_FUNC void xr_coro_release_heap(XrCoroutine *coro);
XR_FUNC void xr_coro_release_resources(XrCoroutine *coro);
XR_FUNC void xr_coro_spawn(XrayIsolate *X, XrCoroutine *coro);
XR_FUNC void xr_sched_enqueue(XrCoroState *sched, XrCoroutine *coro);
XR_FUNC void xr_sched_remove(XrCoroState *sched, XrCoroutine *target);
XR_FUNC XrCoroutine *xr_sched_dequeue(XrCoroState *sched);
// Scheduler initialization
XR_FUNC void xr_sched_init(XrCoroState *sched);
XR_FUNC void xr_sched_destroy(XrCoroState *sched);

// Multicore runtime
XR_FUNC void xr_multicore_init(XrayIsolate *X, int num_workers);
XR_FUNC void xr_multicore_destroy(XrayIsolate *X);

// Wake mechanism
XR_FUNC void xr_coro_ready(XrayIsolate *X, XrCoroutine *gp, bool next);
XR_FUNC XrCoroutine *xr_current_coro(XrayIsolate *X);
XR_FUNC void xr_coro_wake_waiter(XrayIsolate *X, XrCoroutine *coro);


// Multicore runtime channel wake (auto fallback to single-thread mode)
XR_FUNC XrCoroutine *xr_runtime_wake_channel(XrayIsolate *X, void *channel, bool wake_sender);
XR_FUNC void xr_runtime_wake_channel_all(XrayIsolate *X, void *channel);


// Coroutine control
XR_FUNC void xr_coro_cancel(XrCoroutine *coro);

// Scope structured concurrency
XR_FUNC void xr_scope_add_coro(XrCoroState *sched, XrCoroutine *coro, XrCoroutine *parent);

#endif // XVM_INTERNAL_H
