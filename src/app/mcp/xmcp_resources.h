/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_resources.h - MCP resource handlers
 *
 * KEY CONCEPT:
 *   Exposes static data as MCP resources:
 *   - xray://spec/cheatsheet
 *   - xray://spec/concurrency
 *   - xray://stdlib/modules
 */

#ifndef XMCP_RESOURCES_H
#define XMCP_RESOURCES_H

#include "../../base/xdefs.h"

/* Forward declarations */
typedef struct XrJsonValue XrJsonValue;
typedef struct XmcpServer XmcpServer;

/* Handle "resources/list" request. */
XR_FUNC XrJsonValue *xmcp_handle_resources_list(XmcpServer *server);

/* Handle "resources/read" request. */
XR_FUNC XrJsonValue *xmcp_handle_resources_read(XmcpServer *server, XrJsonValue *params);

#endif /* XMCP_RESOURCES_H */
