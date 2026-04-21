/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_protocol.h - MCP protocol message handling
 *
 * KEY CONCEPT:
 *   Handles MCP lifecycle messages (initialize, ping).
 *   Protocol version: 2025-03-26
 */

#ifndef XMCP_PROTOCOL_H
#define XMCP_PROTOCOL_H

#include "../../base/xdefs.h"

/* Forward declarations */
typedef struct XrJsonValue XrJsonValue;
typedef struct XmcpServer XmcpServer;

#define XMCP_PROTOCOL_VERSION "2025-03-26"
#define XMCP_SERVER_NAME      "xray-mcp-server"
#define XMCP_SERVER_VERSION   "0.1.0"

/* Handle "initialize" request. Returns the result JSON object. */
XR_FUNC XrJsonValue *xmcp_handle_initialize(XmcpServer *server, XrJsonValue *params);

#endif /* XMCP_PROTOCOL_H */
