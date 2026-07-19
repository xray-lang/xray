/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "xi_analysis_manager.h"
#include "xi_range.h"
#include "xi_tbaa.h"
#include <stdio.h>

static bool require_one(XiFunc *func, XiEvidenceDomain domain) {
    if (xi_evidence_is_proven_current(func, domain))
        return true;
    switch (domain) {
        case XI_EVD_RANGE:
            xi_range_analyze(func);
            break;
        case XI_EVD_ALIAS:
            xi_tbaa_annotate(func);
            break;
        default:
            return false;
    }
    return xi_evidence_is_proven_current(func, domain);
}

bool xi_analysis_require(XiFunc *func, XiEvidenceDomainMask domains, char *error,
                         size_t error_size) {
    if (!func) {
        if (error && error_size)
            snprintf(error, error_size, "analysis manager received a null function");
        return false;
    }
    for (uint32_t bit = 1; bit <= XI_EVD_MEMSSA; bit <<= 1u) {
        if ((domains & bit) == 0)
            continue;
        XiEvidenceDomain domain = (XiEvidenceDomain) bit;
        if (require_one(func, domain))
            continue;
        XiEvidenceView view = xi_evidence_query(func, domain);
        if (error && error_size) {
            snprintf(error, error_size, "evidence domain '%s' is unavailable: %s",
                     xi_evidence_domain_name(domain), xi_evidence_reason_name(view.reason));
        }
        return false;
    }
    return true;
}
