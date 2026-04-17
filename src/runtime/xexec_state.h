/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexec_state.h - Isolate-wide execution state (XrVMState, shared arrays,
 *                  constructor call stack)
 *
 * KEY CONCEPT:
 *   XrVMState is the isolate's execution state: value stack, frames,
 *   exception handlers, builtin globals, shared variables, GC counters,
 *   JIT state (if enabled), defer stack, etc. Lives at the runtime layer
 *   so gc/, class/, reflection and JIT-free builds can all consume it
 *   without reverse-depending on vm/.
 */

#ifndef XEXEC_STATE_H
#define XEXEC_STATE_H

#include "value/xvalue.h"
#include "xexec_frame.h"
#include "../base/xconstants.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"

/* ========== Constructor Call Stack Entry ========== */

typedef struct XrCtorCallEntry {
    void *class_ptr;            // class being constructed
    int frame_count;            // frame depth at constructor entry
} XrCtorCallEntry;

/* ========== Dynamic Shared Array ========== */

#define XR_SHARED_INITIAL_CAPACITY 64
#define XR_SHARED_GROW_FACTOR 2

typedef struct XrSharedArray {
    XrValue *data;              // dynamic array
    int capacity;               // current capacity
    int count;                  // allocated count (high water mark)
    int free_list;              // free index list head (-1 = empty)
} XrSharedArray;

// Initialize shared array
static inline void xr_shared_array_init(XrSharedArray *arr) {
    arr->capacity = XR_SHARED_INITIAL_CAPACITY;
    arr->data = (XrValue *)xr_malloc(arr->capacity * sizeof(XrValue));
    XR_CHECK(arr->data != NULL, "shared array allocation failed");
    arr->count = 0;
    arr->free_list = -1;
    for (int i = 0; i < arr->capacity; i++) {
        arr->data[i] = XR_NULL_VAL;
    }
}

// Free shared array
static inline void xr_shared_array_free(XrSharedArray *arr) {
    if (arr->data) {
        xr_free(arr->data);
        arr->data = NULL;
    }
    arr->capacity = 0;
    arr->count = 0;
    arr->free_list = -1;
}

// Ensure capacity (grow if needed)
static inline void xr_shared_array_ensure(XrSharedArray *arr, int index) {
    if (index < arr->capacity) return;

    int new_cap = arr->capacity;
    while (new_cap <= index) {
        new_cap *= XR_SHARED_GROW_FACTOR;
    }

    XrValue *new_data = (XrValue *)xr_realloc(arr->data, new_cap * sizeof(XrValue));
    XR_CHECK(new_data != NULL, "shared array reallocation failed");
    arr->data = new_data;
    for (int i = arr->capacity; i < new_cap; i++) {
        arr->data[i] = XR_NULL_VAL;
    }
    arr->capacity = new_cap;
}

// Get value at index (with auto-grow)
static inline XrValue xr_shared_array_get(XrSharedArray *arr, int index) {
    if (index >= arr->capacity) {
        return XR_NULL_VAL;
    }
    return arr->data[index];
}

// Set value at index (with auto-grow)
static inline void xr_shared_array_set(XrSharedArray *arr, int index, XrValue val) {
    xr_shared_array_ensure(arr, index);
    arr->data[index] = val;
    if (index >= arr->count) {
        arr->count = index + 1;
    }
}

/* ========== VM Execution State ========== */

typedef struct XrVMState {
    // Value stack
    XrValue stack[XR_STACK_MAX];   // fixed-size value stack
    XrValue *stack_top;            // next free slot

    // Call stack
    XrBcCallFrame frames[XR_FRAMES_MAX]; // call frame array
    int frame_count;               // current call depth
    int module_base_frame;         // module boundary for stack trace

    // Exception handling
    XrExceptionHandler exception_handlers[XR_EXCEPTION_HANDLERS_MAX];
    int handler_count;
    XrValue current_exception;     // active exception

    // Closure support
    void *strings_map;             // interned strings table

    // Builtin globals (read-only, predefined) + shared variables (user-defined)
    XrValue builtins[XR_GLOBALS_MAX];       // predefined builtin globals (Reflect, Array, etc.)
    int builtin_count;
    XrSharedArray shared;                   // dynamic shared variable storage

    // GC state
    size_t bytes_allocated;        // total allocated bytes
    size_t next_gc;                // threshold for next GC

    // Execution state
    bool trace_execution;          // debug: trace opcodes
    int last_nret;                 // return count from last call

    // Constructor tracking (for super() validation)
    XrCtorCallEntry ctor_call_stack[XR_CTOR_CALL_STACK_MAX];
    int ctor_call_depth;

#ifdef XRAY_HAS_JIT
    // JIT compiler state (v3: self-hosted XIR pipeline)
    struct XirJitState *jit;       // JIT compiler state (compile queue, code cache)
    int jit_threshold;             // call count threshold for Tier 1 compilation
    int jit_opt_threshold;         // exec count threshold for Tier 2 optimization
#endif

    // Coroutine support
    void *scheduler;               // coroutine scheduler
    void *current_coro;            // currently running coroutine
    struct XrMap *main_locals;     // REPL local variables

    // Multi-core runtime
    void *runtime;                 // XrayRuntime*
    bool multicore_enabled;

    // Defer stack
    XrValue *defer_stack;          // deferred function calls
    int defer_count;
    int defer_capacity;
    int *defer_frame_marks;        // frame boundaries in defer stack

} XrVMState;

#endif // XEXEC_STATE_H
