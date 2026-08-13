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
#include "../runtime/mem/xobj_header.h"
#include <stdatomic.h>
#include "../runtime/class/xclass.h"
#include "../runtime/class/xmethod.h"
#include "xic_method.h"
#include "xic_field_table.h"
#include "../runtime/value/xvalue_print.h"
#include "../runtime/value/xvalue_format.h"
#include "../base/xmalloc.h"
#include "../runtime/object/xstring.h"
#include "../runtime/object/xarray.h"
#include "../runtime/object/xmap.h"
#include "../runtime/object/xtuple.h"
#include "../runtime/object/xset.h"
#include "../runtime/object/xjson.h"
#include "../runtime/class/xinstance.h"
#include "../runtime/xisolate_internal.h"
#include "../runtime/object/xiterator.h"
#include "../runtime/class/xclass_system.h"
#include "../runtime/class/xenum.h"
#include "../runtime/object/xpanic_info.h"
#include "../runtime/object/xbigint.h"
#include "../runtime/value/xstruct_layout.h"
#include "../runtime/value/xenum_descriptor.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../shared/xr_assert_condition_core.h"
#include "../shared/xr_truthy_core.h"
#include "../shared/xr_type_identity_core.h"
#include "../base/xglobal_indices.h"
#include "xvm_coro_api.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include "../base/xdefs.h"

XR_FUNC XrAggregateLayout *xr_vm_struct_ref_layout(XrVMRuntime *isolate, XrValue ref);
XR_FUNC uint8_t *xr_vm_struct_ref_payload(XrVMRuntime *isolate, XrValue ref,
                                          XrAggregateLayout **layout_out);
XR_FUNC int xr_struct_layout_field_index(XrVMRuntime *isolate, const XrAggregateLayout *layout,
                                         int prop_symbol);
XR_FUNC bool xr_struct_read_field_value(XrVMRuntime *isolate, uint8_t *fp,
                                        XrAggregateFieldLayout *field, XrValue *out);
XR_FUNC bool xr_struct_write_field_value(XrVMRuntime *isolate, uint8_t *fp,
                                         const XrAggregateFieldLayout *field, XrValue src);
/* Cross the frame-local aggregate boundary by creating a stable nominal value
 * instance. Returns null when the aggregate is anonymous or allocation fails. */
XR_FUNC XrValue xr_vm_struct_materialize_instance(XrVMRuntime *isolate, XrValue ref);
XR_FUNC bool xr_instance_struct_get_field(XrVMRuntime *isolate, XrInstance *inst,
                                          int field_index, XrValue *out);
XR_FUNC bool xr_instance_struct_set_field(XrVMRuntime *isolate, XrInstance *inst,
                                          int field_index, XrValue value);
XR_FUNC XrValue xr_vm_send_result_value(XrVMRuntime *isolate, uint32_t member_index);

/* ========== Inline Helper Functions ========== */

/*
 * Falsy values in control-flow / truthiness helpers:
 *   - null
 *   - false
 *   - 0 (integer)
 *   - 0.0 (float)
 *
 * Empty strings, collections, and other heap objects are not implicitly falsy.
 * User conditions must be bool or nullable presence (T?) checked via ISNULL.
 */
static inline bool vm_is_truthy(XrValue value) {
    XrTruthyCoreKind kind = XR_TRUTHY_CORE_OBJECT;
    int64_t integer = 0;
    double floating = 0.0;
    if (XR_IS_NULL(value)) {
        kind = XR_TRUTHY_CORE_NULL;
    } else if (XR_IS_BOOL(value)) {
        kind = XR_TRUTHY_CORE_BOOL;
        integer = XR_TO_BOOL(value);
    } else if (XR_IS_INT(value)) {
        kind = XR_TRUTHY_CORE_INT;
        integer = XR_TO_INT(value);
    } else if (XR_IS_FLOAT(value)) {
        kind = XR_TRUTHY_CORE_FLOAT;
        floating = XR_TO_FLOAT(value);
    }
    return xr_truthy_core_eval(XR_SEM_OWNER_ID_SHARED_TRUTHINESS_HI,
                               XR_SEM_OWNER_ID_SHARED_TRUTHINESS_LO, XR_SEM_CONSUMER_VM, kind,
                               integer, floating, 0);
}

