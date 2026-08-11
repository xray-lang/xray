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

struct XrTargetProfile {
    atomic_uint_least32_t references;
    bool frozen;
    XrTargetProfileDraft facts;
    XrFingerprint fingerprint;
};

XR_FUNC void xr_target_profile_compute_fingerprint(const XrTargetProfileDraft *facts,
                                                   XrFingerprint *out);

#endif  // XR_TARGET_PROFILE_INTERNAL_H
