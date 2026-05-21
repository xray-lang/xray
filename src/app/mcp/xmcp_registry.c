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
#include <string.h>

XR_FUNC void xmcp_registry_options_default(XmcpRegistryOptions *options) {
    XR_DCHECK(options != NULL, "xmcp_registry_options_default: NULL options");
    options->enable_runner = false;
}

static bool xmcp_registry_tool_enabled(const XmcpToolDef *tool,
                                       const XmcpRegistryOptions *options) {
    XR_DCHECK(tool != NULL, "xmcp_registry_tool_enabled: NULL tool");
    XR_DCHECK(options != NULL, "xmcp_registry_tool_enabled: NULL options");

    if (tool->toolset == XMCP_TOOLSET_RUNNER)
        return options->enable_runner;
    return true;
}

XR_FUNC void xmcp_registry_init(XmcpRegistry *registry, const XmcpRegistryOptions *options) {
    XR_DCHECK(registry != NULL, "xmcp_registry_init: NULL registry");
    XR_DCHECK(options != NULL, "xmcp_registry_init: NULL options");

    memset(registry, 0, sizeof(*registry));
    registry->resources = xmcp_resources_table();
    registry->resource_templates = xmcp_resource_templates_table();
    registry->prompts = xmcp_prompts_table();
    registry->resource_count = xmcp_resources_count();
    registry->resource_template_count = xmcp_resource_templates_count();
    registry->prompt_count = xmcp_prompts_count();

    const XmcpToolDef *tools = xmcp_tools_table();
    size_t total = xmcp_tools_count();
    for (size_t i = 0; i < total; i++) {
        const XmcpToolDef *tool = &tools[i];
        if (!xmcp_registry_tool_enabled(tool, options))
            continue;
        XR_DCHECK(registry->tool_count < XMCP_REGISTRY_MAX_TOOLS,
                  "xmcp_registry_init: too many tools");
        registry->tools[registry->tool_count++] = tool;
    }
}

XR_FUNC const XmcpToolDef *xmcp_registry_tool_at(const XmcpRegistry *registry, size_t index) {
    XR_DCHECK(registry != NULL, "xmcp_registry_tool_at: NULL registry");
    XR_DCHECK(index < registry->tool_count, "xmcp_registry_tool_at: index out of range");
    XR_DCHECK(registry->tools[index] != NULL, "xmcp_registry_tool_at: NULL tool");
    return registry->tools[index];
}

XR_FUNC const XmcpToolDef *xmcp_registry_find_tool(const XmcpRegistry *registry, const char *name) {
    XR_DCHECK(registry != NULL, "xmcp_registry_find_tool: NULL registry");
    XR_DCHECK(name != NULL, "xmcp_registry_find_tool: NULL name");

    if (registry->tool_count == 0)
        return NULL;
    for (size_t i = 0; i < registry->tool_count; i++) {
        const XmcpToolDef *tool = registry->tools[i];
        if (tool->name && strcmp(tool->name, name) == 0)
            return tool;
    }
    return NULL;
}

XR_FUNC const XmcpResourceDef *xmcp_registry_resource_at(const XmcpRegistry *registry,
                                                         size_t index) {
    XR_DCHECK(registry != NULL, "xmcp_registry_resource_at: NULL registry");
    XR_DCHECK(registry->resources != NULL, "xmcp_registry_resource_at: NULL resources");
    XR_DCHECK(index < registry->resource_count, "xmcp_registry_resource_at: index out of range");
    return &registry->resources[index];
}

XR_FUNC const XmcpResourceTemplateDef *
xmcp_registry_resource_template_at(const XmcpRegistry *registry, size_t index) {
    XR_DCHECK(registry != NULL, "xmcp_registry_resource_template_at: NULL registry");
    XR_DCHECK(registry->resource_templates != NULL,
              "xmcp_registry_resource_template_at: NULL templates");
    XR_DCHECK(index < registry->resource_template_count,
              "xmcp_registry_resource_template_at: index out of range");
    return &registry->resource_templates[index];
}

XR_FUNC const XmcpPromptDef *xmcp_registry_prompt_at(const XmcpRegistry *registry, size_t index) {
    XR_DCHECK(registry != NULL, "xmcp_registry_prompt_at: NULL registry");
    XR_DCHECK(registry->prompts != NULL, "xmcp_registry_prompt_at: NULL prompts");
    XR_DCHECK(index < registry->prompt_count, "xmcp_registry_prompt_at: index out of range");
    return &registry->prompts[index];
}

XR_FUNC const XmcpPromptDef *xmcp_registry_find_prompt(const XmcpRegistry *registry,
                                                       const char *name) {
    XR_DCHECK(registry != NULL, "xmcp_registry_find_prompt: NULL registry");
    XR_DCHECK(name != NULL, "xmcp_registry_find_prompt: NULL name");

    if (!registry->prompts || registry->prompt_count == 0)
        return NULL;
    for (size_t i = 0; i < registry->prompt_count; i++) {
        const XmcpPromptDef *prompt = &registry->prompts[i];
        if (prompt->name && strcmp(prompt->name, name) == 0)
            return prompt;
    }
    return NULL;
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
