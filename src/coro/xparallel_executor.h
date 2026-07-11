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
#include "../os/os_thread.h"

#ifndef XR_FUNC
#define XR_FUNC extern
#endif

struct XrRuntime;

enum {
    XR_PARALLEL_EXECUTOR_MAX_LANES = XR_MAX_WORKERS,
};

typedef struct XrParallelJoin {
    xr_mutex_t mutex;
    xr_cond_t done;
    int remaining_lanes;
    bool initialized;
} XrParallelJoin;

XR_FUNC int xr_parallel_runtime_worker_cap(const struct XrRuntime *runtime, int max_lanes);
XR_FUNC int xr_parallel_resolve_lane_count(const struct XrRuntime *runtime, int64_t item_count,
                                           int64_t requested_workers, int max_lanes);
XR_FUNC bool xr_parallel_lane_bounds(int64_t start, int64_t end, int lane_count, int lane_id,
                                     int64_t *out_begin, int64_t *out_end);
XR_FUNC void xr_parallel_join_init(XrParallelJoin *join, int remaining_lanes);
XR_FUNC void xr_parallel_join_lane_done(XrParallelJoin *join);
XR_FUNC void xr_parallel_join_wait(XrParallelJoin *join, struct XrRuntime *runtime);
XR_FUNC void xr_parallel_join_destroy(XrParallelJoin *join);

#endif /* XRAY_CORO_XPARALLEL_EXECUTOR_H */
