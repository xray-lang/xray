/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_typed_lifecycle.h - TargetPlan-driven managed owner cleanup
 *
 * KEY CONCEPT:
 *   Cleanup ownership comes only from verified TargetPlan root and RELEASE
 *   rows. Dynamic carrier metadata validates representation; it never selects
 *   whether an object is owned or whether it must be released.
 */

#ifndef XR_TYPED_LIFECYCLE_H
#define XR_TYPED_LIFECYCLE_H

#include "xr_typed_frame.h"
#include "../runtime/abi/xr_runtime_contract.h"
#include "../runtime/abi/xr_runtime_object_header.h"
#include "../runtime/abi/xr_runtime_string_object.h"

#define XR_TYPED_LIFECYCLE_CONTEXT_SCHEMA_VERSION UINT32_C(1)

typedef enum XrTypedLifecycleStatus {
    XR_TYPED_LIFECYCLE_OK = 0,
    XR_TYPED_LIFECYCLE_INVALID_ARGUMENT,
    XR_TYPED_LIFECYCLE_PLAN_NOT_VERIFIED,
    XR_TYPED_LIFECYCLE_PLAN_IDENTITY_MISMATCH,
    XR_TYPED_LIFECYCLE_TARGET_PROFILE_MISMATCH,
    XR_TYPED_LIFECYCLE_CONTRACT_UNAVAILABLE,
    XR_TYPED_LIFECYCLE_FRAME_ERROR,
    XR_TYPED_LIFECYCLE_CARRIER_INVALID,
    XR_TYPED_LIFECYCLE_RELEASE_FAILED,
    XR_TYPED_LIFECYCLE_ALREADY_EXECUTED,
} XrTypedLifecycleStatus;

typedef enum XrTypedLifecycleExit {
    XR_TYPED_LIFECYCLE_EXIT_NORMAL = 0,
    XR_TYPED_LIFECYCLE_EXIT_ERROR,
    XR_TYPED_LIFECYCLE_EXIT_CANCEL,
    XR_TYPED_LIFECYCLE_EXIT_RETURN,
    XR_TYPED_LIFECYCLE_EXIT_COUNT,
} XrTypedLifecycleExit;

/* The allocation owner resolves an opaque carrier address before the VM
 * dereferences it and reclaims a last-release object. These callbacks do not
 * select cleanup semantics: TargetPlan has already selected RELEASE. */
typedef XrRuntimeObjectHeader *(*XrTypedLifecycleObjectResolver)(
    void *context, uintptr_t address);
typedef void (*XrTypedLifecycleObjectReclaimer)(
    void *context, XrRuntimeObjectHeader *header);

typedef struct XrTypedLifecycleEvent {
    XrFingerprint plan_fingerprint;
    XrStableId slot_identity;
    uint32_t function;
    uint32_t semantic_operation;
    uint32_t slot;
    int32_t physical_rc_before;
    int32_t physical_rc_after;
    uint8_t action;
    uint8_t exit_kind;
    uint8_t physical_last_release;
    uint8_t reserved;
} XrTypedLifecycleEvent;

/* Emitted only after physical release and any last-release reclamation have
 * succeeded. The stable ID identifies the verified TargetPlan slot; this
 * signal is not an ownership-certificate oracle. */
typedef void (*XrTypedLifecycleObserver)(
    void *context, const XrTypedLifecycleEvent *event);

typedef struct XrTypedLifecycleBindings {
    XrTypedLifecycleObjectResolver resolve_object;
    XrTypedLifecycleObjectReclaimer reclaim_object;
    void *allocation_context;
    XrTypedLifecycleObserver observer;
    void *observer_context;
} XrTypedLifecycleBindings;

typedef struct XrTypedLifecycleOwnerContract XrTypedLifecycleOwnerContract;

typedef struct XrTypedLifecycleContext {
    uint32_t schema_version;
    uint32_t function;
    XrTargetPlan *plan;
    XrFingerprint plan_fingerprint;
    XrRuntimeDynamicValueAbi dynamic_value;
    XrTypedLifecycleOwnerContract *owners;
    uint32_t owner_count;
    XrTypedLifecycleObjectResolver resolve_object;
    XrTypedLifecycleObjectReclaimer reclaim_object;
    void *allocation_context;
    XrTypedLifecycleObserver observer;
    void *observer_context;
} XrTypedLifecycleContext;

XR_FUNC XrTypedLifecycleStatus xr_typed_lifecycle_context_init(
    const XrTargetPlan *verified_plan,
    const XrFingerprint *required_plan_fingerprint, uint32_t function,
    const XrTypedLifecycleBindings *bindings,
    XrTypedLifecycleContext *context);
XR_FUNC void xr_typed_lifecycle_context_dispose(
    XrTypedLifecycleContext *context);
XR_FUNC XrTypedLifecycleStatus xr_typed_lifecycle_execute(
    XrTypedLifecycleContext *context, XrTypedFrame *frame,
    uint32_t semantic_operation, XrTypedLifecycleExit exit_kind,
    uint32_t *executed);

#endif  // XR_TYPED_LIFECYCLE_H
