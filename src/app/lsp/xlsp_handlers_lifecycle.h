/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_handlers_lifecycle.h - LSP lifecycle handler declarations
 */

#ifndef XLSP_HANDLERS_LIFECYCLE_H
#define XLSP_HANDLERS_LIFECYCLE_H

#include "xlsp_json.h"

typedef struct XrLspServer XrLspServer;

// Lifecycle handlers
XR_FUNC XrJsonValue *xlsp_handle_lc_initialize(XrLspServer *server, XrJsonValue *params);
XR_FUNC void xlsp_handle_lc_initialized(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_lc_shutdown(XrLspServer *server, XrJsonValue *params);
XR_FUNC void xlsp_handle_lc_exit(XrLspServer *server, XrJsonValue *params);

#endif // XLSP_HANDLERS_LIFECYCLE_H
