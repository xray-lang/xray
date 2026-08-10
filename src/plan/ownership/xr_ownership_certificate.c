/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_ownership_certificate.c - Immutable ownership proof storage
 */

#include "xr_ownership_certificate_internal.h"
#include "../../base/xmalloc.h"

void xr_ownership_certificate_free(XrOwnershipCertificate *certificate) {
    if (!certificate)
        return;
    for (uint32_t i = 0; i < certificate->owner_count; i++)
        xr_free((void *) certificate->owners[i].canonical_key);
    xr_free(certificate->owners);
    xr_free(certificate->events);
    xr_free(certificate->edge_states);
    xr_free(certificate);
}

XrFingerprint xr_ownership_certificate_fingerprint(const XrOwnershipCertificate *certificate) {
    XrFingerprint empty = {{0}};
    return certificate ? certificate->fingerprint : empty;
}

size_t xr_ownership_certificate_owner_count(const XrOwnershipCertificate *certificate) {
    return certificate ? certificate->owner_count : 0;
}

size_t xr_ownership_certificate_event_count(const XrOwnershipCertificate *certificate) {
    return certificate ? certificate->event_count : 0;
}

size_t xr_ownership_certificate_edge_state_count(const XrOwnershipCertificate *certificate) {
    return certificate ? certificate->edge_state_count : 0;
}

const XrOwnershipOwnerRecord *
xr_ownership_certificate_owner(const XrOwnershipCertificate *certificate, uint32_t index) {
    return certificate && index < certificate->owner_count ? &certificate->owners[index] : NULL;
}

const XrOwnershipEventRecord *
xr_ownership_certificate_event(const XrOwnershipCertificate *certificate, uint32_t index) {
    return certificate && index < certificate->event_count ? &certificate->events[index] : NULL;
}

const XrOwnershipEdgeStateRecord *
xr_ownership_certificate_edge_state(const XrOwnershipCertificate *certificate, uint32_t index) {
    return certificate && index < certificate->edge_state_count ? &certificate->edge_states[index]
                                                                : NULL;
}
