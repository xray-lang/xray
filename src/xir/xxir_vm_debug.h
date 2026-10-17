/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_debug.h - Owned observations of admitted Lowered VM execution
 */
#ifndef XXIR_VM_DEBUG_H
#define XXIR_VM_DEBUG_H
#include "xxir_vm.h"

typedef struct XrXirVmDebug XrXirVmDebug;
typedef struct XrXirVmDebugSnapshot XrXirVmDebugSnapshot;
typedef enum XrXirVmDebugMode {
    XR_XIR_VM_DEBUG_CONTINUE, XR_XIR_VM_DEBUG_PAUSE,
    XR_XIR_VM_DEBUG_STEP_IN, XR_XIR_VM_DEBUG_STEP_OVER, XR_XIR_VM_DEBUG_STEP_OUT,
    XR_XIR_VM_DEBUG_STATEMENT_IN, XR_XIR_VM_DEBUG_STATEMENT_OVER, XR_XIR_VM_DEBUG_STATEMENT_OUT,
    XR_XIR_VM_DEBUG_LINE_IN, XR_XIR_VM_DEBUG_LINE_OVER, XR_XIR_VM_DEBUG_LINE_OUT
} XrXirVmDebugMode;
typedef struct XrXirVmDebugLocation { uint32_t function, instruction; } XrXirVmDebugLocation;
typedef enum XrXirVmDebugHitKind {
    XR_XIR_VM_DEBUG_HIT_ALWAYS,XR_XIR_VM_DEBUG_HIT_EQUAL,XR_XIR_VM_DEBUG_HIT_GREATER,
    XR_XIR_VM_DEBUG_HIT_GREATER_EQUAL,XR_XIR_VM_DEBUG_HIT_LESS,XR_XIR_VM_DEBUG_HIT_LESS_EQUAL,
    XR_XIR_VM_DEBUG_HIT_MULTIPLE
} XrXirVmDebugHitKind;
typedef struct XrXirVmDebugRule {
    XrXirVmDebugLocation location;
    XrXirVmDebugHitKind hit_kind;
    uint64_t hit_value;
    /* Zero counts this location independently; 1..32 share one logical hit
     * counter across locations. A group's predicate must be identical. */
    uint32_t group;
} XrXirVmDebugRule;
typedef enum XrXirVmDebugCode { XR_XIR_VM_DEBUG_PENDING,XR_XIR_VM_DEBUG_NATIVE,XR_XIR_VM_DEBUG_VM } XrXirVmDebugCode;

typedef struct XrXirVmDebugFrameInfo {
    uint64_t id, epoch;
    uint32_t function, instruction, value_count;
    uint64_t thread;
    XrXirVmDebugCode code;
} XrXirVmDebugFrameInfo;

/* Metadata uses this current Domain throughout the observer's lifetime.
 * Capacity bounds all observed user frames across activations. Native frames
 * carry authenticated metadata only. State operations and queries
 * run on its sole control/drive owner between polls; only request_pause may
 * be called concurrently. Owned snapshot references are atomic. */
XR_FUNC XrXirValueStatus xr_xir_vm_debug_new(XrXirDomain *domain, uint32_t capacity,
    XrXirVmDebug **output);
XR_FUNC void xr_xir_vm_debug_drop(XrXirVmDebug *debug);
/* Success transfers the artifact through the ordinary Program receiving gate
 * and retains the observer. Failure preserves artifact and output. */
XR_FUNC XrXirStatus xr_xir_compile_vm_debug_program_take(XrXirArtifact **artifact,
    XrXirVmDebug *debug, XrXirProgram **output);
/* Select genuine native entries from the exact same closed proof. A nonzero
 * mask byte selects native; zero selects observed VM. Native callbacks and
 * environments remain intact. Success moves artifact and native->code; failure
 * preserves both inputs and output. At least one entry of each kind is required. */
XR_FUNC XrXirStatus xr_xir_compile_vm_debug_mixed_program_take(XrXirArtifact **,
    XrXirProgramSpec *native,const unsigned char *mask,uint32_t count,XrXirVmDebug *,XrXirProgram **);
/* Construct only the Instance of this observer's authenticated Program.
 * One target is bound; foreign Programs and a second target are rejected. */
XR_FUNC XrXirCallStatus xr_xir_vm_debug_instance_new(XrXirVmDebug *debug,
    XrXirProgram *program, const XrXirInstanceConfig *config, XrXirInstance **output);
/* Locations are verified function/instruction ordinals. They are not source
 * lines. Changes require a stopped observer or no live execution frames. */
XR_FUNC XrXirValueStatus xr_xir_vm_debug_breakpoint(XrXirVmDebug *debug,
    uint32_t function, uint32_t instruction, bool enabled);
/* Atomically replaces the bounded breakpoint set after validating every
 * location and charging work. Failure preserves the previous set. */
XR_FUNC XrXirValueStatus xr_xir_vm_debug_breakpoints_replace(XrXirVmDebug *,
    const XrXirVmDebugLocation *, uint32_t count);
/* Replace the entire rule set after verification; successful replacement resets
 * encounter counts. Counts cover actual VM visits across the bound target and
 * never count a stopped PC again on resume. Overflow fails without wrapping. */