static inline bool vm_is_falsey(XrValue value) {
    return !vm_is_truthy(value);
}

/* ========== Debug Macros ========== */
// #define XR_DEBUG_VM

#ifdef XR_DEBUG_VM
#define VM_DEBUG_PRINT(...) printf("[VM DEBUG] " __VA_ARGS__)
#else
#define VM_DEBUG_PRINT(...) ((void) 0)
#endif

/* ========== Register Bounds Checking (Debug Mode) ========== */

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
#define CHECK_FRAME_BOUNDARY(isolate, ci, new_base)                                                \
    do {                                                                                           \
        if ((ci) && (ci)->closure && (ci)->closure->proto) {                                       \
            XrValue *_frame_end = (ci)->base + (ci)->closure->proto->maxstacksize;                 \
            if ((new_base) < _frame_end) {                                                         \
                fprintf(stderr, "\n[REG ERROR] Frame overlap detected!\n");                        \
                fprintf(stderr, "  Current frame: base=%p, end=%p, maxstacksize=%d\n",             \
                        (void *) (ci)->base, (void *) _frame_end,                                  \
                        (ci)->closure->proto->maxstacksize);                                       \
                fprintf(stderr, "  New frame base: %p (overlap by %ld slots)\n",                   \
                        (void *) (new_base), (long) (_frame_end - (new_base)));                    \
                if ((ci)->closure->proto->name) {                                                  \
                    fprintf(stderr, "  Current function: %s\n",                                    \
                            XR_STRING_CHARS((ci)->closure->proto->name));                          \
                }                                                                                  \
                XR_CHECK(0, "New frame overlaps current frame");                                   \
            }                                                                                      \
        }                                                                                          \
    } while (0)

// Check register index is within current frame bounds
#define CHECK_REG_INDEX(ci, reg_idx)                                                               \
    do {                                                                                           \
        if ((ci) && (ci)->closure && (ci)->closure->proto) {                                       \
            int _maxstack = (ci)->closure->proto->maxstacksize;                                    \
            if ((reg_idx) < 0 || (reg_idx) >= _maxstack) {                                         \
                fprintf(stderr, "\n[REG ERROR] Register index out of bounds!\n");                  \
                fprintf(stderr, "  Register: R[%d], maxstacksize: %d\n", (reg_idx), _maxstack);    \
                if ((ci)->closure->proto->name) {                                                  \
                    fprintf(stderr, "  Function: %s\n", (ci)->closure->proto->name->chars);        \
                }                                                                                  \
                XR_CHECK(0, "Register index out of frame bounds");                                 \
            }                                                                                      \
        }                                                                                          \
    } while (0)

// Check call stack won't overflow
#define CHECK_FRAME_OVERFLOW(isolate)                                                              \
    do {                                                                                           \
        if ((isolate)->vm.frame_count >= XR_FRAMES_MAX) {                                          \
            fprintf(stderr, "\n[REG ERROR] Frame stack overflow!\n");                              \
            fprintf(stderr, "  frame_count: %d, XR_FRAMES_MAX: %d\n", (isolate)->vm.frame_count,   \
                    XR_FRAMES_MAX);                                                                \
            XR_CHECK(0, "Frame stack overflow");                                                   \
        }                                                                                          \
    } while (0)

#else
// Release mode: these checks are no-ops with zero overhead
#define CHECK_FRAME_BOUNDARY(isolate, ci, new_base)                                                \
    ((void) (isolate), (void) (ci), (void) (new_base))
#define CHECK_REG_INDEX(ci, reg_idx) ((void) (ci), (void) (reg_idx))
#define CHECK_FRAME_OVERFLOW(isolate) ((void) (isolate))
#endif  // ========== Object Types ==========

#define OBJ_CLOSURE 1

// VM constants now in core/xconstants.h (included via xvm.h)

/* Builtin method dispatch & bound method definitions:
 * see runtime/closure/xbound_method.h (included below via the runtime closure layer).
 */
#include "../runtime/closure/xbound_method.h"
/* Symbol -> MethodHandler bridges (xr_*_get_handler,
 * primitive native bound-method handlers are declared in xbound_method.h above
 * and implemented under runtime/closure. */

