/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_verify.h - Independent TargetPlan verifier
 */

#ifndef XR_TARGET_VERIFY_H
#define XR_TARGET_VERIFY_H

#include "xr_target_plan.h"

XR_FUNC bool xr_target_plan_verify(const XrTargetPlan *plan, char *error, size_t error_size);

#endif  // XR_TARGET_VERIFY_H
