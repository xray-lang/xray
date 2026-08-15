/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_vm_materialize.h - Runtime-only trace fact materialization
 *
 * KEY CONCEPT:
 *   Materialization copies only facts present in the verified TargetPlan. A
 *   missing schema relation is reported explicitly and is never reconstructed
 *   from source names, SemanticPlan, or legacy bytecode.
 */

#ifndef XR_VM_MATERIALIZE_H
#define XR_VM_MATERIALIZE_H

#include "xr_vm_trace.h"

typedef enum XrVmMaterializeStatus {
    XR_VM_MATERIALIZE_OK = 0,
    XR_VM_MATERIALIZE_INVALID_ARGUMENT,
    XR_VM_MATERIALIZE_PLAN_NOT_VERIFIED,
    XR_VM_MATERIALIZE_PLAN_IDENTITY_MISMATCH,
    XR_VM_MATERIALIZE_EVENT_INVALID,
} XrVmMaterializeStatus;

typedef struct XrVmMaterializedSlot {
    uint8_t availability;
    uint8_t role;
    uint8_t root_kind;
    uint8_t ownership;
    uint32_t id;
    XrStableId identity;
    uint32_t offset;
    uint32_t size;
    uint16_t alignment;
    uint16_t register_rep;
    uint16_t memory_rep;
    uint16_t reserved;
} XrVmMaterializedSlot;

typedef struct XrVmMaterializedEvent {
    XrVmTraceEvent event;
    uint8_t function_identity;
    uint8_t generation_identity;
    uint8_t source_span;
    uint8_t owner_identity;
    uint8_t layout_identity;
    uint8_t reserved8[3];
    uint32_t semantic_function;
    uint32_t semantic_operation;
    uint32_t coroutine_state;
    uint32_t source_start_line;
    uint32_t source_start_column;
    uint32_t source_end_line;
    uint32_t source_end_column;
    XrStableId semantic_operation_identity;
    XrStableId source_span_identity;
    XrStableId owner_stable_identity;
    XrStableId coroutine_state_stable_identity;
    XrFingerprint layout_fingerprint;
    XrVmMaterializedSlot result;
    XrVmMaterializedSlot operands[2];
    uint8_t call_identity;
    uint8_t reserved_call[3];
    XrStableId call_stable_identity;
    uint32_t callee_function;
} XrVmMaterializedEvent;

XR_FUNC XrVmMaterializeStatus xr_typed_materialize_event(
    const XrTargetPlan *verified_plan,
    const XrFingerprint *required_plan_fingerprint,
    const XrVmTraceEvent *event, XrVmMaterializedEvent *materialized);

#endif  // XR_VM_MATERIALIZE_H
