/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_ownership_audit.h - Executed-path ownership lifecycle oracle
 *
 * KEY CONCEPT:
 *   Owner and transition manifests are loaded before execution. Recording is bounded,
 *   allocation-free, callback-free, and fail-closed so instrumentation cannot
 *   reorder destructors or turn missing evidence into a successful run.
 */

#ifndef XR_OWNERSHIP_AUDIT_H
#define XR_OWNERSHIP_AUDIT_H

#include "../abi/xr_runtime_descriptor.h"
#include "../../shared/xr_ownership_event.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct XrOwnershipAudit XrOwnershipAudit;

typedef enum XrOwnershipAuditStatus {
    XR_OWN_AUDIT_OK = 0,
    XR_OWN_AUDIT_INVALID_ARGUMENT = 1,
    XR_OWN_AUDIT_OUT_OF_MEMORY = 2,
    XR_OWN_AUDIT_CAPACITY_EXCEEDED = 3,
    XR_OWN_AUDIT_REENTRANT = 4,
    XR_OWN_AUDIT_ALREADY_FINISHED = 5,
    XR_OWN_AUDIT_DUPLICATE_OWNER = 6,
    XR_OWN_AUDIT_DUPLICATE_TRANSITION = 7,
    XR_OWN_AUDIT_DUPLICATE_INSTANCE = 8,
    XR_OWN_AUDIT_UNKNOWN_OWNER = 9,
    XR_OWN_AUDIT_UNKNOWN_TRANSITION = 10,
    XR_OWN_AUDIT_INVALID_DESCRIPTOR = 11,
    XR_OWN_AUDIT_IDENTITY_MISMATCH = 12,
    XR_OWN_AUDIT_ORIGIN_MISMATCH = 13,
    XR_OWN_AUDIT_DOMAIN_MISMATCH = 14,
    XR_OWN_AUDIT_INVALID_TRANSITION = 15,
    XR_OWN_AUDIT_LOAN_MISMATCH = 16,
    XR_OWN_AUDIT_PHYSICAL_RC_MISMATCH = 17,
    XR_OWN_AUDIT_EXIT_MISMATCH = 18,
    XR_OWN_AUDIT_DESTRUCTOR_MISMATCH = 19,
    XR_OWN_AUDIT_GENERATION_PIN_MISSING = 20,
    XR_OWN_AUDIT_GENERATION_PIN_MISMATCH = 21,
    XR_OWN_AUDIT_INCOMPLETE = 22,
    XR_OWN_AUDIT_TEARDOWN_MISMATCH = 23,
    XR_OWN_AUDIT_FINALIZE_MISMATCH = 24,
    XR_OWN_AUDIT_RECLAIM_MISMATCH = 25,
} XrOwnershipAuditStatus;

typedef enum XrOwnershipAuditExpectationFlags {
    XR_OWN_AUDIT_REQUIRE_GENERATION_PIN = UINT32_C(1) << 0,
    XR_OWN_AUDIT_TRACK_ALLOCATION_LIFECYCLE = UINT32_C(1) << 1,
} XrOwnershipAuditExpectationFlags;

typedef enum XrOwnershipAuditTransitionFlags {
    XR_OWN_AUDIT_TRANSITION_TERMINAL = UINT32_C(1) << 0,
    XR_OWN_AUDIT_TRANSITION_OPENS_INSTANCE = UINT32_C(1) << 1,
    XR_OWN_AUDIT_TRANSITION_CHANGES_DOMAIN = UINT32_C(1) << 2,
} XrOwnershipAuditTransitionFlags;

typedef enum XrOwnershipAuditEventFlags {
    XR_OWN_AUDIT_EVENT_PHYSICAL_RC = UINT32_C(1) << 0,
} XrOwnershipAuditEventFlags;

typedef enum XrOwnershipAuditPhysicalRcMode {
    XR_OWN_AUDIT_RC_NONE = 0,
    XR_OWN_AUDIT_RC_LOCAL,
    XR_OWN_AUDIT_RC_SHARED,
    XR_OWN_AUDIT_RC_STICKY,
} XrOwnershipAuditPhysicalRcMode;

