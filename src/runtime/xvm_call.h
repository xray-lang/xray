/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_call.h - Minimal VM call interface for lower-layer modules
 *
 * KEY CONCEPT:
 *   Declares xr_vm_call_closure() without pulling in full VM headers.
 *   Allows object/(L2) and class/(L3) to call closures without
 *   depending on vm/(L5) headers.
 *
 * WHY THIS DESIGN:
 *   - Breaks L2/L3 → L5 layer violation for higher-order methods
 *     (forEach, map, filter, reduce, any, etc.)
 *   - Function signature only needs types from value/(L1) layer
 *   - Implementation stays in vm/xvm.c
 */

#ifndef XVM_CALL_H
#define XVM_CALL_H

#include "value/xvalue.h"

/* ========== VM Execution Result ========== */

typedef enum {
    XR_VM_OK,
    XR_VM_COMPILE_ERROR,
    XR_VM_RUNTIME_ERROR,
    XR_VM_BLOCKED,      // Coroutine blocked (waiting on Channel)
    XR_VM_YIELD,        // Coroutine yielded (preemptive scheduling)
    XR_VM_DEBUG_BREAK,  // Stopped at breakpoint
    XR_VM_CANCELLED,    // Coroutine cancelled (sysmon or user cancel)
    XR_VM_GO_CHILD      // Continuation stealing: child ready, parent saved
} XrVMResult;

/* ========== VM Call Interface ========== */

struct XrayIsolate;
struct XrClosure;

// Call a script closure from C code.
// Used by higher-order container methods (map/filter/reduce/forEach).
// Implementation in vm/xvm.c.
XR_FUNC XrValue xr_vm_call_closure(struct XrayIsolate *isolate, struct XrClosure *closure,
                                   XrValue *args, int nargs);

// Truthy check for higher-order filter methods.
// Implementation in vm/xvm.c.
XR_FUNC bool xr_vm_is_truthy(XrValue value);

/* ========== VM Interpreter Entry (coro → vm decoupling) ========== */

struct XrVMContext;

// Execute bytecode from current frame.  Implementation in vm/xvm.c.
// Declared here so coro/ (L3) can call the interpreter without
// including vm/xvm_internal.h (L5).
XR_FUNC XrVMResult run(struct XrayIsolate *isolate, struct XrVMContext *vm_ctx);

#endif  // XVM_CALL_H
