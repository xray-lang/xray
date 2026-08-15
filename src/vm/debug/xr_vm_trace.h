/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_vm_trace.h - Runtime-only typed TargetPlan trace contract
 *
 * KEY CONCEPT:
 *   Trace identities are numeric facts scoped by an exact TargetPlan
 *   fingerprint. A debug session copies generation identity bytes and never
 *   retains a plan, frame, or generation object.
 */

#ifndef XR_VM_TRACE_H
#define XR_VM_TRACE_H

#include "../../plan/target/xr_target_plan.h"
#include "../../../include/xray_runtime_generation.h"

#define XR_VM_TRACE_SCHEMA_VERSION UINT32_C(1)
#define XR_VM_TRACE_ID_NONE UINT32_MAX

typedef enum XrVmTraceEventKind {
    XR_VM_TRACE_FRAME_ENTER = 0,
    XR_VM_TRACE_BLOCK_ENTER,
    XR_VM_TRACE_INSTRUCTION,
    XR_VM_TRACE_CALL_ENTER,
    XR_VM_TRACE_CALL_RETURN,
    XR_VM_TRACE_ERROR,
    XR_VM_TRACE_FRAME_EXIT,
    XR_VM_TRACE_EVENT_KIND_COUNT,
} XrVmTraceEventKind;

typedef struct XrVmTraceEvent {
    uint32_t schema_version;
    uint8_t kind;
    uint8_t generation_identity_present;
    uint16_t opcode;
    uint64_t ordinal;
    XrFingerprint target_plan_fingerprint;
    XrFingerprint generation_fingerprint;
    uint64_t generation_number;
    uint32_t function;
    uint32_t related_function;
    uint32_t instruction;
    uint32_t block;
    uint32_t call;
    uint32_t frame;
    uint32_t parent_frame;
    uint32_t related_frame;
    uint32_t frame_depth;
    uint32_t status;
} XrVmTraceEvent;

typedef bool (*XrVmTraceEmitFn)(void *context,
                                const XrVmTraceEvent *event);

typedef struct XrVmTraceSink {
    XrVmTraceEmitFn emit;
    void *context;
} XrVmTraceSink;

typedef struct XrVmTraceBuffer {
    XrVmTraceEvent *events;
    size_t capacity;
    size_t count;
    bool capacity_exceeded;
} XrVmTraceBuffer;

typedef struct XrVmProfile XrVmProfile;

typedef struct XrVmDebugSession {
    uint32_t schema_version;
    uint8_t generation_identity_present;
    uint8_t reserved8[3];
    XrFingerprint target_plan_fingerprint;
    XrModuleGenerationIdentity generation_identity;
    XrVmTraceSink trace;
    XrVmProfile *profile;
} XrVmDebugSession;

typedef enum XrVmDebugSessionStatus {
    XR_VM_DEBUG_SESSION_OK = 0,
    XR_VM_DEBUG_SESSION_INVALID_ARGUMENT,
    XR_VM_DEBUG_SESSION_PLAN_IDENTITY_MISMATCH,
    XR_VM_DEBUG_SESSION_GENERATION_IDENTITY_INVALID,
} XrVmDebugSessionStatus;

XR_FUNC XrVmDebugSessionStatus xr_typed_debug_session_init(
    const XrFingerprint *target_plan_fingerprint,
    const XrModuleGenerationIdentity *generation_identity,
    const XrVmTraceSink *trace, XrVmProfile *profile,
    XrVmDebugSession *session);
XR_FUNC bool xr_typed_debug_session_matches_plan(
    const XrVmDebugSession *session,
    XrFingerprint target_plan_fingerprint);
XR_FUNC bool xr_typed_debug_emit(const XrVmDebugSession *session,
                              uint64_t ordinal, XrVmTraceEvent *event);

XR_FUNC bool xr_typed_trace_buffer_init(XrVmTraceBuffer *buffer,
                                     XrVmTraceEvent *storage,
                                     size_t capacity);
XR_FUNC XrVmTraceSink xr_typed_trace_buffer_sink(XrVmTraceBuffer *buffer);

#endif  // XR_VM_TRACE_H
