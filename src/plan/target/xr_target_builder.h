/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_builder.h - Composable TargetPlan construction
 *
 * KEY CONCEPT:
 *   The public API owns one internal collection sequence and one final freeze.
 *   This foundation currently collects the required scalar family; later
 *   families need canonical intent materialization before joining the mask.
 */

#ifndef XR_TARGET_BUILDER_H
#define XR_TARGET_BUILDER_H

#include "xr_target_plan.h"

XR_FUNC bool xr_target_plan_build(const XrSemanticPlan *semantic_plan,
                                  XrTargetProfile *profile,
                                  XrTargetPlan **out,
                                  char *error,
                                  size_t error_size);

#endif  // XR_TARGET_BUILDER_H
