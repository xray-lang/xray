/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_params.h - Immutable request JSON on its compiler ledger
 *
 * KEY CONCEPT:
 *   Preserve the decoder's exact input, including duplicate keys and numeric
 *   representation, after the dispatcher's JSON dies. Blocks pin the original
 *   ledger; this owner never resets or substitutes its resource accounting.
 */
#ifndef XLSP_SOURCE_PARAMS_H
#define XLSP_SOURCE_PARAMS_H
#include "../../base/xcompile_resources.h"
#include "../../base/xjson.h"
typedef struct XlspSourceParams XlspSourceParams;
XR_FUNC XrCompileResourceStatus xlsp_source_params_new(XrCompileResources *,
    const XrJsonValue *, XlspSourceParams **);
XR_FUNC const XrJsonValue *xlsp_source_params_view(const XlspSourceParams *);
XR_FUNC void xlsp_source_params_free(XlspSourceParams *);
#endif // XLSP_SOURCE_PARAMS_H