XR_FUNC XrXirValueStatus xr_xir_vm_debug_rules_replace(XrXirVmDebug *,const XrXirVmDebugRule *,uint32_t count);
typedef enum XrXirVmDebugExceptionFilter {
    XR_XIR_VM_DEBUG_EXCEPTION_NONE=0, XR_XIR_VM_DEBUG_EXCEPTION_ALL=1,
    XR_XIR_VM_DEBUG_EXCEPTION_CAUGHT=2, XR_XIR_VM_DEBUG_EXCEPTION_UNCAUGHT=4
} XrXirVmDebugExceptionFilter;
/* Select first-chance language throws in the current logical execution flow.
 * Caught/uncaught require complete Source facts and a pure observed VM Program;
 * a Task escape is uncaught in that flow, regardless of a future await. Faults
 * and native-origin throws are not silently classified by these VM filters.
 * Source-owned throw sites include local jumps and cleanup error continuations;
 * an unmapped IR function observes its actual THROW operation. */
XR_FUNC XrXirValueStatus xr_xir_vm_debug_break_on_throw(XrXirVmDebug *,uint32_t filters);
/* Pause requests may arrive from the control thread. The sole execution driver
 * polls one quantum at a time and stops polling while paused. No language
 * suspension or wake token is fabricated for a debugger stop. */
XR_FUNC void xr_xir_vm_debug_request_pause(XrXirVmDebug *debug);
XR_FUNC bool xr_xir_vm_debug_paused(const XrXirVmDebug *debug);
XR_FUNC uint64_t xr_xir_vm_debug_epoch(const XrXirVmDebug *debug);
/* Disable observation before teardown so language cleanup cannot be stopped. */
XR_FUNC void xr_xir_vm_debug_disable(XrXirVmDebug *debug);
/* A current epoch is required; stale requests preserve the stopped state. */
XR_FUNC XrXirValueStatus xr_xir_vm_debug_continue(XrXirVmDebug *debug,
    uint64_t epoch, XrXirVmDebugMode mode);
/* Pay passive Call notification work outside the producer callback. A disabled
 * observer does no further observation work and cannot interrupt cleanup. */
XR_FUNC XrXirValueStatus xr_xir_vm_debug_observation_status(XrXirVmDebug *,bool terminal);
/* Returns an independent owned snapshot. It survives resume, cancellation,
 * Program disposal and observer disposal. No frame pointer is exposed. */
XR_FUNC XrXirValueStatus xr_xir_vm_debug_snapshot(XrXirVmDebug *debug,
    XrXirVmDebugSnapshot **output);
XR_FUNC void xr_xir_vm_debug_snapshot_drop(XrXirVmDebugSnapshot *snapshot);
XR_FUNC uint32_t xr_xir_vm_debug_snapshot_depth(const XrXirVmDebugSnapshot *snapshot);
XR_FUNC uint64_t xr_xir_vm_debug_snapshot_stopped_thread(const XrXirVmDebugSnapshot *);
XR_FUNC XrXirValueStatus xr_xir_vm_debug_snapshot_frame(const XrXirVmDebugSnapshot *snapshot,
    uint32_t index, XrXirVmDebugFrameInfo *output, XrXirValue *name);
/* Copies only a materialized, currently admitted typed value. Uninitialized,
 * consumed and non-value operands are rejected without changing output.
 * Mutable values retain ordinary language ownership, without a deep clone. */
XR_FUNC XrXirValueStatus xr_xir_vm_debug_snapshot_value(const XrXirVmDebugSnapshot *snapshot,
    uint32_t frame, uint32_t value, XrXirValue *output);
/* Copies a Cell payload captured with the current Instance read authority.
 * No authority is inferred from the escaped Cell handle. A Cell that could not
 * be read at capture time, or a non-Cell operand, returns UNSUPPORTED. */
XR_FUNC XrXirValueStatus xr_xir_vm_debug_snapshot_cell_value(const XrXirVmDebugSnapshot *snapshot,
    uint32_t frame, uint32_t value, XrXirValue *output);
/* Copies the concrete enum owned by a first-chance throw snapshot. Other stops
 * return UNSUPPORTED and preserve empty output. The copy may escape the stop. */
XR_FUNC XrXirValueStatus xr_xir_vm_debug_snapshot_exception(const XrXirVmDebugSnapshot *, XrXirValue *);
/* Read-only member permission from the captured authentic caller. The value
 * must use that frame's retained arena. This grants no execution, mutable root,
 * loan or Instance authority and survives Program/Instance disposal. */
XR_FUNC XrXirValueStatus xr_xir_vm_debug_snapshot_member(const XrXirVmDebugSnapshot *,
    uint32_t frame,const XrXirValue *,uint32_t field,bool *allowed);
/* A nested nominal type name is visible only in the captured caller's scope.
 * The owning value must match the retained frame arena; no field is read. */
XR_FUNC XrXirValueStatus xr_xir_vm_debug_snapshot_type_visible(const XrXirVmDebugSnapshot *,
    uint32_t frame,const XrXirValue *,XrXirType,bool *allowed);
#endif // XXIR_VM_DEBUG_H
