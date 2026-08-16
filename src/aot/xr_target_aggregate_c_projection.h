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

typedef enum XrCAggregateProjectionKind {
    XR_C_AGGREGATE_PROJECTION_INVALID = 0,
    XR_C_AGGREGATE_PROJECTION_NAMED_STRUCT = 1,
    XR_C_AGGREGATE_PROJECTION_FIXED_ARRAY_BACKING = 2,
    XR_C_AGGREGATE_PROJECTION_TUPLE_BACKING = 3,
} XrCAggregateProjectionKind;

typedef struct XrCAggregateProjection {
    uint32_t layout;
    uint32_t backing_value;
    uint32_t element_count;
    uint64_t abi_key;
    uint8_t kind;
    uint8_t element_native_type;
    uint16_t reserved;
    char c_type[40];
    char element_c_type[16];
} XrCAggregateProjection;

XR_FUNC bool xr_c_aggregate_projection(
    const XrTargetPlan *target_plan,
    const XrTargetValueRepRecord *binding,
    XrCAggregateProjection *out);

#endif  // XR_TARGET_AGGREGATE_C_PROJECTION_H
