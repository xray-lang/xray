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
 *   Capabilities are derived from the server feature registry.
 *   MCP protocol version 2025-03-26.
 */

#include "xmcp_json.h"
#include "xmcp_protocol.h"
#include "xmcp_server.h"
#include "../../base/xjson.h"
#include "../../base/xchecks.h"

static XrJsonValue *build_capabilities(XmcpServer *server) {
    XR_DCHECK(server != NULL, "build_capabilities: NULL server");
    XrJsonValue *caps = xjson_new_object();

    if (xmcp_registry_has_tools(&server->registry)) {
        XrJsonValue *tc = xjson_new_object();
        XMCP_JSON_BOOL(tc, "listChanged", true);
        xmcp_json_set(caps, "tools", tc);
    }

    if (xmcp_registry_has_resources(&server->registry)) {
        XrJsonValue *rc = xjson_new_object();
        XMCP_JSON_BOOL(rc, "listChanged", false);
        xmcp_json_set(caps, "resources", rc);
    }

    if (xmcp_registry_has_prompts(&server->registry)) {
        XrJsonValue *pc = xjson_new_object();
        XMCP_JSON_BOOL(pc, "listChanged", true);
        xmcp_json_set(caps, "prompts", pc);
    }

    /* Logging is always available */
    xmcp_json_set(caps, "logging", xjson_new_object());

    return caps;
}

XR_FUNC XrJsonValue *xmcp_handle_initialize(XmcpServer *server, XrJsonValue *params, XmcpRpcError *error) {
    (void) error;
    XR_DCHECK(server != NULL, "xmcp_handle_initialize: NULL server");

    /* Log connecting client for diagnostics and future version negotiation. */
    const char *client_name = NULL;
    XrJsonValue *client_info = params ? xjson_get_object(params, "clientInfo") : NULL;
    if (client_info)
        client_name = xjson_get_string(client_info, "name");
    fprintf(stderr, "[xray-mcp] initialize from client: %s\n",
            client_name ? client_name : "(unknown)");

    if (server->lifecycle_state == XMCP_LIFECYCLE_CREATED)
        server->lifecycle_state = XMCP_LIFECYCLE_INITIALIZE_SENT;

    XrJsonValue *result = xjson_new_object();

    /* Protocol version */
    XMCP_JSON_STRING(result, "protocolVersion", XMCP_PROTOCOL_VERSION);

    /* Dynamically inferred capabilities */
    xmcp_json_set(result, "capabilities", build_capabilities(server));

    /* Server info */
    XrJsonValue *info = xjson_new_object();
    XMCP_JSON_STRING(info, "name", XMCP_SERVER_NAME);
    XMCP_JSON_STRING(info, "version", XMCP_SERVER_VERSION);
    xmcp_json_set(result, "serverInfo", info);

    return xmcp_json_finish(result);
}
