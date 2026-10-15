/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_config.h - Owned configuration transactions
 *
 * KEY CONCEPT:
 *   Read and validate on the original ledger, then publish to the application
 *   heap only if the receiving configuration still matches the captured base.
 */
#ifndef XLSP_SOURCE_CONFIG_H
#define XLSP_SOURCE_CONFIG_H
#include "xlsp_server.h"
#include "../../base/xtoml.h"
#include "../../xir/xxir.h"

typedef struct XlspSourceConfig XlspSourceConfig;
/* Failure preserves output and current. The result owns both configurations
 * after the TOML, file bytes, caller strings and caller ledger lease die. */
XR_FUNC XrXirStatus xlsp_source_config_read(XrCompileResources *resources,
    const char *root_path, const XlspConfig *current,
    const XrTomlParseLimits *limits, XlspSourceConfig **output);
XR_FUNC const XlspConfig *xlsp_source_config_view(const XlspSourceConfig *owner);
/* Caller exclusion is required. Stale bases and all allocation/work failures
 * preserve the receiver, including its existing pointers. Receiver allocations
 * use the application heap; all scans and copies use the original work ledger. */
XR_FUNC XrXirStatus xlsp_source_config_apply(const XlspSourceConfig *owner,
    XlspConfig *target);
XR_FUNC void xlsp_source_config_free(XlspSourceConfig *owner);
#endif // XLSP_SOURCE_CONFIG_H
