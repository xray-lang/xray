/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xphi_detector.c - Native poll-loop projection of cluster.PhiDetector
 */

#include "xphi_detector.h"

#include <math.h>
#include <string.h>

void xr_phi_detector_init(XrPhiDetector *detector, double expected_interval_ms) {
    memset(detector, 0, sizeof(*detector));
    detector->mean = expected_interval_ms;
    detector->variance = 100.0;
}

void xr_phi_detector_record(XrPhiDetector *detector, int64_t now_ms) {
    if (detector->last_heartbeat_ts > 0) {
        double interval = (double) (now_ms - detector->last_heartbeat_ts);
        if (interval < 0.0)
            interval = 0.0;

        if (detector->sample_count >= XR_PHI_WINDOW_SIZE) {
            double old_value = detector->intervals[detector->write_idx];
            detector->sum -= old_value;
            detector->sum_sq -= old_value * old_value;
        } else {
            detector->sample_count++;
        }

        detector->intervals[detector->write_idx] = interval;
        detector->write_idx = (detector->write_idx + 1) % XR_PHI_WINDOW_SIZE;
        detector->sum += interval;
        detector->sum_sq += interval * interval;
        detector->mean = detector->sum / detector->sample_count;
        detector->variance =
            (detector->sum_sq / detector->sample_count) - (detector->mean * detector->mean);
        if (detector->variance < 1.0)
            detector->variance = 1.0;
    }
    detector->last_heartbeat_ts = now_ms;
}

double xr_phi_detector_value(const XrPhiDetector *detector, int64_t now_ms) {
    if (detector->last_heartbeat_ts == 0 || detector->sample_count < 2)
        return 0.0;

    double elapsed = (double) (now_ms - detector->last_heartbeat_ts);
    if (elapsed < 0.0)
        elapsed = 0.0;
    double stddev = sqrt(detector->variance);
    if (stddev < 1.0)
        stddev = 1.0;

    double y = (elapsed - detector->mean) / stddev;
    double later = 1.0 / (1.0 + exp(1.7155 * y));
    if (later < 1e-15)
        later = 1e-15;
    return -log10(later);
}
