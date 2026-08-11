/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_profile_internal.h - Target profile storage and canonical hashing
 */

#ifndef XR_TARGET_PROFILE_INTERNAL_H
#define XR_TARGET_PROFILE_INTERNAL_H

#include "xr_target_profile.h"
#include <stdatomic.h>

/* Artifact decoders and white-box tests may construct this storage directly.
 * Production planning must use xr_target_profile_build(). */
typedef struct XrTargetProfileDraft {
    uint32_t schema_version;
    XrTargetMachineFacts machine;
    uint64_t provider_mask;
    XrFingerprint provider_set_fingerprint;
    XrFingerprint object_header_fingerprint;
    XrFingerprint runtime_abi_fingerprint;
} XrTargetProfileDraft;

struct XrTargetProfile {
    atomic_uint_least32_t references;
    bool frozen;
    XrTargetProfileDraft facts;
    XrFingerprint fingerprint;
};

XR_FUNC void xr_target_profile_compute_fingerprint(const XrTargetProfileDraft *facts,
                                                   XrFingerprint *out);
XR_FUNC bool xr_target_profile_freeze(const XrTargetProfileDraft *draft,
                                      XrTargetProfile **out, char *error,
                                      size_t error_size);
XR_FUNC const XrTargetProfileDraft *xr_target_profile_facts(
    const XrTargetProfile *profile);

#endif  // XR_TARGET_PROFILE_INTERNAL_H
