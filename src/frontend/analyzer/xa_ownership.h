/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_ownership.h - Canonical source ownership, alias, loan, and move evidence.
 *
 * These axes deliberately remain independent.  Binding availability is a CFG
 * fact, root aliasing is an object-graph fact, capability controls permitted
 * operations, and loans are bounded place/provenance facts.
 */

#ifndef XA_OWNERSHIP_H
#define XA_OWNERSHIP_H

#include "xa_effect_db.h"
#include "../../base/xstorage.h"
#include <stdbool.h>
#include <stdint.h>

typedef uint32_t XaRootId;
typedef uint32_t XaLoanId;
typedef uint32_t XaCfgPoint;
typedef uint32_t XaProvenanceId;
typedef uint32_t XaOwnershipProofId;
typedef uint32_t XaStoragePlanId;
typedef uint32_t XaTransferPlanId;
typedef uint32_t XaAllocationSiteId;
typedef uint32_t XaMonomorphizationId;
typedef uint32_t XaAllocationContextId;
typedef uint32_t XaOwnershipDomainId;
typedef uint32_t XaDropPlanId;

typedef enum XaBindingUseState {
    XA_BINDING_UNINITIALIZED = 0,
    XA_BINDING_LIVE,
    XA_BINDING_MOVED,
    XA_BINDING_MAYBE_MOVED,
    XA_BINDING_UNKNOWN,
} XaBindingUseState;

/* Aliasing state of an ownership root.
 *
 * LOCAL_ALIASED is recoverable: every alias is a named local binding, so the
 * analyzer can prove the last one died and restore UNIQUE.  ESCAPED is not:
 * the reference was written into a heap graph (container element, object
 * field, enum payload), and function-local analysis cannot observe when that
 * slot is overwritten.  ALIAS_UNKNOWN is the fail-closed answer for values
 * whose provenance the analyzer never established. */
typedef enum XaRootAliasState {
    XA_ROOT_UNIQUE = 0,
    XA_ROOT_LOCAL_ALIASED,
    XA_ROOT_ESCAPED,
    XA_ROOT_ALIAS_UNKNOWN,
} XaRootAliasState;

typedef enum XaBindingMutability {
    XA_BINDING_REBINDABLE = 0,
    XA_BINDING_STABLE,
} XaBindingMutability;

typedef enum XaValueCapability {
    XA_CAP_MUTABLE = 0,
    XA_CAP_CONST,
    XA_CAP_SYNC_INTERIOR_MUTABLE,
    XA_CAP_UNKNOWN,
} XaValueCapability;

/* Loan forms, ordered from the most structured place borrow to the least.
 *
 * CAPTURE is the closure form: a closure value holds the whole root by
 * reference, so the loan has no place projection and lives as long as the
 * closure binding does.  It is a loan and not an alias because the borrower
 * is a value with its own liveness, exactly like a Slice view. */
typedef enum XaLoanKind {
    XA_LOAN_READ = 0,
    XA_LOAN_WRITE,
    XA_LOAN_RAW_READ,
    XA_LOAN_RAW_WRITE,
    XA_LOAN_CAPTURE,
    XA_LOAN_CLEANUP_READ,
} XaLoanKind;

#define XA_LOAN_KIND_IS_RAW(kind) ((kind) == XA_LOAN_RAW_READ || (kind) == XA_LOAN_RAW_WRITE)

typedef enum XaPlaceProjectionKind {
    XA_PLACE_ROOT = 0,
    XA_PLACE_FIELD,
    XA_PLACE_INDEX,
    XA_PLACE_RANGE,
    XA_PLACE_DEREF,
    XA_PLACE_UNKNOWN,
} XaPlaceProjectionKind;

#define XA_PLACE_PATH_MAX 8

typedef struct XaPlaceProjection {
    XaPlaceProjectionKind kind;
    uint32_t key;
} XaPlaceProjection;

typedef struct XaPlacePath {
    XaRootId root;
    XaPlaceProjection projections[XA_PLACE_PATH_MAX];
    uint8_t count;
    bool precise;
} XaPlacePath;

typedef struct XaLoan {
    XaLoanId id;
    XaRootId root;
    XaPlacePath path;
    XaLoanKind kind;
    XaCfgPoint begin;
    XaCfgPoint last_use;
    XaProvenanceId provenance;
    uint32_t borrower_symbol_id;
    uint32_t owner_symbol_id;
    uint16_t loop_depth_at_creation;
    struct XaLoan *next;
} XaLoan;

enum {
    XA_OWNERSHIP_EV_BINDING_LIVE = 1u << 0,
    XA_OWNERSHIP_EV_ROOT_UNIQUE = 1u << 1,
    XA_OWNERSHIP_EV_LOAN_FREE = 1u << 2,
    XA_OWNERSHIP_EV_ALIAS_FREE = 1u << 3,
    XA_OWNERSHIP_EV_ESCAPE_FREE = 1u << 4,
    XA_OWNERSHIP_EV_CAPABILITY = 1u << 5,
    XA_OWNERSHIP_EV_CFG_CONSISTENT = 1u << 6,
    XA_OWNERSHIP_EV_STORAGE = 1u << 7,
    XA_OWNERSHIP_EV_TRANSFER = 1u << 8,
};

typedef struct XaOwnershipCandidateProof {
    XaOwnershipProofId id;
    XaRootId root;
    uint32_t source_symbol_id;
    XaValueCapability capability;
    uint32_t evidence;
    XaUnknownReasonSet unknown_reasons;
    bool complete;
} XaOwnershipCandidateProof;

typedef struct XaFinalMoveProof {
    XaOwnershipProofId id;
    XaOwnershipProofId candidate_id;
    XaStoragePlanId storage_plan_id;
    XaTransferPlanId transfer_plan_id;
    XaRootId root;
    XaValueCapability source_capability;
    XaValueCapability target_capability;
    uint32_t consume_line;
    uint32_t consume_column;
    uint32_t evidence;
    XaUnknownReasonSet unknown_reasons;
    bool complete;
} XaFinalMoveProof;

typedef struct XaAllocationInstanceKey {
    XaAllocationSiteId source_site;
    XaMonomorphizationId mono;
    XaAllocationContextId context;
} XaAllocationInstanceKey;

typedef struct XaAllocationInstancePlan {
    XaStoragePlanId id;
    XaAllocationInstanceKey key;
    XrSemanticStorageDomain domain;
    XrBackendMaterialization materialization;
    XaOwnershipDomainId ownership_domain;
    XaValueCapability capability;
    uint32_t lifetime_class;
    XaDropPlanId drop_plan;
    uint32_t evidence;
    XaUnknownReasonSet unknown_reasons;
    bool complete;
} XaAllocationInstancePlan;

typedef enum XaCapabilityConversionAction {
    XA_CONVERT_FRESH = 0,
    XA_CONVERT_MOVE,
    XA_CONVERT_COPY,
    XA_CONVERT_CONST_RETAIN,
    XA_CONVERT_REJECT,
} XaCapabilityConversionAction;

typedef struct XaCapabilityConversionPlan {
    XaCapabilityConversionAction action;
    XaValueCapability source_capability;
    XaValueCapability target_capability;
    XaOwnershipProofId proof_id;
    XaStoragePlanId storage_plan_id;
    uint8_t cost_class; /* 0 = O(1), 1 = O(n), 2 = rejected/unknown */
    XaUnknownReasonSet unknown_reasons;
    bool complete;
} XaCapabilityConversionPlan;

#endif /* XA_OWNERSHIP_H */
