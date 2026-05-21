/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_prompts.h - MCP prompt handlers
 *
 * KEY CONCEPT:
 *   Predefined prompt templates that help AI assistants interact
 *   effectively with Xray code. Prompts embed language knowledge
 *   into system messages for context-aware AI assistance.
 */

#ifndef XMCP_PROMPTS_H
#define XMCP_PROMPTS_H

#include "../../base/xdefs.h"
#include "xmcp_registry.h"
#include <stddef.h>

/* Forward declarations */
typedef struct XrJsonValue XrJsonValue;
typedef struct XmcpServer XmcpServer;

/* Handle "prompts/list" request. */
XR_FUNC XrJsonValue *xmcp_handle_prompts_list(XmcpServer *server);

XR_FUNC size_t xmcp_prompts_count(void);
XR_FUNC const XmcpPromptDef *xmcp_prompts_table(void);

/* Handle "prompts/get" request. */
XR_FUNC XrJsonValue *xmcp_handle_prompts_get(XmcpServer *server, XrJsonValue *params);

#endif  // XMCP_PROMPTS_H
