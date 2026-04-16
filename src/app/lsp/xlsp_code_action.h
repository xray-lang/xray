/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_code_action.h - Code action support (quickfix, refactor)
 */

#ifndef XLSP_CODE_ACTION_H
#define XLSP_CODE_ACTION_H

#include "xlsp_server.h"
#include "xlsp_json.h"

XR_FUNC XrJsonValue *xlsp_handle_code_action(XrLspServer *server, XrJsonValue *params);

#endif // XLSP_CODE_ACTION_H
