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

#define XMCP_REGISTRY_MAX_TOOLS 16

typedef enum XmcpToolset {
    XMCP_TOOLSET_CORE = 0,
    XMCP_TOOLSET_KNOWLEDGE,
    XMCP_TOOLSET_RUNNER
} XmcpToolset;

/* Per-call context passed by tools/call dispatch to a handler. Lives on the
 * dispatcher stack; handlers MUST NOT retain pointers beyond the call. */
typedef struct XmcpCallContext {
    int64_t progress_token; /* -1 when the client did not request progress */
} XmcpCallContext;

typedef XrJsonValue *(*XmcpToolHandler)(XmcpServer *server, const XmcpCallContext *ctx,
                                        XrJsonValue *args);
typedef XrJsonValue *(*XmcpSchemaBuilder)(void);

typedef struct XmcpToolDef {
    const char *name;
    const char *title;
    const char *description;
    XmcpToolset toolset;
    XmcpSchemaBuilder build_schema;
    XmcpSchemaBuilder build_output_schema;
    XmcpToolHandler handler;
    bool read_only;
    bool open_world;
} XmcpToolDef;

#define XMCP_PROMPT_ARG_MAX 4

typedef struct XmcpResourceDef {
    const char *uri;
    const char *name;
    const char *description;
    const char *mime_type;
} XmcpResourceDef;

typedef struct XmcpResourceTemplateDef {
    const char *uri_template;
    const char *name;
    const char *description;
    const char *mime_type;
} XmcpResourceTemplateDef;

typedef struct XmcpPromptArgDef {
    const char *name;
    const char *description;
    bool required;
} XmcpPromptArgDef;

typedef struct XmcpPromptDef {
    const char *name;
    const char *description;
    int arg_count;
    XmcpPromptArgDef args[XMCP_PROMPT_ARG_MAX];
} XmcpPromptDef;

typedef struct XmcpRegistryOptions {
    bool enable_runner;
} XmcpRegistryOptions;

typedef struct XmcpRegistry {
    const XmcpToolDef *tools[XMCP_REGISTRY_MAX_TOOLS];
    const XmcpResourceDef *resources;
    const XmcpResourceTemplateDef *resource_templates;
    const XmcpPromptDef *prompts;
    size_t tool_count;
    size_t resource_count;
    size_t resource_template_count;
    size_t prompt_count;
} XmcpRegistry;

XR_FUNC void xmcp_registry_options_default(XmcpRegistryOptions *options);
XR_FUNC void xmcp_registry_init(XmcpRegistry *registry, const XmcpRegistryOptions *options);
XR_FUNC const XmcpToolDef *xmcp_registry_find_tool(const XmcpRegistry *registry, const char *name);
XR_FUNC const XmcpToolDef *xmcp_registry_tool_at(const XmcpRegistry *registry, size_t index);
XR_FUNC const XmcpResourceDef *xmcp_registry_resource_at(const XmcpRegistry *registry,
                                                         size_t index);
XR_FUNC const XmcpResourceTemplateDef *
xmcp_registry_resource_template_at(const XmcpRegistry *registry, size_t index);
XR_FUNC const XmcpPromptDef *xmcp_registry_find_prompt(const XmcpRegistry *registry,
                                                       const char *name);
XR_FUNC const XmcpPromptDef *xmcp_registry_prompt_at(const XmcpRegistry *registry, size_t index);
XR_FUNC bool xmcp_registry_has_tools(const XmcpRegistry *registry);
XR_FUNC bool xmcp_registry_has_resources(const XmcpRegistry *registry);
XR_FUNC bool xmcp_registry_has_prompts(const XmcpRegistry *registry);

#endif  // XMCP_REGISTRY_H
