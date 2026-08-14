/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_aggregate_c_projection.h - Exact TargetPlan aggregate C projection
 */

#ifndef XR_TARGET_AGGREGATE_C_PROJECTION_H
#define XR_TARGET_AGGREGATE_C_PROJECTION_H

#include "../plan/target/xr_target_plan.h"

typedef struct XrCAggregateProjection {
    uint32_t layout;
    uint64_t abi_key;
    char c_type[40];
} XrCAggregateProjection;

XR_FUNC bool xr_c_aggregate_projection(
    const XrTargetPlan *target_plan,
    const XrTargetValueRepRecord *binding,
    XrCAggregateProjection *out);

#endif  // XR_TARGET_AGGREGATE_C_PROJECTION_H