/* ========== Helper Functions (in xvm_helpers.c) ========== */
XR_FUNC void xr_runtime_error(XrVMRuntime *isolate, const char *format, ...);

// Debug info: find local variable name by register and PC
XR_FUNC const char *xr_debug_local_name(XrProto *proto, int reg, int pc);

// Closure operations: see runtime/closure/xclosure.h

// Value operation helpers
XR_FUNC bool xr_vm_is_truthy(XrValue value);

/* ========== API Functions (in xvm_api.c) ========== */

/*
** Single authoritative VM context resolver.
**
** Resolution order:
**   1. Current worker's current coroutine VM backend context
**   2. Current worker's VM backend scratch context
**   3. &isolate->vm_ctx (logical-root/direct-entry context)
**
** This is the ONLY supported way to obtain the live execution context inside
** the VM module. Internal helpers must accept ctx as a parameter; they MUST
** NOT re-resolve via isolate->vm.* fields.
*/
XR_FUNC XrVMContext *xr_vm_current_ctx(XrVMRuntime *isolate);

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
XR_FUNC XrValue xr_vm_call_closure(XrVMRuntime *isolate, XrClosure *closure, XrValue *args,
                                   int nargs);

// Install the Map/Set instance hash/eq hooks (xvm_value_hooks.c) so a user
// Hashable class keys by value. Idempotent; called from isolate init.
XR_FUNC void xr_value_install_instance_hooks(void);

// VM execution
XR_FUNC XrVMResult xr_vm_interpret_proto(XrVMRuntime *isolate, XrProto *proto);
XR_FUNC bool xr_vm_bind_proto_shared_slots(XrVMRuntime *isolate, XrProto *proto);

// Exception handling
XR_FUNC void xr_vm_throw_exception(XrVMRuntime *isolate, XrValue exception);
XR_FUNC void xr_cleanup_scope_enter(XrVMRuntime *isolate, XrVMContext *ctx);
XR_FUNC void xr_cleanup_scope_leave(XrVMRuntime *isolate, XrVMContext *ctx);
XR_FUNC void xr_cleanup_scope_check_error(XrVMRuntime *isolate, XrVMContext *ctx);
/*
 * Uncaught value-return error diagnostic (spec §8.1.1). Shared by every
 * top-level finalization path so the wording stays in one place; honours
 * suppress_exception_print and ignores a null error. Defined in xvm_api.c.
 */
XR_FUNC void xr_vm_report_uncaught_error(XrVMRuntime *isolate, XrValue error, bool in_go_coroutine);

/*
 * Single-call throw helper: records the full call chain into
 * exc.stackTrace then performs the unwind. New code should prefer
 * this over the add_stacktrace + throw_exception pair so the
 * trace mechanism stays in one place. Defined in xvm_exception.c.
 */
XR_FUNC void xr_vm_unwind_with_trace(XrVMRuntime *isolate, XrValue exception);

/* ========== Per-coroutine Inline Caches (in xvm_ic.c) ========== */

/*
** Lazily allocate a per-(ctx, proto) field IC table sized to the proto's
** instruction count. Returns NULL on OOM. All slots are pre-allocated so
** cache_index = pc - PROTO_CODE_BASE remains a valid lookup index.
*/
XR_FUNC struct XrICFieldTable *xr_vm_ctx_ensure_ic_fields(XrVMContext *ctx, XrProto *proto);

/*
** Lazily allocate a per-(ctx, proto) method IC table. Slots are zeroed
** by xr_ic_method_table_new and the count is set to PROTO_CODE_COUNT.
*/
XR_FUNC struct XrICMethodTable *xr_vm_ctx_ensure_ic_methods(XrVMContext *ctx, XrProto *proto);

/*
** Read-only IC table accessors. Return NULL when no IC has been recorded
** for this proto in this ctx. All three are safe to call before the IC
** table has been lazily allocated.
*/
XR_FUNC struct XrICFieldTable *xr_vm_ctx_get_ic_fields(const XrVMContext *ctx,
                                                       const XrProto *proto);
XR_FUNC struct XrICMethodTable *xr_vm_ctx_get_ic_methods(const XrVMContext *ctx,
                                                         const XrProto *proto);

