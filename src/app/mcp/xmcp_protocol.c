/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_protocol.c - MCP protocol lifecycle handling
 *
 * KEY CONCEPT:
 *   Builds the initialize response with dynamically inferred capabilities.
 *   Capabilities are derived from server feature flags (has_tools, etc.).
 *   MCP protocol version 2025-03-26.
 */

#include "xmcp_protocol.h"
#include "xmcp_server.h"
#include "../../base/xjson.h"
#include "../../base/xchecks.h"

/* Build capabilities object from server feature flags. */
static XrJsonValue *build_capabilities(XmcpServer *server) {
    XR_DCHECK(server != NULL, "build_capabilities: NULL server");
    XrJsonValue *caps = xjson_new_object();

    if (server->has_tools) {
        XrJsonValue *tc = xjson_new_object();
        XJSON_SET_BOOL(tc, "listChanged", true);
        xjson_object_set(caps, "tools", tc);
    }

    if (server->has_resources) {
        XrJsonValue *rc = xjson_new_object();
        XJSON_SET_BOOL(rc, "listChanged", false);
        xjson_object_set(caps, "resources", rc);
    }

    if (server->has_prompts) {
        XrJsonValue *pc = xjson_new_object();
        XJSON_SET_BOOL(pc, "listChanged", true);
        xjson_object_set(caps, "prompts", pc);
    }

    /* Logging is always available */
    xjson_object_set(caps, "logging", xjson_new_object());

    return caps;
}

XrJsonValue *xmcp_handle_initialize(XmcpServer *server, XrJsonValue *params) {
    (void)params;
    XR_DCHECK(server != NULL, "xmcp_handle_initialize: NULL server");

    XrJsonValue *result = xjson_new_object();

    /* Protocol version */
    XJSON_SET_STRING(result, "protocolVersion", XMCP_PROTOCOL_VERSION);

    /* Dynamically inferred capabilities */
    xjson_object_set(result, "capabilities", build_capabilities(server));

    /* Server info */
    XrJsonValue *info = xjson_new_object();
    XJSON_SET_STRING(info, "name", XMCP_SERVER_NAME);
    XJSON_SET_STRING(info, "version", XMCP_SERVER_VERSION);
    xjson_object_set(result, "serverInfo", info);

    return result;
}
