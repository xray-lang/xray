/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_source_allocation_probe.h - Source metadata allocation test boundary
 */

#ifndef XR_PROGRAM_SOURCE_ALLOCATION_PROBE_H
#define XR_PROGRAM_SOURCE_ALLOCATION_PROBE_H

#include "xr_program_allocation_probe.h"

XR_FUNC char *xr_program_test_strdup(const char *source);
#define xr_strdup(source) xr_program_test_strdup(source)

#endif // XR_PROGRAM_SOURCE_ALLOCATION_PROBE_H