/*
** Deep-copy snapshot of the current IC state for `proto` in `ctx`. The
** returned table is independently owned by the caller; concurrent ctx
** mutation cannot tear the snapshot. Caller must release via
** xr_ic_field_table_free / xr_ic_method_table_free.
** Returns NULL when no IC has been recorded.
*/
XR_FUNC struct XrICFieldTable *xr_vm_ic_fields_snapshot(XrVMContext *ctx, XrProto *proto);
XR_FUNC struct XrICMethodTable *xr_vm_ic_methods_snapshot(XrVMContext *ctx, XrProto *proto);

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
** execution state belongs to the active XrVMContext. Frame creation in the
** dispatch loop is performed inline via VM_FRAMES / VM_FRAME_COUNT macros
** that operate on vm_ctx. Public C-callers must use
** xr_vm_call_closure / xr_vm_interpret_proto / xr_vm_execute_module.
*/

// Type conversion: xr_value_to_string declared in runtime/value/xvalue_format.h.

// Arithmetic operations
XR_FUNC XrValue vm_add_operation(XrVMRuntime *isolate, XrValue left, XrValue right);
XR_FUNC XrValue vm_numeric_sub(XrValue left, XrValue right);
XR_FUNC XrValue vm_numeric_mul(XrValue left, XrValue right);
XR_FUNC XrValue vm_numeric_div(XrVMRuntime *isolate, XrValue left, XrValue right);
XR_FUNC XrValue vm_numeric_mod(XrVMRuntime *isolate, XrValue left, XrValue right);

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
XR_FUNC bool vm_values_equal_deep(XrVMRuntime *isolate, XrValue a, XrValue b);
XR_FUNC bool vm_numeric_less(XrValue left, XrValue right);
XR_FUNC bool vm_numeric_less_equal(XrValue left, XrValue right);
XR_FUNC bool vm_numeric_greater(XrValue left, XrValue right);
XR_FUNC bool vm_numeric_greater_equal(XrValue left, XrValue right);

/* ========== VM Execution Loop (in xvm.c) ========== */
XR_FUNC XrVMResult run(XrVMRuntime *isolate, XrVMContext *vm_ctx);

/* In-dispatch direct coroutine switch (in xvm_coro_backend.c).
 * Called from the dispatch loop when a coroutine blocks on a channel/await
 * op (XR_DISP_SWITCH). Returns the VM context of an admissible just-woken
 * LIFO partner — the caller swaps vm_ctx and jumps to startfunc — or NULL,
 * in which case the caller must return XR_VM_BLOCKED (ordinary slow path).
 * On success this owns ALL worker bookkeeping for both coroutines. */
XR_FUNC XrVMContext *xr_vm_try_direct_switch(XrVMRuntime *isolate, XrVMContext *cur_ctx);

/* ========== Coroutine Scheduler ========== */
XR_FUNC void xr_coro_free(XrCoroutine *coro);
XR_FUNC void xr_coro_spawn(XrVMRuntime *X, XrCoroutine *coro);
// Coroutine bookkeeping initialization
XR_FUNC void xr_coro_state_init(XrCoroState *state);
XR_FUNC void xr_coro_state_destroy(XrCoroState *state);

// Multicore runtime
XR_FUNC void xr_multicore_init(XrVMRuntime *X, int num_workers);

// Wake mechanism
XR_FUNC void xr_coro_ready(XrVMRuntime *X, XrCoroutine *gp, bool next);
XR_FUNC XrCoroutine *xr_current_coro(XrVMRuntime *X);
XR_FUNC void xr_coro_wake_waiter_runtime(XrRuntime *runtime, XrCoroutine *coro);
XR_FUNC void xr_coro_wake_waiter(XrVMRuntime *X, XrCoroutine *coro);

// Multicore runtime channel wake (auto fallback to single-thread mode)
XR_FUNC XrCoroutine *xr_runtime_wake_channel(XrRuntime *runtime, void *channel, bool wake_sender);
XR_FUNC void xr_runtime_wake_channel_all(XrRuntime *runtime, void *channel);

// Coroutine control
XR_FUNC void xr_coro_cancel(XrCoroutine *coro);

// Scope structured concurrency
XR_FUNC void xr_scope_add_coro(XrCoroState *sched, XrCoroutine *coro, XrCoroutine *parent);

#endif  // XVM_INTERNAL_H
