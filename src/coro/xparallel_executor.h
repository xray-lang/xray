/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparallel_executor.h - Runtime-owned parallel batch shape helpers
 */

#ifndef XRAY_CORO_XPARALLEL_EXECUTOR_H
#define XRAY_CORO_XPARALLEL_EXECUTOR_H

#include <stdbool.h>
#include <stdint.h>

#include "../base/xconstants.h"

#ifndef XR_FUNC
#define XR_FUNC extern
#endif

struct XrRuntime;

enum {
    XR_PARALLEL_EXECUTOR_MAX_LANES = XR_MAX_WORKERS,
};

XR_FUNC int xr_parallel_runtime_worker_cap(const struct XrRuntime *runtime, int max_lanes);
XR_FUNC int xr_parallel_resolve_lane_count(const struct XrRuntime *runtime, int64_t item_count,
                                           int64_t requested_workers, int max_lanes);
XR_FUNC bool xr_parallel_lane_bounds(int64_t start, int64_t end, int lane_count, int lane_id,
                                     int64_t *out_begin, int64_t *out_end);

#endif /* XRAY_CORO_XPARALLEL_EXECUTOR_H */
