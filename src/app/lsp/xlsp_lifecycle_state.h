/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_lifecycle_state.h - Lifecycle admission and shutdown state ownership
 */
#ifndef XLSP_LIFECYCLE_STATE_H
#define XLSP_LIFECYCLE_STATE_H

#include "../../base/xjson.h"

typedef struct XrLspServer XrLspServer;

typedef enum {
    XLSP_LIFECYCLE_ALLOW = 0,
    XLSP_LIFECYCLE_DROP = 1,
    XLSP_LIFECYCLE_INVALID_REQUEST = -32600,
    XLSP_LIFECYCLE_NOT_INITIALIZED = -32002
} XlspLifecycleAdmission;

// The dispatcher validates the JSON envelope and method bytes before admission.
XR_FUNC XlspLifecycleAdmission xlsp_lifecycle_admit(const XrLspServer *server,
                                                  const char *method, bool is_request);
XR_FUNC XrJsonValue *xlsp_handle_lc_shutdown(XrLspServer *server, XrJsonValue *params);
XR_FUNC void xlsp_handle_lc_exit(XrLspServer *server, XrJsonValue *params);

#endif // XLSP_LIFECYCLE_STATE_H
