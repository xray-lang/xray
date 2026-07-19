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
#include <stdint.h>

#define XI_ANALYSIS_DOMAIN_COUNT 10u

typedef struct XiAnalysisProducerStats {
    uint64_t elapsed_ns;
    uint32_t requests;
    uint32_t recomputes;
} XiAnalysisProducerStats;

typedef struct XiAnalysisManager {
    XiFunc *func;
    XiAnalysisProducerStats producers[XI_ANALYSIS_DOMAIN_COUNT];
} XiAnalysisManager;

XR_FUNC void xi_analysis_manager_init(XiAnalysisManager *manager, XiFunc *func);
XR_FUNC XiEvidenceView xi_analysis_require(XiAnalysisManager *manager, XiEvidenceDomain domain,
                                           XiEvidenceSubject subject);
XR_FUNC bool xi_analysis_require_proven_domains(XiAnalysisManager *manager,
                                                XiEvidenceDomainMask domains, char *error,
                                                size_t error_size);
XR_FUNC void xi_analysis_manager_dump(const XiAnalysisManager *manager, void *file);

#endif  // XI_ANALYSIS_MANAGER_H
