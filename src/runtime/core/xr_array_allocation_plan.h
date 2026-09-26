/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_array_allocation_plan.h - Checked executor-neutral array allocation sizing
 */
#ifndef XR_ARRAY_ALLOCATION_PLAN_H
#define XR_ARRAY_ALLOCATION_PLAN_H

#include <stddef.h>
#include <stdint.h>

typedef enum XrArrayAllocationStatus {
    XR_ARRAY_ALLOCATION_OK,
    XR_ARRAY_ALLOCATION_INVALID_LENGTH,
    XR_ARRAY_ALLOCATION_RESOURCE_LIMIT
} XrArrayAllocationStatus;

typedef struct XrArrayAllocationPlan {
    XrArrayAllocationStatus status;
    uint32_t count;
    size_t bytes;
} XrArrayAllocationPlan;

/* The element size describes the executor's physical value representation.
 * A zero-sized representation is valid for unit-like values. The count budget
 * still applies even when no element bytes are required. */
static inline XrArrayAllocationPlan xr_array_allocation_plan(
    int64_t length, size_t element_size, uint32_t count_budget, size_t byte_budget) {
    XrArrayAllocationPlan plan = {XR_ARRAY_ALLOCATION_RESOURCE_LIMIT, 0u, 0u};
    if (length < 0) {
        plan.status = XR_ARRAY_ALLOCATION_INVALID_LENGTH;
        return plan;
    }
    if ((uint64_t) length > count_budget)
        return plan;
    if (element_size != 0u && (uint64_t) length > byte_budget / element_size)
        return plan;
    plan.status = XR_ARRAY_ALLOCATION_OK;
    plan.count = (uint32_t) length;
    plan.bytes = (size_t) plan.count * element_size;
    return plan;
}

#endif
