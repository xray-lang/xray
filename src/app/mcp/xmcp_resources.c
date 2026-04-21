/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_resources.c - MCP resource implementations
 *
 * KEY CONCEPT:
 *   Three static resources for Phase 1:
 *   - xray://spec/cheatsheet    Full language cheatsheet
 *   - xray://spec/concurrency   Concurrency model reference
 *   - xray://stdlib/modules     Complete stdlib module list
 */

#include "xmcp_resources.h"
#include "xmcp_server.h"
#include "xmcp_knowledge.h"
#include "../lsp/xlsp_json.h"
#include "../../base/xchecks.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Resource definitions
 * -------------------------------------------------------------------------- */

typedef struct {
    const char *uri;
    const char *name;
    const char *description;
    const char *mime_type;
} ResourceDef;

static const ResourceDef RESOURCES[] = {
    {
        "xray://spec/cheatsheet",
        "Xray Language Cheatsheet",
        "Complete syntax quick reference for the Xray programming language",
        "text/markdown"
    },
    {
        "xray://spec/concurrency",
        "Xray Concurrency Model",
        "Detailed reference for Xray's concurrency safety model, channels, and coroutines",
        "text/markdown"
    },
    {
        "xray://stdlib/modules",
        "Xray Standard Library",
        "List of all standard library modules with descriptions",
        "text/markdown"
    },
    {NULL, NULL, NULL, NULL}
};

/* --------------------------------------------------------------------------
 * resources/list handler
 * -------------------------------------------------------------------------- */

XrJsonValue *xmcp_handle_resources_list(XmcpServer *server) {
    (void)server;

    XrJsonValue *result = xlsp_json_new_object();
    XrJsonValue *resources = xlsp_json_new_array();

    for (int i = 0; RESOURCES[i].uri != NULL; i++) {
        XrJsonValue *res = xlsp_json_new_object();
        XLSP_JSON_SET_STRING(res, "uri", RESOURCES[i].uri);
        XLSP_JSON_SET_STRING(res, "name", RESOURCES[i].name);
        XLSP_JSON_SET_STRING(res, "description", RESOURCES[i].description);
        XLSP_JSON_SET_STRING(res, "mimeType", RESOURCES[i].mime_type);
        xlsp_json_array_push(resources, res);
    }

    xlsp_json_object_set(result, "resources", resources);
    return result;
}

/* --------------------------------------------------------------------------
 * resources/read handler
 * -------------------------------------------------------------------------- */

XrJsonValue *xmcp_handle_resources_read(XmcpServer *server, XrJsonValue *params) {
    (void)server;
    XR_DCHECK(params != NULL, "xmcp_handle_resources_read: NULL params");

    const char *uri = xlsp_json_get_string(params, "uri");
    if (!uri) {
        XrJsonValue *r = xlsp_json_new_object();
        XrJsonValue *c = xlsp_json_new_array();
        xlsp_json_object_set(r, "contents", c);
        return r;
    }

    const char *text = NULL;
    const char *mime = "text/markdown";

    if (strcmp(uri, "xray://spec/cheatsheet") == 0) {
        text = xmcp_knowledge_get_cheatsheet();
    } else if (strcmp(uri, "xray://spec/concurrency") == 0) {
        text = xmcp_knowledge_get_concurrency();
    } else if (strcmp(uri, "xray://stdlib/modules") == 0) {
        text = xmcp_knowledge_get_stdlib_list();
    }

    XrJsonValue *result = xlsp_json_new_object();
    XrJsonValue *contents = xlsp_json_new_array();

    if (text) {
        XrJsonValue *item = xlsp_json_new_object();
        XLSP_JSON_SET_STRING(item, "uri", uri);
        XLSP_JSON_SET_STRING(item, "mimeType", mime);
        xlsp_json_object_set(item, "text", xlsp_json_new_string(text));
        xlsp_json_array_push(contents, item);
    }

    xlsp_json_object_set(result, "contents", contents);
    return result;
}
