/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_tools.h - MCP tool handlers
 *
 * KEY CONCEPT:
 *   Phase 1 tools: xray_check, xray_syntax_lookup, xray_stdlib_search.
 *   Each tool receives JSON params, returns a JSON result.
 */

#ifndef XMCP_TOOLS_H
#define XMCP_TOOLS_H

#include "../../base/xdefs.h"

/* Forward declarations */
typedef struct XrJsonValue XrJsonValue;
typedef struct XmcpServer XmcpServer;

/* Handle "tools/list" request. */
XR_FUNC XrJsonValue *xmcp_handle_tools_list(void);

/* Handle "tools/call" request. */
XR_FUNC XrJsonValue *xmcp_handle_tools_call(XmcpServer *server, XrJsonValue *params);

#endif // XMCP_TOOLS_H