typedef enum XrOwnershipAuditLifecycleKind {
    XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN = 0,
    XR_OWN_AUDIT_LIFECYCLE_BEGIN_FINALIZE = 1,
    XR_OWN_AUDIT_LIFECYCLE_END_FINALIZE = 2,
    XR_OWN_AUDIT_LIFECYCLE_RECLAIM = 3,
    XR_OWN_AUDIT_LIFECYCLE_END_TEARDOWN = 4,
    XR_OWN_AUDIT_LIFECYCLE_KIND_COUNT = 5,
} XrOwnershipAuditLifecycleKind;

typedef enum XrOwnershipAuditAllocationState {
    XR_OWN_AUDIT_ALLOCATION_UNKNOWN = 0,
    XR_OWN_AUDIT_ALLOCATION_UNTRACKED = 1,
    XR_OWN_AUDIT_ALLOCATION_LIVE = 2,
    XR_OWN_AUDIT_ALLOCATION_FINALIZING = 3,
    XR_OWN_AUDIT_ALLOCATION_FINALIZED = 4,
    XR_OWN_AUDIT_ALLOCATION_RECLAIMED = 5,
} XrOwnershipAuditAllocationState;

typedef struct XrOwnershipAuditConfig {
    size_t max_owner_manifests;
    size_t max_transition_manifests;
    size_t max_dynamic_instances;
    size_t max_events;
    size_t max_loans;
    size_t max_generations;
    size_t max_lifecycle_manifests;
    size_t max_lifecycle_events;
    size_t max_teardown_domains;
} XrOwnershipAuditConfig;

/* A static owner can activate repeatedly across invocations and loop
 * iterations. The complete key names one executed owner activation. */
typedef struct XrOwnershipAuditObjectKey {
    XrStableId owner_id;
    XrStableId invocation_id;
    uint64_t activation_epoch;
} XrOwnershipAuditObjectKey;

typedef struct XrOwnershipAuditOwnerManifest {
    XrStableId owner_id;
    const XrRuntimeLayoutDescriptor *descriptor;
    const XrRuntimeExtentDescriptor *extent;
    XrStableId allocation_site_id;
    XrStableId frame_id;
    XrStableId generation_id;
    XrStableId destructor_id;
    XrFingerprint premise_fingerprint;
    XrStableId initial_domain_contract_id;
    int32_t initial_logical_balance;
    uint32_t flags;
    uint8_t initial_state;
    uint8_t initial_semantic_domain;
    uint8_t initial_materialization;
} XrOwnershipAuditOwnerManifest;

typedef struct XrOwnershipAuditTransitionManifest {
    XrStableId transition_id;
    XrStableId owner_id;
    XrStableId operation_id;
    XrStableId exit_id;
    XrStableId generation_id;
    XrStableId next_domain_contract_id;
    uint32_t state_before_mask;
    uint32_t flags;
    int16_t logical_delta;
    uint8_t kind;
    uint8_t state_after;
    uint8_t program_point;
    uint8_t next_semantic_domain;
    uint8_t next_materialization;
    uint8_t physical_rc_mode;
} XrOwnershipAuditTransitionManifest;

typedef struct XrOwnershipAuditEvent {
    XrOwnershipAuditObjectKey object;
    XrStableId transition_id;
    XrStableId layout_id;
    XrStableId allocation_site_id;
    XrStableId operation_id;
    XrStableId exit_id;
    XrStableId frame_id;
    XrStableId generation_id;
    XrStableId destructor_id;
    XrStableId loan_id;
    XrFingerprint premise_fingerprint;
    XrRuntimeDomainIdentity domain;
    XrRuntimeDomainIdentity next_domain;
    int32_t physical_rc_before;
    int32_t physical_rc_after;
    uint32_t flags;
    uint8_t kind;
    uint8_t program_point;
    uint8_t physical_rc_mode;
} XrOwnershipAuditEvent;

