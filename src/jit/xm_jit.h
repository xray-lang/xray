/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_jit.h - JIT compiler state and VM integration
 *
 * KEY CONCEPT:
 *   Manages the JIT compilation pipeline: hot function detection,
 *   bytecode → Xm → ARM64 compilation, and native code execution.
 *   The VM calls xm_jit_try_compile() when call_count reaches threshold,
 *   and xm_jit_call() to execute compiled code.
 *
 * WHY THIS DESIGN:
 *   - Thin integration layer between VM and Xm pipeline
 *   - C bridge unpacks XrValue → raw int64 args array, calls JIT, repacks result
 *   - JIT calling convention: x0=coro, x1=args_ptr, return in x0
 *   - Unified args array: consistent with OSR entry and future JIT→JIT calls
 */

#ifndef XM_JIT_H
#define XM_JIT_H

#include "xm_code_alloc.h"
#include "xm_codegen.h"
#include "xm_tfa.h"
#include "xjit_compile_queue.h"
#include <stdint.h>
#include <stdbool.h>
#include "../base/xdefs.h"

typedef struct XrProto XrProto;
typedef struct XrValue XrValue;
typedef struct XrCoroutine XrCoroutine;

/* ========== JIT State ========== */

typedef struct XmJitState {
    XmCodeAlloc code_alloc;
    struct XrayIsolate *isolate;  // owning isolate (for CHA class lookup)
    int threshold;                // call count threshold for Tier 1
    int compiled_count;
    bool enabled;
    TfaState *tfa;  // lazily allocated TFA state (NULL until first use)
    void
        *dominant_shape;  // XrShape* — most common NEWJSON shape in module (NULL if none/ambiguous)
    bool verbose;         // diagnostic logging (auto-enabled by --jit-force)
    bool stats_enabled;   // --jit-stats: print compilation statistics on exit
    XmCompileQueue *bg_queue;  // background compilation queue (NULL if sync mode)

    // Code cache eviction: track compiled protos for LRU reclaim
    struct XrProto **compiled_protos;  // array of compiled protos (for eviction scan)
    uint32_t compiled_protos_count;
    uint32_t compiled_protos_cap;
    uint32_t stats_evicted;  // number of evicted protos

    // Statistics (populated during compilation, printed by xm_jit_destroy)
    uint32_t stats_tier1;         // functions compiled at Tier 1
    uint32_t stats_tier2;         // functions recompiled at Tier 2
    uint32_t stats_conservative;  // conservative recompiles (deopt_count >= 5)
    uint32_t stats_deopt_total;   // total deopts across all protos
    uint32_t stats_disabled;      // protos permanently disabled (deopt_count >= 20)
    uint64_t stats_compile_ns;    // cumulative compile time (nanoseconds)
    uint64_t stats_code_bytes;    // total generated code bytes
} XmJitState;

/* ========== Deoptimization ========== */

// Sentinel return value from JIT code when a guard fails.
// Chosen to be an invalid raw payload sentinel so JIT/bridge code can
// recognize deopt without treating it as a normal helper result.
#define XM_DEOPT_MARKER ((int64_t) 0xDEAD0001DEAD0001LL)

/* ========== Suspend/Resume ========== */

// Sentinel return value from JIT code when AWAIT needs to block.
// JIT saves live registers to coro->jit_suspend and returns this.
// xm_jit_call detects it and returns XR_VM_BLOCKED to the worker.
// On resume, worker calls jit_resume_entry to re-enter JIT code.
#define XM_SUSPEND_MARKER ((int64_t) 0xDEAD0002DEAD0002LL)

/* ========== API ========== */

XR_FUNC XmJitState *xm_jit_init(struct XrayIsolate *isolate, int threshold);
XR_FUNC void xm_jit_destroy(XmJitState *jit);

// Try to compile a proto to native code.
// On success, sets proto->jit_entry and returns true.
// Compiles typed functions (i64/f64/ptr/bool parameters and return).
XR_FUNC bool xm_jit_try_compile(XmJitState *jit, XrProto *proto);

