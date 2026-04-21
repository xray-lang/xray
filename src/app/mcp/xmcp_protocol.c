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
 *   Builds the initialize response with server capabilities.
 *   MCP protocol version 2025-03-26.
 */

#include "xmcp_protocol.h"
#include "xmcp_server.h"
#include "../lsp/xlsp_json.h"
#include "../../base/xchecks.h"

XrJsonValue *xmcp_handle_initialize(XmcpServer *server, XrJsonValue *params) {
    (void)params;
    XR_DCHECK(server != NULL, "xmcp_handle_initialize: NULL server");

    XrJsonValue *result = xlsp_json_new_object();

    /* Protocol version */
    XLSP_JSON_SET_STRING(result, "protocolVersion", XMCP_PROTOCOL_VERSION);

    /* Capabilities */
    XrJsonValue *caps = xlsp_json_new_object();
    xlsp_json_object_set(caps, "tools", xlsp_json_new_object());

    XrJsonValue *res_caps = xlsp_json_new_object();
    XLSP_JSON_SET_BOOL(res_caps, "listChanged", false);
    xlsp_json_object_set(caps, "resources", res_caps);

    xlsp_json_object_set(result, "capabilities", caps);

    /* Server info */
    XrJsonValue *info = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(info, "name", XMCP_SERVER_NAME);
    XLSP_JSON_SET_STRING(info, "version", XMCP_SERVER_VERSION);
    xlsp_json_object_set(result, "serverInfo", info);

    return result;
}
