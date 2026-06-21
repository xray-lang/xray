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
 *   XrVMState is the isolate's VM storage host: it embeds the fixed-size
 *   value stack, frame array, exception-handler array, builtin globals,
 *   shared variables, GC counters, defer stack, and VM bookkeeping. Scheduler
 *   runtime ownership lives on XrayIsolate, not in XrVMState.
 *
 *   IMPORTANT: XrVMState and XrVMContext (xexec_frame.h) are
 *   complementary, not redundant:
 *
 *     - XrVMState owns the storage. The fields stack[], frames[],
 *       exception_handlers[], strings_map, builtins[], shared,
 *       defer_stack, coro_state, ctor_call_stack are
 *       the actual backing memory.
 *     - XrVMContext is the access path (xexec_frame.h). It carries
 *       pointers (stack / frames / handlers / ic_*_tables) that the
 *       run() loop reads on the hot path. Single-thread mode has
 *       isolate->vm_ctx aliasing into isolate->vm.* for zero-overhead
 *       access; per-coroutine mode has each XrCoroutine carrying its
 *       own XrVMContext with independently allocated buffers.
 *
 *   Code accessing VM-wide configuration (builtins, shared, coro_state) goes
 *   through isolate->vm.*. Code accessing scheduler runtime uses the
 *   isolate-level scheduler owner.
 *   directly. Code accessing per-execution-entity state (current
 *   stack/frames/handlers/IC tables) MUST go through a XrVMContext --
 *   either xr_vm_current_ctx(isolate) or one threaded through the
 *   helper signature. Only init / teardown paths in xvm_helpers.c and
 *   xvm_compile.c touch isolate->vm.{stack,frames,...} directly to
 *   prepare the storage that XrVMContext later aliases.
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
    void *class_ptr;  // class being constructed
    int frame_count;  // frame depth at constructor entry
} XrCtorCallEntry;

struct XrStructLayout;

/* ========== Dynamic Shared Array ========== */

#define XR_SHARED_INITIAL_CAPACITY 64
#define XR_SHARED_GROW_FACTOR 2

typedef struct XrSharedArray {
    XrValue *data;  // dynamic array
    int capacity;   // current capacity
    int count;      // allocated count (high water mark)
    int free_list;  // free index list head (-1 = empty)
} XrSharedArray;

// Initialize shared array
static inline void xr_shared_array_init(XrSharedArray *arr) {
    arr->capacity = XR_SHARED_INITIAL_CAPACITY;
    arr->data = (XrValue *) xr_malloc(arr->capacity * sizeof(XrValue));
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
    if (index < arr->capacity)
        return;

    int new_cap = arr->capacity;
    while (new_cap <= index) {
        new_cap *= XR_SHARED_GROW_FACTOR;
    }

    XrValue *new_data = (XrValue *) xr_realloc(arr->data, new_cap * sizeof(XrValue));
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
    XrValue stack[XR_STACK_MAX];  // fixed-size value stack
    XrValue *stack_top;           // next free slot

    // Call stack
    XrBcCallFrame frames[XR_FRAMES_MAX];  // call frame array
    int frame_count;                      // current call depth
    int module_base_frame;                // module boundary for stack trace

    // Exception handling (legacy, retained for panic)
    XrExceptionHandler exception_handlers[XR_EXCEPTION_HANDLERS_MAX];
    int handler_count;
    XrValue current_exception;  // active exception

    // Value-return error channel (null = no error)
    XrValue pending_error;

    // Closure support
    void *strings_map;  // interned strings table

    // Builtin globals (read-only, predefined) + shared variables (user-defined)
    XrValue builtins[XR_GLOBALS_MAX];  // predefined builtin globals (Reflect, Array, etc.)
    int builtin_count;
    XrSharedArray shared;  // dynamic shared variable storage (legacy slot-indexed top-level;
                           // being phased out in favor of `globals` below)
    struct XrGlobalDict *globals;  // name-keyed top-level binding dict (single source of truth
                                   // post-migration; coexists with `shared` until Phase 3)

    // GC state
    size_t bytes_allocated;  // total allocated bytes
    size_t next_gc;          // threshold for next GC

    // Execution state
    bool trace_execution;  // debug: trace opcodes
    int last_nret;         // return count from last call

    // Constructor tracking (for super() validation)
    XrCtorCallEntry ctor_call_stack[XR_CTOR_CALL_STACK_MAX];
    int ctor_call_depth;

    // Coroutine support
    void *coro_state;           // XrCoroState* (single-thread scheduler + bookkeeping)
    void *current_coro;         // currently running coroutine
    struct XrMap *main_locals;  // REPL local variables

    // Isolate-wide value-struct layout registry. STRUCT_REF.heap_type stores
    // this 16-bit id when the payload has no XrClass* header.
    struct XrStructLayout **struct_layouts;
    uint16_t struct_layout_count;
    uint16_t struct_layout_capacity;

} XrVMState;

XR_FUNC uint16_t xr_vm_struct_layout_register(XrVMState *vm, struct XrStructLayout *layout);
XR_FUNC struct XrStructLayout *xr_vm_struct_layout_lookup(XrVMState *vm, uint16_t layout_id);

#endif  // XEXEC_STATE_H
