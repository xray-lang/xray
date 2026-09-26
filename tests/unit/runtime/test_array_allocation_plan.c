/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_array_allocation_plan.c - Independent count and byte budget boundaries
 */
#include "runtime/core/xr_array_allocation_plan.h"
#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "requirement failed at line %d: %s\n", __LINE__, #condition); \
    abort(); } } while (0)

static void expect_failure(int64_t count, size_t width, uint32_t counts, size_t bytes,
                           XrArrayAllocationStatus status) {
    XrArrayAllocationPlan plan = xr_array_allocation_plan(count, width, counts, bytes);
    REQUIRE(plan.status == status);
    REQUIRE(plan.count == 0u && plan.bytes == 0u);
}

int main(void) {
    expect_failure(-1, 1u, UINT32_MAX, SIZE_MAX, XR_ARRAY_ALLOCATION_INVALID_LENGTH);
    expect_failure(INT64_MIN, 0u, 0u, 0u, XR_ARRAY_ALLOCATION_INVALID_LENGTH);
    expect_failure(INT64_MAX, 1u, UINT32_MAX, SIZE_MAX, XR_ARRAY_ALLOCATION_RESOURCE_LIMIT);
    expect_failure((int64_t)UINT32_MAX + 1, 0u, UINT32_MAX, SIZE_MAX,
                   XR_ARRAY_ALLOCATION_RESOURCE_LIMIT);
    expect_failure(1, 1u, 0u, SIZE_MAX, XR_ARRAY_ALLOCATION_RESOURCE_LIMIT);
    expect_failure(3, 8u, 3u, 23u, XR_ARRAY_ALLOCATION_RESOURCE_LIMIT);
    expect_failure(2, SIZE_MAX, 2u, SIZE_MAX, XR_ARRAY_ALLOCATION_RESOURCE_LIMIT);
    expect_failure(1, SIZE_MAX, 1u, SIZE_MAX - 1u, XR_ARRAY_ALLOCATION_RESOURCE_LIMIT);
    XrArrayAllocationPlan plan = xr_array_allocation_plan(0, SIZE_MAX, 0u, 0u);
    REQUIRE(plan.status == XR_ARRAY_ALLOCATION_OK && plan.count == 0u && plan.bytes == 0u);
    plan = xr_array_allocation_plan(1, SIZE_MAX, 1u, SIZE_MAX);
    REQUIRE(plan.status == XR_ARRAY_ALLOCATION_OK && plan.count == 1u && plan.bytes == SIZE_MAX);
    plan = xr_array_allocation_plan(UINT32_MAX, 1u, UINT32_MAX, SIZE_MAX);
    REQUIRE(plan.status == XR_ARRAY_ALLOCATION_OK && plan.count == UINT32_MAX && plan.bytes == UINT32_MAX);
    plan = xr_array_allocation_plan(UINT32_MAX, 0u, UINT32_MAX, 0u);
    REQUIRE(plan.status == XR_ARRAY_ALLOCATION_OK && plan.count == UINT32_MAX && plan.bytes == 0u);
    /* Exhaust every small count, width and budget against a widened product. */
    for (uint32_t count = 0u; count < 12u; ++count)
        for (size_t width = 0u; width < 12u; ++width)
            for (uint32_t budget = 0u; budget < 12u; ++budget)
                for (size_t bytes = 0u; bytes < 145u; ++bytes) {
                    uint64_t product = (uint64_t) count * width;
                    int allowed = count <= budget && product <= bytes;
                    plan = xr_array_allocation_plan(count, width, budget, bytes);
                    REQUIRE(plan.status == (allowed ? XR_ARRAY_ALLOCATION_OK : XR_ARRAY_ALLOCATION_RESOURCE_LIMIT));
                    REQUIRE(plan.count == (allowed ? count : 0u));
                    REQUIRE(plan.bytes == (allowed ? (size_t) product : 0u));
                }
    return 0;
}
