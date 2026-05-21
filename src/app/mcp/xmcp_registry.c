/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_registry.c - MCP feature registry
 */

#include "xmcp_registry.h"
#include "xmcp_tools.h"
#include "xmcp_resources.h"
#include "xmcp_prompts.h"
#include "../../base/xchecks.h"

XR_FUNC void xmcp_registry_init(XmcpRegistry *registry) {
    XR_DCHECK(registry != NULL, "xmcp_registry_init: NULL registry");
    registry->tool_count = xmcp_tools_count();
    registry->resource_count = xmcp_resources_count();
    registry->resource_template_count = xmcp_resource_templates_count();
    registry->prompt_count = xmcp_prompts_count();
}

XR_FUNC bool xmcp_registry_has_tools(const XmcpRegistry *registry) {
    XR_DCHECK(registry != NULL, "xmcp_registry_has_tools: NULL registry");
    return registry->tool_count > 0;
}

XR_FUNC bool xmcp_registry_has_resources(const XmcpRegistry *registry) {
    XR_DCHECK(registry != NULL, "xmcp_registry_has_resources: NULL registry");
    return registry->resource_count > 0 || registry->resource_template_count > 0;
}

XR_FUNC bool xmcp_registry_has_prompts(const XmcpRegistry *registry) {
    XR_DCHECK(registry != NULL, "xmcp_registry_has_prompts: NULL registry");
    return registry->prompt_count > 0;
}