// Call a JIT-compiled function with arguments from the VM stack.
// JIT convention: x0=coro, x1=pointer to raw int64 args array.
// coro: current coroutine (for safepoint checks)
// args: pointer to first argument on VM stack (XrValue array)
// nargs: number of arguments (must match proto->numparams)
// return_type_info: XrType* for return value (NULL = any/void)
// result: output XrValue
// Returns: XM_JIT_OK on success, XM_JIT_DEOPT on deopt, XM_JIT_SUSPEND on
// channel/await block. Thread-local return avoids gopark race on jit_ctx.
#define XM_JIT_OK 0
#define XM_JIT_DEOPT 1
#define XM_JIT_SUSPEND 2
XR_FUNC int xm_jit_call(void *jit_entry, XrCoroutine *coro, XrValue *args, int nargs,
                        struct XrType *return_type_info, XrValue *result);

// Multi-return value reconstruction: fill results[1..n] from jit_ctx->ret_vals[].
// Called after xm_jit_call() succeeds when the caller expects multiple results.
// results[0] must already be filled by xm_jit_call; nresults = total expected.
XR_FUNC void xm_jit_read_multi_ret(XrCoroutine *coro, XrValue *results, int nresults);

// Mid-function deopt recovery: reconstruct VM frame from saved registers.
// Must be called after xm_jit_call returns false due to deopt.
// frame: VM register array (R[0]..R[maxstack-1]) to fill
// maxstack: size of frame array
// Returns bytecode PC to resume at, or -1 if recovery not possible.
XR_FUNC int32_t xm_jit_deopt_recover(XrCoroutine *coro, XrValue *frame, int maxstack);

// Resume JIT execution after suspend (await blocked then woken).
// Called by worker when coro->jit_resume_entry != NULL.
// Re-enters JIT via resume entry stub, which reloads saved registers
// and dispatches to the continuation point.
// Returns: XM_JIT_OK on success, XM_JIT_DEOPT on deopt, XM_JIT_SUSPEND
// on nested channel/await block.  Thread-local return avoids gopark race.
XR_FUNC int xm_jit_resume(XrCoroutine *coro, XrValue *result);

// OSR entry: enter JIT code at a loop header.
// osr_entry: pointer to OSR entry stub (code + osr_offset)
// coro: current coroutine
// values: array of raw int64 values (indexed by bytecode slot)
// result: output XrValue
// Returns true on success, false on deopt.
XR_FUNC bool xm_jit_osr_enter(void *osr_entry, XrCoroutine *coro, int64_t *values,
                              uint8_t return_type, XrValue *result);

/* Unified JIT result installation data.
 * Both sync and background paths fill this struct, then call
 * xm_jit_install_to_proto() which writes fields in the correct order
 * with a release fence before publishing jit_entry. */
typedef struct {
    void *code;          // compiled machine code (becomes jit_entry)
    void *fast_entry;    // fast entry point (skip param setup)
    void *resume_entry;  // resume entry for suspend/resume (NULL = none)
    uint8_t opt_level;   // XM_OPT_BASIC or XM_OPT_FULL
    void *stack_map;     // XrStackMapTable* (ownership transferred)
    void *deopt_table;   // XmRtDeoptEntry* (heap-allocated, ownership transferred)
    uint32_t ndeopt;
    void *osr_entries;  // XmOsrEntry* (heap-allocated, ownership transferred)
    uint32_t nosr;
} XmInstallData;

// Install compiled JIT code into proto fields with correct memory ordering.
// All metadata is written BEFORE jit_entry (the publish point).
// A release fence ensures visibility on ARM64 weak memory order.
// Frees old metadata (stack_map, deopt_table, osr_entries) if present.
XR_FUNC void xm_jit_install_to_proto(XrProto *proto, const XmInstallData *data);

// Install a completed background compilation result into proto fields.
// Thread-safe: uses CAS to prevent double-install from racing workers.
// Called from both OP_CALL and OSR paths when jit_entry_pending is ready.
XR_FUNC void xm_jit_install_bg_result(XrProto *proto);

// OSR trigger: called from VM at loop back-edges.
// Tries JIT compilation if not yet compiled, then attempts OSR entry.
// bc_pc: bytecode PC index of the loop header (target of back-edge)
// base: pointer to R(0) on the interpreter stack
// maxstack: proto->maxstacksize (number of register slots)
// return_type: derived from proto->return_type_info
// result: output XrValue (set on successful OSR)
// Returns: XM_JIT_OK on success, XM_JIT_DEOPT on deopt/no-match,
// XM_JIT_SUSPEND on channel/await block.
XR_FUNC int xm_jit_osr_trigger(XmJitState *jit, XrProto *proto, XrCoroutine *coro, uint32_t bc_pc,
                               XrValue *base, int maxstack, uint8_t return_type, XrValue *result);

#endif  // XM_JIT_H
