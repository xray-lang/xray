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
#include "../runtime/value/xvalue_print.h"
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


/* ========== Builtin Method Dispatch ========== */
typedef XrValue (*MethodHandler)(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc);

/* ========== Bound Method ========== */
typedef struct XrBoundMethod {
    XrGCHeader gc;
    XrValue receiver;
    MethodHandler handler;  // direct function pointer, zero-dispatch call
} XrBoundMethod;
XR_FUNC XrBoundMethod* xr_bound_method_new(XrayIsolate *isolate, XrValue receiver, MethodHandler handler);
XR_FUNC XrValue xr_value_from_bound_method(XrBoundMethod *bm);
XR_FUNC XrBoundMethod* xr_value_to_bound_method(XrValue v);
XR_FUNC bool xr_value_is_bound_method(XrValue v);

// BoundMethod handler lookup (implemented in xvm_builtins.c)
XR_FUNC MethodHandler xr_map_get_handler(int symbol);
XR_FUNC MethodHandler xr_array_get_handler(int symbol);
XR_FUNC MethodHandler xr_set_get_handler(int symbol);
XR_FUNC MethodHandler xr_string_get_handler(int symbol);
XR_FUNC MethodHandler xr_iterator_get_handler(int symbol);
XR_FUNC XrValue xr_enum_get_member_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc);

// Method call dispatch functions (implemented in xvm_builtins.c)
XR_FUNC XrValue map_method_call_by_symbol(XrayIsolate *isolate, XrMap *map, int symbol, XrValue *args, int argc);
XR_FUNC XrValue json_method_call_by_symbol(XrayIsolate *isolate, XrJson *json, int symbol, XrValue *args, int argc);
XR_FUNC XrValue string_method_call_by_symbol(XrayIsolate *isolate, XrString *str, int symbol, XrValue *args, int argc);
XR_FUNC XrValue array_method_call_by_symbol(XrayIsolate *isolate, XrArray *array, int symbol, XrValue *args, int argc);
XR_FUNC XrValue set_method_call_by_symbol(XrayIsolate *isolate, XrSet *set, int symbol, XrValue *args, int argc);
XR_FUNC XrValue float_method_call_by_symbol(XrayIsolate *isolate, xr_Number value, int symbol, XrValue *args, int argc);
XR_FUNC XrValue datetime_method_call_by_symbol(XrayIsolate *isolate, void *dt, int symbol, XrValue *args, int argc);
XR_FUNC XrValue int_method_call_by_symbol(XrayIsolate *isolate, xr_Integer value, int symbol, XrValue *args, int argc);
XR_FUNC XrValue bool_method_call_by_symbol(XrayIsolate *isolate, bool value, int symbol);
XR_FUNC XrValue bigint_method_call_by_symbol(XrayIsolate *isolate, XrBigInt *bigint, int symbol, XrValue *args, int argc);

/* ========== Helper Functions (in xvm_helpers.c) ========== */
XR_FUNC void xr_runtime_error(XrayIsolate *isolate, const char *format, ...);

// Debug info: find local variable name by register and PC
XR_FUNC const char* xr_vm_get_local_name(XrProto *proto, int reg, int pc);

// C function operations
XR_FUNC void xr_vm_cfunction_free(XrCFunction *cfunc);

// Closure operations
XR_FUNC XrClosure *xr_vm_closure_new(XrayIsolate *isolate, XrProto *proto,
                             struct XrCoroutine *coro);

// VM initialization and cleanup
XR_FUNC void xr_vm_vm_init(XrayIsolate *isolate);
XR_FUNC void xr_vm_vm_free(XrayIsolate *isolate);

// Value operation helpers
XR_FUNC bool xr_vm_is_truthy(XrValue value);

/* ========== API Functions (in xvm_api.c) ========== */

// Call closure from C code
XR_FUNC XrValue xr_vm_call_closure(XrayIsolate *isolate, XrClosure *closure, XrValue *args, int nargs);

// Extended closure call (supports blocking return, for coroutines)
XR_FUNC XrValue xr_vm_call_closure_ex(XrayIsolate *isolate, XrClosure *closure, 
                               XrValue *args, int nargs, XrVMResult *out_result);

// VM execution
XR_FUNC XrVMResult xr_vm_interpret_proto(XrayIsolate *isolate, XrProto *proto);
XR_FUNC XrVMResult xr_vm_interpret(const char *source);
XR_FUNC XrVMResult xr_vm_interpret_proto_isolate(XrayIsolate *isolate, XrProto *proto);

// Exception handling
XR_FUNC void xr_vm_add_stacktrace(XrayIsolate *isolate, XrValue exception);
XR_FUNC void xr_vm_throw_exception(XrayIsolate *isolate, XrValue exception);

