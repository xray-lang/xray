/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_target_profile.h - Numeric toolchain TargetProfile authority
 */

#ifndef XTC_TARGET_PROFILE_H
#define XTC_TARGET_PROFILE_H

#include "xtc_model.h"
#include "../../plan/target/xr_target_profile.h"

/* Builds the one production profile currently owned by this process: its
 * exact native hosted target. Cross-target/freestanding callers must supply a
 * future independently validated runtime/provider manifest authority. */
XR_FUNC bool xtc_target_profile_build_native_hosted(
    const XrToolchainTarget *target,
    const XrTargetCodegenFacts *codegen,
    XrTargetProfile **out, char *error, size_t error_size);
XR_FUNC bool xtc_target_profile_build_native_freestanding(
    const XrToolchainTarget *target, const XrTargetCodegenFacts *codegen,
    uint64_t provider_mask, XrTargetProfile **out, char *error, size_t error_size);
XR_FUNC bool xtc_target_profile_build_current_native_hosted(
    const XrTargetCodegenFacts *codegen,
    XrTargetProfile **out, char *error, size_t error_size);

#endif  // XTC_TARGET_PROFILE_H
