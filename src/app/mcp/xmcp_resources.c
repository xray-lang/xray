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
 *   Static resources + URI template resources:
 *   - xray://spec/cheatsheet    Full language cheatsheet
 *   - xray://spec/concurrency   Concurrency model reference
 *   - xray://stdlib/modules     Complete stdlib module list
 *   - xray://spec/topic/{name}  Syntax topic by name (template)
 *   - xray://stdlib/{module}    Stdlib module detail (template)
 */

#include "xmcp_resources.h"
#include "xmcp_protocol.h"
#include "xmcp_server.h"
#include "xmcp_knowledge.h"
#include "../../base/xjson.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <string.h>
#include <stdio.h>

/* --------------------------------------------------------------------------
 * Static resource definitions
 * -------------------------------------------------------------------------- */

static const XmcpResourceDef RESOURCES[] = {
    {"xray://spec/cheatsheet", "Xray Language Cheatsheet",
     "Complete syntax quick reference for the Xray programming language", "text/markdown"},
    {"xray://spec/concurrency", "Xray Concurrency Model",
     "Detailed reference for Xray's concurrency safety model, channels, and coroutines",
     "text/markdown"},
    {"xray://stdlib/modules", "Xray Standard Library",
     "List of all standard library modules with descriptions", "text/markdown"},
    {NULL, NULL, NULL, NULL}};

/* --------------------------------------------------------------------------
 * Resource template definitions
 * -------------------------------------------------------------------------- */

static const XmcpResourceTemplateDef TEMPLATES[] = {
    {"xray://spec/topic/{name}", "Xray Syntax Topic",
     "Look up a specific Xray language syntax topic by name. "
     "Topics: variables, types, functions, control_flow, class, struct, "
     "interface, enum, generics, collections, string, channel, coroutine, "
     "concurrency_rules, modules, testing, operators, builtin_functions.",
     "text/markdown"},
    {"xray://stdlib/{module}", "Xray Stdlib Module",
     "Detailed information about a specific standard library module.", "text/markdown"},
    {NULL, NULL, NULL, NULL}};

XR_FUNC size_t xmcp_resources_count(void) {
    size_t count = 0;
    while (RESOURCES[count].uri != NULL)
        count++;
    return count;
}

XR_FUNC const XmcpResourceDef *xmcp_resources_table(void) {
    return RESOURCES;
}

XR_FUNC size_t xmcp_resource_templates_count(void) {
    size_t count = 0;
    while (TEMPLATES[count].uri_template != NULL)
        count++;
    return count;
}

XR_FUNC const XmcpResourceTemplateDef *xmcp_resource_templates_table(void) {
    return TEMPLATES;
}

/* --------------------------------------------------------------------------
 * URI template matching (RFC 6570 Level 1: simple {variable})
 * -------------------------------------------------------------------------- */

/* Match a URI against a template. If matched, extract the variable value.
 * Returns the extracted variable string or NULL if no match.
 * The returned pointer points into the `uri` string (no allocation). */
static const char *match_template(const char *uri, const char *tmpl, int *var_len) {
    XR_DCHECK(uri != NULL, "match_template: NULL uri");
    XR_DCHECK(tmpl != NULL, "match_template: NULL tmpl");
    XR_DCHECK(var_len != NULL, "match_template: NULL var_len");

    /* Find '{' in template */
    const char *lbrace = strchr(tmpl, '{');
    if (!lbrace)
        return NULL;

    /* Prefix must match */
    size_t prefix_len = (size_t) (lbrace - tmpl);
    if (strncmp(uri, tmpl, prefix_len) != 0)
        return NULL;

    /* Find '}' in template */
    const char *rbrace = strchr(lbrace, '}');
    if (!rbrace)
        return NULL;

    /* Extract the variable value from URI */
    const char *var_start = uri + prefix_len;

    /* Suffix after '}' must match */
    const char *suffix = rbrace + 1;
    size_t suffix_len = strlen(suffix);

    if (suffix_len == 0) {
        /* No suffix: rest of URI is the variable */
        *var_len = (int) strlen(var_start);
    } else {
        /* Find suffix in remaining URI */
        const char *suf_pos = strstr(var_start, suffix);
        if (!suf_pos)
            return NULL;
        *var_len = (int) (suf_pos - var_start);
    }

    /* Variable must be non-empty */
    if (*var_len <= 0)
        return NULL;
    return var_start;
}

/* --------------------------------------------------------------------------
 * Template resource readers
 * -------------------------------------------------------------------------- */

/* Read xray://spec/topic/{name} — returns topic content from knowledge base */
static const char *read_topic_resource(XmcpServer *server, const char *name, int name_len) {
    if (!server->knowledge)
        return NULL;

    /* Copy name to stack for NUL termination */
    char buf[128];
    if (name_len >= (int) sizeof(buf))
        return NULL;
    memcpy(buf, name, (size_t) name_len);
    buf[name_len] = '\0';

    return xmcp_knowledge_lookup_topic(server->knowledge, buf);
}

