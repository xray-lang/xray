/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_format.h - Document formatting declarations
 */

#ifndef XLSP_FORMAT_H
#define XLSP_FORMAT_H

#include "../../base/xjson.h"

typedef struct XrLspDocument XrLspDocument;

// Format entire document
XR_FUNC XrJsonValue *xlsp_analyze_format(XrLspDocument *doc);

#endif // XLSP_FORMAT_H
