/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_notifications.c - Real protocol notifications shared by source tools
 *
 * KEY CONCEPT:
 *   Notifications use the server transport without any compiler or VM state.
 */
#include "xmcp_protocol.h"
#include "xmcp_server.h"
#include "../../base/xjson.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"

XR_FUNC void xmcp_send_notification(XmcpServer *server, const char *method, XrJsonValue *params) {
    XR_DCHECK(server != NULL, "xmcp_send_notification: NULL server");
    XR_DCHECK(method != NULL, "xmcp_send_notification: NULL method");
    if (server->lifecycle_state != XMCP_LIFECYCLE_READY)
        return;

    XrJsonValue *msg = xjson_new_object();
    XJSON_SET_STRING(msg, "jsonrpc", "2.0");
    XJSON_SET_STRING(msg, "method", method);
    if (params) {
        xjson_object_set(msg, "params", params);
    }

    size_t len = 0;
    char *json = xjson_stringify(msg, &len);
    if (json) {
        xmcp_stdio_write_message(&server->transport, json, len);
        xr_free(json);
    }
    xjson_free(msg);
}

XR_FUNC void xmcp_send_log_notification(XmcpServer *server, const char *level, const char *message) {
    XR_DCHECK(server != NULL, "xmcp_send_log_notification: NULL server");
    XR_DCHECK(level != NULL, "xmcp_send_log_notification: NULL level");
    XR_DCHECK(message != NULL, "xmcp_send_log_notification: NULL message");
    if (server->lifecycle_state != XMCP_LIFECYCLE_READY)
        return;

    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "level", level);
    xjson_object_set(params, "data", xjson_new_string(message));
    xmcp_send_notification(server, "notifications/message", params);
}

XR_FUNC void xmcp_send_progress_notification(XmcpServer *server, const XrJsonValue *progress_token,
                                     int64_t progress, int64_t total) {
    XR_DCHECK(server != NULL, "xmcp_send_progress_notification: NULL server");
    if (!progress_token)
        return;
    if (server->lifecycle_state != XMCP_LIFECYCLE_READY)
        return;

    XrJsonValue *params = xjson_new_object();
    xjson_object_set(params, "progressToken", xjson_clone((XrJsonValue *)progress_token));
    XJSON_SET_INT(params, "progress", progress);
    if (total > 0) {
        XJSON_SET_INT(params, "total", total);
    }
    xmcp_send_notification(server, "notifications/progress", params);
}

