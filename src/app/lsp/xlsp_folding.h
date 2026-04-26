/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_folding.h - Folding range support
 */

#ifndef XLSP_FOLDING_H
#define XLSP_FOLDING_H

#include "xlsp_server.h"
#include "../../base/xjson.h"

XR_FUNC XrJsonValue *xlsp_handle_folding_range(XrLspServer *server, XrJsonValue *params);

#endif  // XLSP_FOLDING_H