/* Lifecycle manifests are independent static obligations. Domain-level
 * manifests use a zero owner/destructor; object-level manifests use zero
 * domain class fields and bind the static owner plus destructor identity. */
typedef struct XrOwnershipAuditLifecycleManifest {
    XrStableId transition_id;
    XrStableId owner_id;
    XrStableId operation_id;
    XrStableId destructor_id;
    XrStableId domain_contract_id;
    uint8_t kind;
    uint8_t semantic_domain;
    uint8_t materialization;
} XrOwnershipAuditLifecycleManifest;

typedef struct XrOwnershipAuditLifecycleEvent {
    XrOwnershipAuditObjectKey object;
    XrStableId transition_id;
    XrStableId operation_id;
    XrStableId destructor_id;
    XrRuntimeDomainIdentity domain;
    uint8_t kind;
} XrOwnershipAuditLifecycleEvent;

XR_FUNC XrOwnershipAudit *xr_ownership_audit_create(XrOwnershipAuditConfig config,
                                                    XrOwnershipAuditStatus *status);
XR_FUNC void xr_ownership_audit_destroy(XrOwnershipAudit *audit);
XR_FUNC XrOwnershipAuditStatus xr_ownership_audit_register_owner(
    XrOwnershipAudit *audit, const XrOwnershipAuditOwnerManifest *manifest);
XR_FUNC XrOwnershipAuditStatus xr_ownership_audit_register_transition(
    XrOwnershipAudit *audit, const XrOwnershipAuditTransitionManifest *manifest);
XR_FUNC XrOwnershipAuditStatus xr_ownership_audit_register_lifecycle(
    XrOwnershipAudit *audit, const XrOwnershipAuditLifecycleManifest *manifest);
XR_FUNC XrOwnershipAuditStatus xr_ownership_audit_record(XrOwnershipAudit *audit,
                                                        const XrOwnershipAuditEvent *event);
XR_FUNC XrOwnershipAuditStatus xr_ownership_audit_record_lifecycle(
    XrOwnershipAudit *audit, const XrOwnershipAuditLifecycleEvent *event);
XR_FUNC XrOwnershipAuditStatus xr_ownership_audit_finish(XrOwnershipAudit *audit);

/* Registration copies every descriptor fact used by recording. All accessors
 * and destroy require quiescent recorders; concurrent entry poisons the audit
 * but does not turn the recorder into a work queue. */
XR_FUNC XrOwnershipAuditStatus xr_ownership_audit_status(const XrOwnershipAudit *audit);
XR_FUNC const char *xr_ownership_audit_status_name(XrOwnershipAuditStatus status);
XR_FUNC size_t xr_ownership_audit_event_count(const XrOwnershipAudit *audit);
XR_FUNC const XrOwnershipAuditEvent *xr_ownership_audit_event(const XrOwnershipAudit *audit,
                                                             size_t index);
XR_FUNC const XrOwnershipAuditEvent *xr_ownership_audit_failed_event(
    const XrOwnershipAudit *audit);
XR_FUNC size_t xr_ownership_audit_lifecycle_event_count(const XrOwnershipAudit *audit);
XR_FUNC const XrOwnershipAuditLifecycleEvent *xr_ownership_audit_lifecycle_event(
    const XrOwnershipAudit *audit, size_t index);
XR_FUNC const XrOwnershipAuditLifecycleEvent *xr_ownership_audit_failed_lifecycle_event(
    const XrOwnershipAudit *audit);
XR_FUNC XrOwnershipAuditAllocationState xr_ownership_audit_allocation_state(
    const XrOwnershipAudit *audit, XrOwnershipAuditObjectKey object);
XR_FUNC size_t xr_ownership_audit_allocation_count(const XrOwnershipAudit *audit);
XR_FUNC const char *xr_ownership_audit_evidence_scope(void);

#endif  // XR_OWNERSHIP_AUDIT_H
