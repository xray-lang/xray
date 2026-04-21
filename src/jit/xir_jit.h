/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_jit.h - JIT compiler state and VM integration
 *
 * KEY CONCEPT:
 *   Manages the JIT compilation pipeline: hot function detection,
 *   bytecode → XIR → ARM64 compilation, and native code execution.
 *   The VM calls xir_jit_try_compile() when call_count reaches threshold,
 *   and xir_jit_call() to execute compiled code.
 *
 * WHY THIS DESIGN:
 *   - Thin integration layer between VM and XIR pipeline
 *   - C bridge unpacks XrValue → raw int64 args array, calls JIT, repacks result
 *   - JIT calling convention: x0=coro, x1=args_ptr, return in x0
 *   - Unified args array: consistent with OSR entry and future JIT→JIT calls
 */

#ifndef XIR_JIT_H
#define XIR_JIT_H

#include "xir_code_alloc.h"
#include "xir_codegen.h"
#include "xir_tfa.h"
#include "xjit_compile_queue.h"
#include <stdint.h>
#include <stdbool.h>
#include "../base/xdefs.h"

typedef struct XrProto XrProto;
typedef struct XrValue XrValue;
typedef struct XrCoroutine XrCoroutine;

/* ========== JIT State ========== */

typedef struct XirJitState {
    XirCodeAlloc code_alloc;
    struct XrayIsolate *isolate;   // owning isolate (for CHA class lookup)
    int          threshold;        // call count threshold for Tier 1
    int          compiled_count;
    bool         enabled;
    TfaState    *tfa;              // lazily allocated TFA state (NULL until first use)
    void        *dominant_shape;   // XrShape* — most common NEWJSON shape in module (NULL if none/ambiguous)
    bool         verbose;          // diagnostic logging (auto-enabled by --jit-force)
    XirCompileQueue *bg_queue;     // background compilation queue (NULL if sync mode)
} XirJitState;

/* ========== Deoptimization ========== */

// Sentinel return value from JIT code when a guard fails.
// Chosen to be an invalid XrValue bit pattern (NaN-boxed nonsense).
#define XIR_DEOPT_MARKER  ((int64_t)0xDEAD0001DEAD0001LL)

/* ========== Suspend/Resume ========== */

// Sentinel return value from JIT code when AWAIT needs to block.
// JIT saves live registers to coro->jit_suspend and returns this.
// xir_jit_call detects it and returns XR_VM_BLOCKED to the worker.
// On resume, worker calls jit_resume_entry to re-enter JIT code.
#define XIR_SUSPEND_MARKER ((int64_t)0xDEAD0002DEAD0002LL)

/* ========== API ========== */

XR_FUNC XirJitState *xir_jit_init(struct XrayIsolate *isolate, int threshold);
XR_FUNC void         xir_jit_destroy(XirJitState *jit);

// Try to compile a proto to native code.
// On success, sets proto->jit_entry and returns true.
// Compiles typed functions (i64/f64/ptr/bool parameters and return).
XR_FUNC bool xir_jit_try_compile(XirJitState *jit, XrProto *proto);

// Call a JIT-compiled function with arguments from the VM stack.
// JIT convention: x0=coro, x1=pointer to raw int64 args array.
// coro: current coroutine (for safepoint checks)
// args: pointer to first argument on VM stack (XrValue array)
// nargs: number of arguments (must match proto->numparams)
// return_type_info: XrType* for return value (NULL = any/void)
// result: output XrValue
// Returns: XIR_JIT_OK on success, XIR_JIT_DEOPT on deopt, XIR_JIT_SUSPEND on
// channel/await block. Thread-local return avoids gopark race on jit_ctx.
#define XIR_JIT_OK      0
#define XIR_JIT_DEOPT   1
#define XIR_JIT_SUSPEND 2
XR_FUNC int xir_jit_call(void *jit_entry, XrCoroutine *coro,
                  XrValue *args, int nargs,
                  struct XrType *return_type_info, XrValue *result);

// Multi-return value reconstruction: fill results[1..n] from jit_ctx->ret_vals[].
// Called after xir_jit_call() succeeds when the caller expects multiple results.
// results[0] must already be filled by xir_jit_call; nresults = total expected.
XR_FUNC void xir_jit_read_multi_ret(XrCoroutine *coro, XrValue *results, int nresults);

// Mid-function deopt recovery: reconstruct VM frame from saved registers.
// Must be called after xir_jit_call returns false due to deopt.
// frame: VM register array (R[0]..R[maxstack-1]) to fill
// maxstack: size of frame array
// Returns bytecode PC to resume at, or -1 if recovery not possible.
XR_FUNC int32_t xir_jit_deopt_recover(XrCoroutine *coro, XrValue *frame, int maxstack);

// Resume JIT execution after suspend (await blocked then woken).
// Called by worker when coro->jit_resume_entry != NULL.
// Re-enters JIT via resume entry stub, which reloads saved registers
// and dispatches to the continuation point.
// Returns: XIR_JIT_OK on success, XIR_JIT_DEOPT on deopt, XIR_JIT_SUSPEND
// on nested channel/await block.  Thread-local return avoids gopark race.
XR_FUNC int xir_jit_resume(XrCoroutine *coro, XrValue *result);

// OSR entry: enter JIT code at a loop header.
// osr_entry: pointer to OSR entry stub (code + osr_offset)
// coro: current coroutine
// values: array of raw int64 values (indexed by bytecode slot)
// result: output XrValue
// Returns true on success, false on deopt.
XR_FUNC bool xir_jit_osr_enter(void *osr_entry, XrCoroutine *coro,
                        int64_t *values, uint8_t return_type,
                        XrValue *result);

// Install a completed background compilation result into proto fields.
// Thread-safe: uses CAS to prevent double-install from racing workers.
// Called from both OP_CALL and OSR paths when jit_entry_pending is ready.
XR_FUNC void xir_jit_install_bg_result(XrProto *proto);

// OSR trigger: called from VM at loop back-edges.
// Tries JIT compilation if not yet compiled, then attempts OSR entry.
// bc_pc: bytecode PC index of the loop header (target of back-edge)
// base: pointer to R(0) on the interpreter stack
// maxstack: proto->maxstacksize (number of register slots)
// return_type: derived from proto->return_type_info
// result: output XrValue (set on successful OSR)
// Returns: XIR_JIT_OK on success, XIR_JIT_DEOPT on deopt/no-match,
// XIR_JIT_SUSPEND on channel/await block.
XR_FUNC int xir_jit_osr_trigger(XirJitState *jit, XrProto *proto, XrCoroutine *coro,
                          uint32_t bc_pc, XrValue *base, int maxstack,
                          uint8_t return_type, XrValue *result);

#endif // XIR_JIT_H
