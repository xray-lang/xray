/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparallel_executor.c - Runtime-owned parallel batch shape helpers
 */

#include "xparallel_executor.h"

#include "xworker.h"

#include <stdint.h>

static int xr_parallel_normalize_max_lanes(int max_lanes) {
    if (max_lanes <= 0 || max_lanes > XR_PARALLEL_EXECUTOR_MAX_LANES)
        return XR_PARALLEL_EXECUTOR_MAX_LANES;
    return max_lanes;
}

int xr_parallel_runtime_worker_cap(const XrRuntime *runtime, int max_lanes) {
    if (!runtime || runtime->worker_count <= 0)
        return 0;
    int cap = runtime->worker_count;
    int max_cap = xr_parallel_normalize_max_lanes(max_lanes);
    if (cap > max_cap)
        cap = max_cap;
    return cap;
}

int xr_parallel_resolve_lane_count(const XrRuntime *runtime, int64_t item_count,
                                   int64_t requested_workers, int max_lanes) {
    if (requested_workers < 0)
        return -1;
    if (!runtime || item_count <= 1 || requested_workers == 1)
        return 0;
    if (xr_runtime_deterministic_mode(runtime))
        return 0;

    int runtime_workers = xr_parallel_runtime_worker_cap(runtime, max_lanes);
    if (runtime_workers <= 1)
        return 0;

    int64_t lanes = requested_workers == 0 ? (int64_t) runtime_workers : requested_workers;
    if (lanes > item_count)
        lanes = item_count;
    if (lanes > runtime_workers)
        lanes = runtime_workers;
    int normalized_max = xr_parallel_normalize_max_lanes(max_lanes);
    if (lanes > normalized_max)
        lanes = normalized_max;
    return lanes > 1 ? (int) lanes : 0;
}

bool xr_parallel_lane_bounds(int64_t start, int64_t end, int lane_count, int lane_id,
                             int64_t *out_begin, int64_t *out_end) {
    if (!out_begin || !out_end || lane_count <= 0 || lane_id < 0 || lane_id >= lane_count ||
        end <= start)
        return false;

    uint64_t count_u = (uint64_t) end - (uint64_t) start;
    int64_t count = count_u > (uint64_t) INT64_MAX ? INT64_MAX : (int64_t) count_u;
    int64_t base = count / lane_count;
    int64_t rem = count % lane_count;
    int64_t lane_items = base + (lane_id < rem ? 1 : 0);
    if (lane_items <= 0)
        return false;

    int64_t extra_before = lane_id < rem ? lane_id : rem;
    int64_t begin = start + (int64_t) lane_id * base + extra_before;
    *out_begin = begin;
    *out_end = begin + lane_items;
    return true;
}
