/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_protocol_output.c - Serialize before changing request identity
 *
 * KEY CONCEPT:
 *   Envelope and serializer allocation failure cannot spend a request ID or
 *   write a partial JSON value. Failed stream writes close publication; bytes
 *   already accepted by the OS are not a rollback-capable transaction.
 */
#include "xlsp_protocol_output.h"
#include "xlsp_protocol_message.h"
#include "xlsp_framing.h"
#include "../../base/xmalloc.h"
#include <limits.h>

static XrXirStatus output_ready(XrLspServer *server) {
    if (!server || !server->transport) return XR_XIR_BAD_STRUCTURE;
    if (server->exit_received || !xlsp_transport_is_connected(server->transport)) {
        server->exit_received = true; return XR_XIR_IO;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xlsp_protocol_write_owned(XrLspServer *server, XrJsonValue *message) {
    XrXirStatus status = output_ready(server);
    if (status == XR_XIR_OK && (!message || message->type != XR_JSON_OBJECT)) status = XR_XIR_BAD_STRUCTURE;
    char *json = NULL; size_t length = 0;
    if (status == XR_XIR_OK) {
        json = xjson_stringify(message, &length);
        if (!json) status = XR_XIR_OUT_OF_MEMORY;
        else if (length > XLSP_FRAME_MAX_BODY) status = XR_XIR_BUDGET;
    }
    if (status == XR_XIR_OK) {
        xlsp_transport_write(server->transport, json, length);
        if (!xlsp_transport_is_connected(server->transport)) { server->exit_received = true; status = XR_XIR_IO; }
    }
    xr_free(json); xjson_free(message); return status;
}
static XrXirStatus output_call(XrLspServer *server, const char *method, XrJsonValue *params, bool request) {
    XrXirStatus status = output_ready(server); int32_t next = 0;
    if (status == XR_XIR_OK && request) {
        if (server->next_request_id == INT32_MAX) status = XR_XIR_BUDGET;
        else next = server->next_request_id + 1;
    }
    XrJsonValue *message = NULL;
    if (status == XR_XIR_OK) {
        XlspRequestId id = {.kind = XLSP_ID_NUMBER, .as.number = next};
        XlspProtocolStatus built = xlsp_protocol_call(request ? &id : NULL, method, &params, &message);
        status = built == XLSP_PROTOCOL_OK ? XR_XIR_OK : built == XLSP_PROTOCOL_OUT_OF_MEMORY ?
            XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
    }
    xjson_free(params);
    if (status == XR_XIR_OK) { status = xlsp_protocol_write_owned(server, message); message = NULL; }
    xjson_free(message);
    if (status == XR_XIR_OK && request) server->next_request_id = next;
    return status;
}
XR_FUNC XrXirStatus xlsp_protocol_send_request(XrLspServer *server, const char *method, XrJsonValue *params) {
    return output_call(server, method, params, true);
}
XR_FUNC XrXirStatus xlsp_protocol_send_notification(XrLspServer *server, const char *method, XrJsonValue *params) {
    return output_call(server, method, params, false);
}