/* Read xray://stdlib/{module} — returns module info from knowledge base */
static char *read_stdlib_resource(XmcpServer *server, const char *module, int module_len) {
    if (!server->knowledge)
        return NULL;

    char buf[128];
    if (module_len >= (int) sizeof(buf))
        return NULL;
    memcpy(buf, module, (size_t) module_len);
    buf[module_len] = '\0';

    return xmcp_knowledge_search_stdlib(server->knowledge, buf, buf, NULL);
}

/* --------------------------------------------------------------------------
 * resources/list handler
 * -------------------------------------------------------------------------- */

XrJsonValue *xmcp_handle_resources_list(XmcpServer *server, XmcpRpcError *error) {
    XR_DCHECK(server != NULL, "xmcp_handle_resources_list: NULL server");
    (void) error;

    XrJsonValue *result = xjson_new_object();
    XrJsonValue *resources = xjson_new_array();

    for (size_t i = 0; i < server->registry.resource_count; i++) {
        const XmcpResourceDef *resource = xmcp_registry_resource_at(&server->registry, i);
        XrJsonValue *res = xjson_new_object();
        XJSON_SET_STRING(res, "uri", resource->uri);
        XJSON_SET_STRING(res, "name", resource->name);
        XJSON_SET_STRING(res, "description", resource->description);
        XJSON_SET_STRING(res, "mimeType", resource->mime_type);
        xjson_array_push(resources, res);
    }

    xjson_object_set(result, "resources", resources);
    return result;
}

/* --------------------------------------------------------------------------
 * resources/templates/list handler
 * -------------------------------------------------------------------------- */

XrJsonValue *xmcp_handle_resource_templates_list(XmcpServer *server, XmcpRpcError *error) {
    XR_DCHECK(server != NULL, "xmcp_handle_resource_templates_list: NULL server");
    (void) error;

    XrJsonValue *result = xjson_new_object();
    XrJsonValue *templates = xjson_new_array();

    for (size_t i = 0; i < server->registry.resource_template_count; i++) {
        const XmcpResourceTemplateDef *resource_template =
            xmcp_registry_resource_template_at(&server->registry, i);
        XrJsonValue *t = xjson_new_object();
        XJSON_SET_STRING(t, "uriTemplate", resource_template->uri_template);
        XJSON_SET_STRING(t, "name", resource_template->name);
        XJSON_SET_STRING(t, "description", resource_template->description);
        XJSON_SET_STRING(t, "mimeType", resource_template->mime_type);
        xjson_array_push(templates, t);
    }

    xjson_object_set(result, "resourceTemplates", templates);
    return result;
}

/* --------------------------------------------------------------------------
 * resources/read handler (static + template URIs)
 * -------------------------------------------------------------------------- */

XrJsonValue *xmcp_handle_resources_read(XmcpServer *server, XrJsonValue *params,
                                        XmcpRpcError *error) {
    XR_DCHECK(server != NULL, "xmcp_handle_resources_read: NULL server");
    XR_DCHECK(error != NULL, "xmcp_handle_resources_read: NULL error");

    if (!params || !xjson_is_object(params)) {
        error->code = XMCP_ERR_INVALID_PARAMS;
        snprintf(error->message, sizeof(error->message),
                 "resources/read: params must be an object");
        return NULL;
    }

    const char *uri = xjson_get_string(params, "uri");
    if (!uri || uri[0] == '\0') {
        error->code = XMCP_ERR_INVALID_PARAMS;
        snprintf(error->message, sizeof(error->message), "resources/read: 'uri' is required");
        return NULL;
    }

    const char *text = NULL;
    char *dyn_text = NULL; /* dynamically allocated text (must be freed) */
    const char *mime = "text/markdown";

    /* Try static resources first */
    if (strcmp(uri, "xray://spec/cheatsheet") == 0) {
        text = xmcp_knowledge_get_cheatsheet();
    } else if (strcmp(uri, "xray://spec/concurrency") == 0) {
        text = xmcp_knowledge_get_concurrency();
    } else if (strcmp(uri, "xray://stdlib/modules") == 0) {
        text = xmcp_knowledge_get_stdlib_list();
    }

    /* Try template resources if no static match */
    if (!text) {
        int var_len = 0;
        const char *var = match_template(uri, "xray://spec/topic/{name}", &var_len);
        if (var) {
            text = read_topic_resource(server, var, var_len);
        }
    }
    if (!text && !dyn_text) {
        int var_len = 0;
        const char *var = match_template(uri, "xray://stdlib/{module}", &var_len);
        if (var) {
            dyn_text = read_stdlib_resource(server, var, var_len);
        }
    }

    const char *final_text = text ? text : dyn_text;
    if (!final_text) {
        error->code = XMCP_ERR_INVALID_PARAMS;
        snprintf(error->message, sizeof(error->message), "resources/read: unknown uri '%s'", uri);
        xr_free(dyn_text);
        return NULL;
    }

    XrJsonValue *result = xjson_new_object();
    XrJsonValue *contents = xjson_new_array();
    XrJsonValue *item = xjson_new_object();
    XJSON_SET_STRING(item, "uri", uri);
    XJSON_SET_STRING(item, "mimeType", mime);
    xjson_object_set(item, "text", xjson_new_string(final_text));
    xjson_array_push(contents, item);
    xjson_object_set(result, "contents", contents);
    xr_free(dyn_text);
    return result;
}
