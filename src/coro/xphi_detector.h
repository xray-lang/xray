/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xphi_detector.h - Runtime storage for a bounded phi accrual detector
 *
 * cluster.PhiDetector owns the algorithm. This POD projection is kept beside
 * the coroutine scheduler because native poll loops cannot enter Xray yet.
 */

#ifndef XR_CORO_PHI_DETECTOR_H
#define XR_CORO_PHI_DETECTOR_H

#include "../base/xdefs.h"

#include <stdint.h>

#define XR_PHI_WINDOW_SIZE 100

typedef struct XrPhiDetector {
    double intervals[XR_PHI_WINDOW_SIZE];
    int sample_count;
    int write_idx;
    double mean;
    double variance;
    double sum;
    double sum_sq;
    int64_t last_heartbeat_ts;
} XrPhiDetector;

XR_FUNC void xr_phi_detector_init(XrPhiDetector *detector, double expected_interval_ms);
XR_FUNC void xr_phi_detector_record(XrPhiDetector *detector, int64_t now_ms);
XR_FUNC double xr_phi_detector_value(const XrPhiDetector *detector, int64_t now_ms);

#endif /* XR_CORO_PHI_DETECTOR_H */
