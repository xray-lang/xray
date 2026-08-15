/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_typed_lifecycle_audit.h - Managed cleanup certificate adapter
 *
 * KEY CONCEPT:
 *   This diagnostic-only adapter independently joins a verified TargetPlan
 *   lifecycle partition to its Semantic ownership certificate. The typed VM
 *   reports slot observations; the adapter, never the carrier tag, rebuilds
 *   canonical owner and release-event identities for the audit oracle.
 */

#ifndef XR_TYPED_LIFECYCLE_AUDIT_H
#define XR_TYPED_LIFECYCLE_AUDIT_H

#include "runtime/ownership/xr_ownership_audit.h"
#include "vm/xr_typed_lifecycle.h"

#define XR_TYPED_LIFECYCLE_AUDIT_SCHEMA_VERSION UINT32_C(1)

typedef struct XrSemanticPlan XrSemanticPlan;
typedef struct XrTypedLifecycleAuditContext XrTypedLifecycleAuditContext;

typedef enum XrTypedLifecycleAuditStatus {
    XR_TYPED_LIFECYCLE_AUDIT_OK = 0,
    XR_TYPED_LIFECYCLE_AUDIT_INVALID_ARGUMENT,
    XR_TYPED_LIFECYCLE_AUDIT_PLAN_MISMATCH,
    XR_TYPED_LIFECYCLE_AUDIT_CERTIFICATE_MISMATCH,
    XR_TYPED_LIFECYCLE_AUDIT_ORACLE_REJECTED,
    XR_TYPED_LIFECYCLE_AUDIT_REENTRANT,
    XR_TYPED_LIFECYCLE_AUDIT_ACTIVATION_EXHAUSTED,
    XR_TYPED_LIFECYCLE_AUDIT_OUT_OF_MEMORY,
} XrTypedLifecycleAuditStatus;

typedef struct XrTypedLifecycleAuditConfig {
    const XrSemanticPlan *verified_semantic_plan;
    const XrTargetPlan *verified_target_plan;
    const XrFingerprint *required_target_plan_fingerprint;
    XrOwnershipAudit *oracle;
    XrStableId invocation_id;
    uint64_t first_activation_epoch;
    uint32_t function;
    bool record_physical_rc;
} XrTypedLifecycleAuditConfig;

XR_FUNC XrTypedLifecycleAuditContext *
xr_typed_lifecycle_audit_create(const XrTypedLifecycleAuditConfig *config,
                                XrTypedLifecycleAuditStatus *status);
XR_FUNC void xr_typed_lifecycle_audit_destroy(XrTypedLifecycleAuditContext *context);

/* This function is directly compatible with XrTypedLifecycleObserver. */
XR_FUNC XrTypedLifecycleStatus xr_typed_lifecycle_audit_observe(void *context,
                                                                const XrTypedLifecycleEvent *event);

XR_FUNC XrTypedLifecycleAuditStatus
xr_typed_lifecycle_audit_status(const XrTypedLifecycleAuditContext *context);
XR_FUNC XrOwnershipAuditStatus
xr_typed_lifecycle_audit_oracle_status(const XrTypedLifecycleAuditContext *context);
XR_FUNC const char *xr_typed_lifecycle_audit_status_name(XrTypedLifecycleAuditStatus status);

#endif  // XR_TYPED_LIFECYCLE_AUDIT_H
