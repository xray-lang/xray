/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_jsonrpc.h - JSON-RPC 2.0 validation for MCP
 *
 * KEY CONCEPT:
 *   JSON parsing produces a DOM; this module validates JSON-RPC request
 *   shape before method dispatch. It does not know MCP method semantics.
 */

#ifndef XMCP_JSONRPC_H
#define XMCP_JSONRPC_H

#include "../../base/xdefs.h"
#include <stdbool.h>

/* Forward declarations */
typedef struct XrJsonValue XrJsonValue;

typedef struct XmcpJsonRpcMessage {
    const char *method;
    XrJsonValue *id;
    XrJsonValue *params;
    bool is_notification;
} XmcpJsonRpcMessage;

XR_FUNC bool xmcp_jsonrpc_validate_message(XrJsonValue *msg, XmcpJsonRpcMessage *out,
                                           XrJsonValue **error_id, int *error_code,
                                           const char **error_message);

#endif  // XMCP_JSONRPC_H
