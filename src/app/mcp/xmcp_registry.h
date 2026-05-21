/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_registry.h - MCP feature registry
 */

#ifndef XMCP_REGISTRY_H
#define XMCP_REGISTRY_H

#include "../../base/xdefs.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct XrJsonValue XrJsonValue;
typedef struct XmcpServer XmcpServer;

typedef XrJsonValue *(*XmcpToolHandler)(XmcpServer *server, XrJsonValue *args);
typedef XrJsonValue *(*XmcpSchemaBuilder)(void);

typedef struct XmcpToolDef {
    const char *name;
    const char *title;
    const char *description;
    XmcpSchemaBuilder build_schema;
    XmcpToolHandler handler;
    bool read_only;
    bool open_world;
} XmcpToolDef;

typedef struct XmcpRegistry {
    const XmcpToolDef *tools;
    size_t tool_count;
    size_t resource_count;
    size_t resource_template_count;
    size_t prompt_count;
} XmcpRegistry;

XR_FUNC void xmcp_registry_init(XmcpRegistry *registry);
XR_FUNC const XmcpToolDef *xmcp_registry_find_tool(const XmcpRegistry *registry, const char *name);
XR_FUNC const XmcpToolDef *xmcp_registry_tool_at(const XmcpRegistry *registry, size_t index);
XR_FUNC bool xmcp_registry_has_tools(const XmcpRegistry *registry);
XR_FUNC bool xmcp_registry_has_resources(const XmcpRegistry *registry);
XR_FUNC bool xmcp_registry_has_prompts(const XmcpRegistry *registry);

#endif  // XMCP_REGISTRY_H
