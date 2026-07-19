/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_analysis_manager.h - Single freshness gate for local Xi evidence
 */

#ifndef XI_ANALYSIS_MANAGER_H
#define XI_ANALYSIS_MANAGER_H

#include "xi_evidence.h"
#include "../base/xdefs.h"
#include <stdbool.h>
#include <stddef.h>

XR_FUNC bool xi_analysis_require(XiFunc *func, XiEvidenceDomainMask domains, char *error,
                                 size_t error_size);

#endif  // XI_ANALYSIS_MANAGER_H
