/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_target_profile.h - Runtime-owned native TargetProfile projection
 */

#ifndef XR_RUNTIME_TARGET_PROFILE_H
#define XR_RUNTIME_TARGET_PROFILE_H

#include "../../base/xdefs.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct XrTargetProfile XrTargetProfile;

/* Projects the canonical native hosted runtime authority without adding
 * backend feature guesses or caller-authored machine facts. */
XR_FUNC bool xr_runtime_target_profile_build_native_hosted(
    XrTargetProfile **out, char *error, size_t error_size);

#endif  // XR_RUNTIME_TARGET_PROFILE_H
