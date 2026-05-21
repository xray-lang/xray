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
 *   Static resources and URI template resources:
 *   - xray://spec/cheatsheet       Full language cheatsheet
 *   - xray://spec/concurrency      Concurrency model
 *   - xray://stdlib/modules        Stdlib module list
 *   - xray://spec/topic/{name}     Syntax topic by name (template)
 *   - xray://stdlib/{module}       Stdlib module detail (template)
 */

#ifndef XMCP_RESOURCES_H
#define XMCP_RESOURCES_H

#include "../../base/xdefs.h"
#include "xmcp_registry.h"
#include <stddef.h>

/* Forward declarations */
typedef struct XrJsonValue XrJsonValue;
typedef struct XmcpServer XmcpServer;

/* Handle "resources/list" request. */
XR_FUNC XrJsonValue *xmcp_handle_resources_list(XmcpServer *server);

XR_FUNC size_t xmcp_resources_count(void);
XR_FUNC const XmcpResourceDef *xmcp_resources_table(void);

/* Handle "resources/read" request. */
XR_FUNC XrJsonValue *xmcp_handle_resources_read(XmcpServer *server, XrJsonValue *params);

/* Handle "resources/templates/list" request. */
XR_FUNC XrJsonValue *xmcp_handle_resource_templates_list(XmcpServer *server);

XR_FUNC size_t xmcp_resource_templates_count(void);
XR_FUNC const XmcpResourceTemplateDef *xmcp_resource_templates_table(void);

#endif  // XMCP_RESOURCES_H
