/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_jsonrpc.c - JSON-RPC 2.0 validation for MCP
 *
 * KEY CONCEPT:
 *   Validates JSON-RPC request shape before MCP method dispatch.
 */

#include "xmcp_jsonrpc.h"
#include "xmcp_protocol.h"
#include "../../base/xchecks.h"
#include "../../base/xjson.h"
#include <string.h>

static bool xmcp_jsonrpc_id_valid(XrJsonValue *id) {
    if (!id)
        return true;
    if (id->type == XR_JSON_NULL || id->type == XR_JSON_STRING)
        return true;
    return id->type == XR_JSON_NUMBER && id->is_integer;
}

static bool xmcp_jsonrpc_params_valid(XrJsonValue *params) {
    if (!params)
        return true;
    return params->type == XR_JSON_OBJECT || params->type == XR_JSON_ARRAY;
}

static bool xmcp_jsonrpc_fail(XrJsonValue *id, XrJsonValue **error_id, int *error_code,
                              const char **error_message, const char *message) {
    if (error_id)
        *error_id = xmcp_jsonrpc_id_valid(id) ? id : NULL;
    if (error_code)
        *error_code = XMCP_ERR_INVALID_REQ;
    if (error_message)
        *error_message = message;
    return false;
}

XR_FUNC bool xmcp_jsonrpc_validate_message(XrJsonValue *msg, XmcpJsonRpcMessage *out,
                                           XrJsonValue **error_id, int *error_code,
                                           const char **error_message) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (error_id)
        *error_id = NULL;
    if (error_code)
        *error_code = 0;
    if (error_message)
        *error_message = NULL;

    if (!out)
        return false;
    if (!msg || msg->type != XR_JSON_OBJECT)
        return xmcp_jsonrpc_fail(NULL, error_id, error_code, error_message,
                                 "Invalid Request: expected object");

    XrJsonValue *id = xjson_get(msg, "id");
    if (!xmcp_jsonrpc_id_valid(id))
        return xmcp_jsonrpc_fail(NULL, error_id, error_code, error_message,
                                 "Invalid Request: invalid id");

    XrJsonValue *version = xjson_get(msg, "jsonrpc");
    if (!xjson_is_string(version) || version->string_len != 3 || memcmp(version->as.string, "2.0", 3))
        return xmcp_jsonrpc_fail(id, error_id, error_code, error_message,
                                 "Invalid Request: invalid jsonrpc version");

    XrJsonValue *method_value = xjson_get(msg, "method");
    const char *method = xjson_is_string(method_value) ? method_value->as.string : NULL;
    if (!method || !method_value->string_len || memchr(method, 0, method_value->string_len))
        return xmcp_jsonrpc_fail(id, error_id, error_code, error_message,
                                 "Invalid Request: missing method");

    XrJsonValue *params = xjson_get(msg, "params");
    if (!xmcp_jsonrpc_params_valid(params))
        return xmcp_jsonrpc_fail(id, error_id, error_code, error_message,
                                 "Invalid Request: invalid params");

    out->method = method;
    out->id = id;
    out->params = params;
    out->is_notification = id == NULL;
    return true;
}
