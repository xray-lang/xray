/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_loop_phase_clock.h - Diagnostic process CPU phase counters
 */
#ifndef XR_DIAGNOSTIC_PHASE_CLOCK_H
#define XR_DIAGNOSTIC_PHASE_CLOCK_H
#include "base/xdefs.h"
enum { PHASE_ROOT, PHASE_WORK, PHASE_OUTPUT };
XR_FUNC void phase_reset(void);
XR_FUNC void phase_begin(unsigned phase);
XR_FUNC void phase_end(unsigned phase);
XR_FUNC void phase_report(const char *engine, int completed);
#endif // XR_DIAGNOSTIC_PHASE_CLOCK_H
