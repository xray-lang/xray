/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjit_scratch.h - Per-worker JIT scratch layout
 *
 * KEY CONCEPT:
 *   JIT functions are synchronous and only one JIT activation runs on a
 *   worker at a time. The scheduler owns one scratch block per XrProc while
 *   the JIT owns the field layout consumed by generated code offsets.
 */

#ifndef XJIT_SCRATCH_H
#define XJIT_SCRATCH_H

#include <stdatomic.h>
#include <stdint.h>

/* Fixed fast-path scratch capacity for one JIT call/helper argument vector.
 * The count is explicit in call_nargs; helpers must reject above this capacity
 * during lowering instead of truncating in the runtime bridge. */
#define XR_JIT_MAX_CALL_ARGS 64

typedef struct XrJitScratch {
    int64_t call_args[XR_JIT_MAX_CALL_ARGS];      // Raw unboxed arguments
    uint8_t call_arg_tags[XR_JIT_MAX_CALL_ARGS];  // Compile-time XR_TAG_* for each call_arg
    int64_t call_nargs;                           // Number of valid call_args entries
    int64_t extra_arg;                            // CALL_C extra metadata argument
    void *call_proto;                             // Current proto for CALLSELF (XrProto*)
    void *call_closure;                           // Current closure for upvalue access (XrClosure*)
    void *exception;                              // Non-NULL when exception pending in JIT code
    int64_t ret_count;  // Number of return values (0 = single via x0); int64_t for 8-byte alignment
    uint32_t deopt_id;  // Deopt point ID (set by deopt stub)
    uint32_t invoke_deopt_id;  // Valid deopt_id for CALL_C invoke recovery (deopt_id=0 safe)

    /* Param tags: runtime XrValue.tag for each argument, set by xm_jit_call.
     * Used by JIT null-check codegen to distinguish int(0) from null
     * for nullable primitive params (int?/float?/bool?). */
    int64_t param_tags[XR_JIT_MAX_CALL_ARGS];

    /* Multi-return values: ret_vals[0] = 2nd return value, ret_vals[1] = 3rd, etc.
     * First return value goes through x0 as usual.
     * Tags for reconstruction stored in ret_tags[]. */
    int64_t ret_vals[7];  // Extra return values (max 8 total, 1st in x0)
    int64_t ret_tags[7];  // XrValue tags for ret_vals[] (int64_t for 8-byte alignment)

    /* Deopt register snapshot: saved by deopt stub before returning DEOPT_MARKER.
     * Indexed by physical register number for O(1) lookup.
     * GP: x0-x28 (29 slots), FP: d0-d15 (16 slots) */
    int64_t deopt_regs[29];
    int64_t deopt_fp_regs[16];
    int64_t deopt_spill_base;  // Frame pointer captured at deopt for GC recovery

    /* Spill slot snapshot: copied from frame by deopt stub before the epilogue.
     * Indexed by spill slot number: deopt_spill_save[slot] = frame[SPILL_BASE + slot*8].
     * Recovery reads from here instead of the frame, which is deallocated after epilogue.
     * Max slots = XM_MAX_SPILL_SLOTS (32). */
    int64_t deopt_spill_save[32];

    int32_t osr_deopt_pc;  // OSR deopt recovery: bytecode PC to resume (-1 = none)

    /* GC stack map: compile-time bitmap for precise GC root scanning.
     * active_safepoint_id indexes into active_stack_map->entries[] to find
     * which registers/spill slots hold GC pointers at the current safepoint. */
    uint32_t active_safepoint_id;  // current safepoint index (UINT32_MAX = none)
    void *active_stack_map;        // XrStackMapTable* for current JIT function
    void *jit_frame_sp;            // FP of current (innermost) JIT frame
    void *safepoint_saved_sp;      // SP saved by safepoint stub (for reading saved regs)

    /* JIT frame stack: caller FPs pushed before cross-function JIT calls.
     *
     * The array replaces a linked root chain. Each cross-function call pushes
     * the caller FP before the call and pops after return, so GC can walk
     * caller frames' spill slots without traversing heap nodes. */
#define XR_JIT_MAX_FRAME_DEPTH 16
    uint32_t jit_frame_depth;
    void *jit_frame_stack[XR_JIT_MAX_FRAME_DEPTH];

    /* Per-vreg runtime tags: written by CALL_C codegen after each CALL_C.
     * Indexed by vreg index (directly from XmRef, no bc_slot indirection).
     * Consumers that need a dynamic tag read vreg_runtime_tags[vreg_idx]. */
#define XR_JIT_MAX_VREG_TAGS 512
    uint8_t vreg_runtime_tags[XR_JIT_MAX_VREG_TAGS];

    /* Tag returned by the last call_c_stub invocation.
     * call_c_stub stores the C helper's x1 here instead of returning it in
     * x1, so x1 is not clobbered by the stub return sequence. */
    int64_t call_result_tag;
    int64_t tag_scratch;

    /* JIT invoke recovery:
     * yieldable C functions run in try-mode from JIT. If they would suspend,
     * the helper requests deopt at invoke_deopt_id so the interpreter executes
     * the ordinary blocking call path. */
    int32_t call_base_offset;  // callee base_offset (set by VM before xm_jit_call)

    /* Heartbeat pointer: set by run_on_worker to &machine->heartbeat.
     * Bumped by xr_coro_gc_safepoint so sysmon does not misdetect
     * long-running JIT code as stuck. */
    _Atomic uint64_t *heartbeat_ptr;

    /* Guard page safepoint: x20 points here.
     * Normal: PROT_READ, ldr succeeds.
     * Armed: PROT_NONE, ldr faults into the safepoint trampoline. */
    void *safepoint_page;       // mmap'd guard page (one per worker)
    void *safepoint_return_pc;  // saved PC+4 after guard page fault
} XrJitScratch;

#endif  // XJIT_SCRATCH_H
