/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_ownership_certificate_internal.h - Ownership certificate construction state
 */

#ifndef XR_OWNERSHIP_CERTIFICATE_INTERNAL_H
#define XR_OWNERSHIP_CERTIFICATE_INTERNAL_H

#include "xr_ownership_certificate.h"

struct XrOwnershipCertificate {
    uint32_t schema;
    bool frozen;
    XrFingerprint semantic_fingerprint;
    XrFingerprint fingerprint;
    XrOwnershipOwnerRecord *owners;
    uint32_t owner_count;
    uint32_t owner_capacity;
    XrOwnershipEventRecord *events;
    uint32_t event_count;
    uint32_t event_capacity;
    XrOwnershipEdgeStateRecord *edge_states;
    uint32_t edge_state_count;
    uint32_t edge_state_capacity;
};

#endif  // XR_OWNERSHIP_CERTIFICATE_INTERNAL_H
