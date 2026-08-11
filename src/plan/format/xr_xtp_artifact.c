/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xtp_artifact.c - Immutable typed artifact candidate ownership
 */

#include "xr_xtp_internal.h"
#include "../../base/xmalloc.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

XR_FUNC void xr_xtp_set_error(char *error, size_t error_size,
                              const char *code, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "%s: XTP: %s", code, detail);
}

XR_FUNC bool xr_xtp_fingerprint_is_zero(XrFingerprint fingerprint) {
    static const uint8_t zero[XR_FINGERPRINT_BYTES] = {0};
    return memcmp(fingerprint.bytes, zero, sizeof(zero)) == 0;
}

XR_FUNC XrXtpCandidate *xr_xtp_candidate_retain(XrXtpCandidate *candidate) {
    if (!candidate)
        return NULL;
    uint_least32_t count = atomic_load_explicit(&candidate->references, memory_order_relaxed);
    do {
        if (count == 0 || count == UINT_LEAST32_MAX)
            return NULL;
    } while (!atomic_compare_exchange_weak_explicit(&candidate->references, &count, count + 1,
                                                     memory_order_relaxed,
                                                     memory_order_relaxed));
    return candidate;
}

XR_FUNC void xr_xtp_candidate_release(XrXtpCandidate *candidate) {
    if (!candidate)
        return;
    uint_least32_t count = atomic_load_explicit(&candidate->references, memory_order_relaxed);
    do {
        if (count == 0)
            return;
    } while (!atomic_compare_exchange_weak_explicit(&candidate->references, &count, count - 1,
                                                     memory_order_acq_rel,
                                                     memory_order_relaxed));
    if (count != 1)
        return;
    xr_free(candidate->bytes);
    xr_free(candidate);
}

XR_FUNC bool xr_xtp_candidate_identity(const XrXtpCandidate *candidate,
                                       XrXtpIdentity *identity) {
    if (!candidate || !identity)
        return false;
    *identity = candidate->identity;
    return true;
}

XR_FUNC bool xr_xtp_candidate_resources(const XrXtpCandidate *candidate,
                                        XrXtpResourceManifest *resources) {
    if (!candidate || !resources)
        return false;
    *resources = candidate->resources;
    return true;
}

XR_FUNC const XrXtpSectionView *xr_xtp_candidate_section(const XrXtpCandidate *candidate,
                                                        XrXtpSectionKind kind) {
    if (!candidate || kind <= XR_XTP_SECTION_INVALID || kind >= XR_XTP_SECTION_COUNT)
        return NULL;
    return &candidate->sections[(uint32_t) kind - 1u];
}

XR_FUNC void xr_xtp_encoded_free(uint8_t *bytes) {
    xr_free(bytes);
}
