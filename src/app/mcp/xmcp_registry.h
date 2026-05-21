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

typedef struct XmcpRegistry {
    size_t tool_count;
    size_t resource_count;
    size_t resource_template_count;
    size_t prompt_count;
} XmcpRegistry;

XR_FUNC void xmcp_registry_init(XmcpRegistry *registry);
XR_FUNC bool xmcp_registry_has_tools(const XmcpRegistry *registry);
XR_FUNC bool xmcp_registry_has_resources(const XmcpRegistry *registry);
XR_FUNC bool xmcp_registry_has_prompts(const XmcpRegistry *registry);

#endif  // XMCP_REGISTRY_H
