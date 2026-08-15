/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_vm_debug_control.h - TargetPlan-driven typed debugger control
 *
 * KEY CONCEPT:
 *   Breakpoints and stops name immutable TargetPlan debug facts.  A stop
 *   snapshot owns copied bytes and numeric identities only for the duration
 *   of its callback; no frame, plan, generation, or slot pointer escapes.
 */

#ifndef XR_VM_DEBUG_CONTROL_H
#define XR_VM_DEBUG_CONTROL_H

#include "xr_vm_trace.h"

#define XR_VM_DEBUG_CONTROL_SCHEMA_VERSION UINT32_C(1)
#define XR_VM_DEBUG_MAX_STACK_DEPTH UINT32_C(256)
#define XR_VM_DEBUG_MAX_BREAKPOINTS UINT32_C(65536)
#define XR_VM_DEBUG_MAX_TRACKED_SLOTS UINT32_C(65536)
#define XR_VM_DEBUG_MAX_SNAPSHOT_BYTES UINT32_C(16777216)

typedef struct XrVmDebugPlan XrVmDebugPlan;
typedef struct XrVmDebugControl XrVmDebugControl;

typedef enum XrVmDebugBreakpointKind {
    XR_VM_DEBUG_BREAKPOINT_INVALID = 0,
    XR_VM_DEBUG_BREAKPOINT_SOURCE_SPAN,
    XR_VM_DEBUG_BREAKPOINT_SEMANTIC_OPERATION,
    XR_VM_DEBUG_BREAKPOINT_KIND_COUNT,
} XrVmDebugBreakpointKind;

typedef struct XrVmDebugBreakpointRequest {
    uint8_t kind;
    uint8_t reserved[3];
    XrStableId identity;
} XrVmDebugBreakpointRequest;

typedef enum XrVmDebugResumeCommand {
    XR_VM_DEBUG_RESUME_INVALID = 0,
    XR_VM_DEBUG_RESUME_CONTINUE,
    XR_VM_DEBUG_RESUME_STEP_INTO,
    XR_VM_DEBUG_RESUME_STEP_OVER,
    XR_VM_DEBUG_RESUME_STEP_OUT,
    XR_VM_DEBUG_RESUME_TERMINATE,
    XR_VM_DEBUG_RESUME_COMMAND_COUNT,
} XrVmDebugResumeCommand;

typedef enum XrVmDebugStopReason {
    XR_VM_DEBUG_STOP_BREAKPOINT = 0,
    XR_VM_DEBUG_STOP_STEP,
} XrVmDebugStopReason;

typedef struct XrVmDebugFrameSnapshot {
    XrVmTraceEvent instruction;
    uint32_t semantic_function;
} XrVmDebugFrameSnapshot;

typedef struct XrVmDebugLocalSnapshot {
    XrStableId identity;
    uint32_t frame;
    uint32_t slot;
    uint32_t semantic_value;
    uint32_t semantic_operation;
    uint32_t value_offset;
    uint32_t value_size;
    uint16_t alignment;
    uint16_t register_rep;
    uint16_t memory_rep;
    uint8_t role;
    uint8_t root_kind;
    uint8_t ownership;
    uint8_t reserved;
} XrVmDebugLocalSnapshot;

typedef struct XrVmDebugStop {
    uint32_t schema_version;
    uint8_t reason;
    uint8_t reserved[3];
    XrVmTraceEvent instruction;
    const XrVmDebugFrameSnapshot *frames;
    uint32_t frame_count;
    const XrVmDebugLocalSnapshot *locals;
    uint32_t local_count;
    const uint8_t *local_bytes;
    uint32_t local_bytes_size;
} XrVmDebugStop;

typedef bool (*XrVmDebugStopFn)(void *context, const XrVmDebugStop *stop,
                                XrVmDebugResumeCommand *resume);

typedef enum XrVmDebugControlStatus {
    XR_VM_DEBUG_CONTROL_OK = 0,
    XR_VM_DEBUG_CONTROL_INVALID_ARGUMENT,
    XR_VM_DEBUG_CONTROL_PLAN_NOT_VERIFIED,
    XR_VM_DEBUG_CONTROL_PLAN_IDENTITY_MISMATCH,
    XR_VM_DEBUG_CONTROL_BREAKPOINT_DUPLICATE,
    XR_VM_DEBUG_CONTROL_BREAKPOINT_NOT_FOUND,
    XR_VM_DEBUG_CONTROL_ALLOCATION_FAILED,
    XR_VM_DEBUG_CONTROL_RESOURCE_LIMIT,
    XR_VM_DEBUG_CONTROL_ACTIVE,
} XrVmDebugControlStatus;

XR_FUNC XrVmDebugControlStatus xr_typed_debug_plan_create(
    const XrTargetPlan *verified_plan, const XrFingerprint *required_plan_fingerprint,
    const XrVmDebugBreakpointRequest *breakpoints, uint32_t breakpoint_count, XrVmDebugPlan **plan);
XR_FUNC void xr_typed_debug_plan_free(XrVmDebugPlan **plan);
XR_FUNC XrVmDebugControlStatus xr_typed_debug_control_create(const XrVmDebugPlan *plan,
                                                             XrVmDebugStopFn stop, void *context,
                                                             XrVmDebugControl **control);
XR_FUNC XrVmDebugControlStatus xr_typed_debug_control_arm(XrVmDebugControl *control,
                                                          XrVmDebugResumeCommand initial_command);
XR_FUNC XrVmDebugControlStatus xr_typed_debug_control_free(XrVmDebugControl **control);
XR_FUNC bool xr_typed_debug_control_matches_plan(const XrVmDebugControl *control,
                                                 XrFingerprint target_plan_fingerprint);

#endif  // XR_VM_DEBUG_CONTROL_H
