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
    XR_XIR_VM_DEBUG_STEP_IN, XR_XIR_VM_DEBUG_STEP_OVER, XR_XIR_VM_DEBUG_STEP_OUT
} XrXirVmDebugMode;
typedef struct XrXirVmDebugFrameInfo {
    uint64_t id, epoch;
    uint32_t function, instruction, value_count;
} XrXirVmDebugFrameInfo;

/* Metadata uses this current Domain throughout the observer's lifetime.
 * Capacity is an explicit VM-frame bound, not a native-stack guarantee. One
 * observer admits one live activation at a time. State operations and queries
 * run on its sole control/drive owner between polls; only request_pause may
 * be called concurrently. Owned snapshot references are atomic. */
XR_FUNC XrXirValueStatus xr_xir_vm_debug_new(XrXirDomain *domain, uint32_t capacity,
    XrXirVmDebug **output);
XR_FUNC void xr_xir_vm_debug_drop(XrXirVmDebug *debug);
/* Success transfers the artifact through the ordinary Program receiving gate
 * and retains the observer. Failure preserves artifact and output. */
XR_FUNC XrXirStatus xr_xir_compile_vm_debug_program_take(XrXirArtifact **artifact,
    XrXirVmDebug *debug, XrXirProgram **output);
/* Construct only the Instance of this observer's authenticated Program.
 * One target is bound; foreign Programs and a second target are rejected. */
XR_FUNC XrXirCallStatus xr_xir_vm_debug_instance_new(XrXirVmDebug *debug,
    XrXirProgram *program, const XrXirInstanceConfig *config, XrXirInstance **output);
/* Locations are verified function/instruction ordinals. They are not source
 * lines. Changes require a stopped observer or no live execution frames. */
XR_FUNC XrXirValueStatus xr_xir_vm_debug_breakpoint(XrXirVmDebug *debug,
    uint32_t function, uint32_t instruction, bool enabled);
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
/* Returns an independent owned snapshot. It survives resume, cancellation,
 * Program disposal and observer disposal. No frame pointer is exposed. */
XR_FUNC XrXirValueStatus xr_xir_vm_debug_snapshot(XrXirVmDebug *debug,
    XrXirVmDebugSnapshot **output);
XR_FUNC void xr_xir_vm_debug_snapshot_drop(XrXirVmDebugSnapshot *snapshot);
XR_FUNC uint32_t xr_xir_vm_debug_snapshot_depth(const XrXirVmDebugSnapshot *snapshot);
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
#endif // XXIR_VM_DEBUG_H