/* ========== Instruction Operation Helpers (in xvm_ops.c) ========== */

// Helper: create new frame and set stack_top correctly
static inline void vm_push_frame(XrayIsolate *isolate, XrBcCallFrame **frame_ptr,
                                 XrClosure *closure, XrValue *base) {
    CHECK_FRAME_OVERFLOW(isolate);
    
    XrBcCallFrame *new_frame = &isolate->vm.frames[isolate->vm.frame_count++];
    new_frame->closure = closure;
    new_frame->pc = PROTO_CODE_BASE(closure->proto);
    new_frame->base_offset = (int)(base - isolate->vm.stack);
    
    // Critical: must update stack_top
    isolate->vm.stack_top = base + closure->proto->maxstacksize;
    
    *frame_ptr = new_frame;
}

/* ========== Unified Frame Creation Helpers ========== */

/*
** Create new frame for method call (unified entry point)
** 
** Calling convention:
**   caller: R[a]=return_value, R[a+1]=this, R[a+2+]=args
**   callee: R[0]=this, R[1+]=args
**   Return value written to ci->base - 1 = caller.R[a]
** 
** @param isolate     VM instance
** @param ci          Current call frame (for bounds checking)
** @param caller_base Caller's base pointer
** @param a           Base register index
** @param closure     Closure being called
** @return Newly created frame
*/
static inline XrBcCallFrame* vm_create_method_frame(
    XrayIsolate *isolate,
    XrBcCallFrame *ci,
    XrValue *caller_base,
    int a,
    XrClosure *closure
) {
    XrValue *new_base = caller_base + a + 1;
    
    CHECK_FRAME_OVERFLOW(isolate);
    CHECK_FRAME_BOUNDARY(isolate, ci, new_base);
    
    XrBcCallFrame *new_frame = &isolate->vm.frames[isolate->vm.frame_count++];
    new_frame->closure = closure;
    new_frame->pc = PROTO_CODE_BASE(closure->proto);
    new_frame->base_offset = (int)(new_base - isolate->vm.stack);
    new_frame->u.l.pending_operator_check = false;
    
    return new_frame;
}

/*
** Create new frame for regular function call
** 
** Calling convention:
**   caller: R[func]=function_object, R[func+1+]=args
**   callee: R[0]=arg1, R[1]=arg2...
**   Return value written to stack + base_offset - 1 = R[func] (overwrites function object)
*/
static inline XrBcCallFrame* vm_create_function_frame(
    XrayIsolate *isolate,
    XrBcCallFrame *ci,
    XrValue *caller_base,
    int func_reg,
    XrClosure *closure
) {
    XrValue *new_base = caller_base + func_reg + 1;
    
    CHECK_FRAME_OVERFLOW(isolate);
    CHECK_FRAME_BOUNDARY(isolate, ci, new_base);
    
    XrBcCallFrame *new_frame = &isolate->vm.frames[isolate->vm.frame_count++];
    new_frame->closure = closure;
    new_frame->pc = PROTO_CODE_BASE(closure->proto);
    new_frame->base_offset = (int)(new_base - isolate->vm.stack);
    new_frame->u.l.pending_operator_check = false;
    
    return new_frame;
}

// Get safe stack start for C closure call (avoid frame overlap)
static inline XrValue* vm_get_safe_stack_start(XrayIsolate *isolate) {
    XrValue *safe_start = isolate->vm.stack_top;
    
    if (isolate->vm.frame_count > 0) {
        XrBcCallFrame *current = &isolate->vm.frames[isolate->vm.frame_count - 1];
        if (current->closure && current->closure->proto) {
            XrValue *frame_end = isolate->vm.stack + current->base_offset + current->closure->proto->maxstacksize;
            if (safe_start < frame_end) {
                safe_start = frame_end;
            }
        }
    }
    
    return safe_start;
}

// Type conversion
XR_FUNC XrString* vm_value_to_string(XrayIsolate *isolate, XrValue val);

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
XR_FUNC void xr_sched_enqueue(XrScheduler *sched, XrCoroutine *coro);
XR_FUNC void xr_sched_remove(XrScheduler *sched, XrCoroutine *target);
XR_FUNC XrCoroutine *xr_sched_dequeue(XrScheduler *sched);
// Scheduler initialization
XR_FUNC void xr_sched_init(XrScheduler *sched);
XR_FUNC void xr_sched_destroy(XrScheduler *sched);

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
XR_FUNC void xr_scope_add_coro(XrScheduler *sched, XrCoroutine *coro, XrCoroutine *parent);

#endif // XVM_INTERNAL_H
