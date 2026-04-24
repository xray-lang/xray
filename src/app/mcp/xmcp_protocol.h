/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_protocol.h - MCP protocol definitions and lifecycle
 *
 * KEY CONCEPT:
 *   Central protocol header: version, error codes, capabilities,
 *   notification infrastructure, and lifecycle handlers.
 *   Protocol version: 2025-03-26
 */

#ifndef XMCP_PROTOCOL_H
#define XMCP_PROTOCOL_H

#include "../../base/xdefs.h"
#include "xray_version.h"
#include <stdbool.h>

/* Forward declarations */
typedef struct XrJsonValue XrJsonValue;
typedef struct XmcpServer XmcpServer;

/* ---- Protocol constants ------------------------------------------------- */

#define XMCP_PROTOCOL_VERSION "2025-03-26"
#define XMCP_SERVER_NAME      "xray-mcp-server"
#define XMCP_SERVER_VERSION   XRAY_VERSION_STRING

/* ---- JSON-RPC 2.0 standard error codes --------------------------------- */

#define XMCP_ERR_PARSE          (-32700)  /* Invalid JSON */
#define XMCP_ERR_INVALID_REQ    (-32600)  /* Not a valid JSON-RPC request */
#define XMCP_ERR_METHOD_NOT_FOUND (-32601) /* Method does not exist */
#define XMCP_ERR_INVALID_PARAMS (-32602)  /* Invalid method parameters */
#define XMCP_ERR_INTERNAL       (-32603)  /* Internal server error */

/* MCP-specific error codes (server-defined range: -32000 to -32099) */
#define XMCP_ERR_NOT_INITIALIZED (-32002) /* Request before initialize */
#define XMCP_ERR_ALREADY_INIT    (-32003) /* Double initialize */

/* ---- Method dispatch table ---------------------------------------------- */

/* Handler signature: returns result JSON on success, NULL to signal error.
 * For notifications (no id), the return value is ignored. */
typedef XrJsonValue *(*XmcpMethodHandler)(XmcpServer *server, XrJsonValue *params);

typedef struct XmcpMethodEntry {
    const char       *method;         /* JSON-RPC method name */
    XmcpMethodHandler handler;        /* Handler function */
    bool              is_notification; /* true = no response expected */
    bool              needs_init;     /* true = reject if not yet initialized */
} XmcpMethodEntry;

/* ---- Lifecycle handlers ------------------------------------------------- */

/* Handle "initialize" request. Returns the result JSON object. */
XR_FUNC XrJsonValue *xmcp_handle_initialize(XmcpServer *server, XrJsonValue *params);

/* ---- Notification sending ----------------------------------------------- */

/* Send a JSON-RPC notification to the client (no id, no response). */
XR_FUNC void xmcp_send_notification(XmcpServer *server,
                                      const char *method,
                                      XrJsonValue *params);

/* Send a log message notification (notifications/message). */
XR_FUNC void xmcp_send_log_notification(XmcpServer *server,
                                          const char *level,
                                          const char *message);

/* Send a progress notification (notifications/progress).
 * progress_token comes from the request's _meta.progressToken.
 * progress and total are 0-based; total can be 0 if unknown. */
XR_FUNC void xmcp_send_progress_notification(XmcpServer *server,
                                               int64_t progress_token,
                                               int progress,
                                               int total);

#endif // XMCP_PROTOCOL_H
