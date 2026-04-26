/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_inlay_hints.h - Inlay hints for inline type annotations
 *
 * KEY CONCEPT:
 *   Provides inline hints for inferred types, parameter names, etc.
 */

#ifndef XLSP_INLAY_HINTS_H
#define XLSP_INLAY_HINTS_H

#include "xlsp_server.h"
#include "../../base/xjson.h"

// Inlay hint kinds
typedef enum {
    XLSP_HINT_TYPE = 1,
    XLSP_HINT_PARAMETER = 2
} XlspInlayHintKind;

// Analyze document for inlay hints in range
XR_FUNC XrJsonValue *xlsp_analyze_inlay_hints(XrLspServer *server, XrLspDocument *doc,
                                              XrLspRange range);

#endif  // XLSP_INLAY_HINTS_H
