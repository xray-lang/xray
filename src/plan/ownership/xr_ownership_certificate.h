/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_ownership_certificate.h - Immutable ownership proof schema
 */

#ifndef XR_OWNERSHIP_CERTIFICATE_H
#define XR_OWNERSHIP_CERTIFICATE_H

#include "../semantic/xr_semantic_ids.h"
#include "../../shared/xr_ownership_event.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct XrSemanticPlan XrSemanticPlan;
typedef struct XrOwnershipCertificate XrOwnershipCertificate;

typedef struct XrOwnershipOwnerRecord {
    XrStableId id;
    const char *canonical_key;
    uint32_t function;
    uint32_t origin_value;
    uint8_t initial_state;
    uint8_t exit_state;
    uint8_t return_provenance;
    uint8_t flags;
} XrOwnershipOwnerRecord;

typedef struct XrOwnershipEventRecord {
    XrStableId id;
    const char *canonical_key;
    uint32_t owner;
    uint32_t operation;
    uint32_t block;
    uint32_t successor; /* edge event, XR_SEMANTIC_INDEX_NONE for block event */
    int16_t logical_delta;
    uint8_t kind;
    uint8_t state_after;
    uint8_t program_point;
    uint8_t reserved;
} XrOwnershipEventRecord;

typedef struct XrOwnershipEdgeStateRecord {
    uint32_t owner;
    uint32_t block;
    uint32_t successor; /* XR_SEMANTIC_INDEX_NONE for a terminal block */
    int32_t entry_balance;
    int32_t exit_balance;
    uint8_t entry_state;
    uint8_t exit_state;
    uint16_t flags;
} XrOwnershipEdgeStateRecord;

enum {
    XR_OWN_EDGE_OUT_OF_SCOPE = UINT16_C(1),
    XR_OWN_EDGE_OWNER_FRONTIER = UINT16_C(2),
};

typedef struct XrOwnershipLoopInvariantRecord {
    XrStableId id;
    const char *canonical_key;
    uint32_t owner;
    uint32_t header;
    uint32_t backedge;
    int32_t balance;
    uint8_t state;
    uint8_t reserved[3];
} XrOwnershipLoopInvariantRecord;

XR_FUNC void xr_ownership_certificate_free(XrOwnershipCertificate *certificate);
XR_FUNC XrFingerprint
xr_ownership_certificate_fingerprint(const XrOwnershipCertificate *certificate);
XR_FUNC size_t xr_ownership_certificate_owner_count(const XrOwnershipCertificate *certificate);
XR_FUNC size_t xr_ownership_certificate_event_count(const XrOwnershipCertificate *certificate);
XR_FUNC size_t xr_ownership_certificate_edge_state_count(const XrOwnershipCertificate *certificate);
XR_FUNC size_t
xr_ownership_certificate_loop_invariant_count(const XrOwnershipCertificate *certificate);
XR_FUNC const XrOwnershipOwnerRecord *
xr_ownership_certificate_owner(const XrOwnershipCertificate *certificate, uint32_t index);
XR_FUNC const XrOwnershipEventRecord *
xr_ownership_certificate_event(const XrOwnershipCertificate *certificate, uint32_t index);
XR_FUNC const XrOwnershipEdgeStateRecord *
xr_ownership_certificate_edge_state(const XrOwnershipCertificate *certificate, uint32_t index);
XR_FUNC const XrOwnershipLoopInvariantRecord *
xr_ownership_certificate_loop_invariant(const XrOwnershipCertificate *certificate, uint32_t index);

#endif  // XR_OWNERSHIP_CERTIFICATE_H
