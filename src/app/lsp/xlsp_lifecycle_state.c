/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_lifecycle_state.c - Lifecycle admission and shutdown state ownership
 */
#include "xlsp_lifecycle_state.h"
#include "xlsp_server.h"
#include <string.h>

XR_FUNC XlspLifecycleAdmission xlsp_lifecycle_admit(const XrLspServer *server,
                                                  const char *method, bool is_request) {
    if (!server || !method || !method[0])
        return is_request ? XLSP_LIFECYCLE_INVALID_REQUEST : XLSP_LIFECYCLE_DROP;
    if (server->exit_received) return XLSP_LIFECYCLE_DROP;
    if (!is_request && !strcmp(method, "exit")) return XLSP_LIFECYCLE_ALLOW;
    if (server->shutdown_received)
        return is_request ? XLSP_LIFECYCLE_INVALID_REQUEST : XLSP_LIFECYCLE_DROP;
    if (!server->initialized) {
        if (is_request && !strcmp(method, "initialize")) return XLSP_LIFECYCLE_ALLOW;
        return is_request ? XLSP_LIFECYCLE_NOT_INITIALIZED : XLSP_LIFECYCLE_DROP;
    }
    if (!strcmp(method, "exit")) return XLSP_LIFECYCLE_INVALID_REQUEST;
    if (!strcmp(method, "initialize"))
        return is_request ? XLSP_LIFECYCLE_INVALID_REQUEST : XLSP_LIFECYCLE_DROP;
    if ((!strcmp(method, "shutdown") && !is_request) ||
        (!strcmp(method, "initialized") && is_request))
        return is_request ? XLSP_LIFECYCLE_INVALID_REQUEST : XLSP_LIFECYCLE_DROP;
    return XLSP_LIFECYCLE_ALLOW;
}

XR_FUNC XrJsonValue *xlsp_handle_lc_shutdown(XrLspServer *server, XrJsonValue *params) {
    if (!server) return NULL;
    XrXirStatus status = XR_XIR_OK;
    if (!server->initialized || server->shutdown_received || server->exit_received)
        status = XR_XIR_BAD_STAGE;
    else if (params) status = XR_XIR_BAD_STRUCTURE;
    XrJsonValue *result = status == XR_XIR_OK ? xjson_new_null() : NULL;
    if (status == XR_XIR_OK && !result) status = XR_XIR_OUT_OF_MEMORY;
    server->source_failure.stage = status == XR_XIR_OK ? 0 : 18;
    server->source_failure.status = (unsigned)status;
    // Result allocation must succeed before the request changes the lifecycle.
    if (status == XR_XIR_OK) server->shutdown_received = true;
    return result;
}

XR_FUNC void xlsp_handle_lc_exit(XrLspServer *server, XrJsonValue *params) {
    if (!server) return;
    XrXirStatus status = params ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
    server->source_failure.stage = status == XR_XIR_OK ? 0 : 19;
    server->source_failure.status = (unsigned)status;
    // Exit is valid before initialization and after shutdown, without allocation.
    if (status == XR_XIR_OK) server->exit_received = true;
}
