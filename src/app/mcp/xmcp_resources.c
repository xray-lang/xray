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
#include "xmcp_server.h"
#include "xmcp_knowledge.h"
#include "../lsp/xlsp_json.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <string.h>
#include <stdio.h>

/* --------------------------------------------------------------------------
 * Static resource definitions
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
 * Resource template definitions
 * -------------------------------------------------------------------------- */

typedef struct {
    const char *uri_template;
    const char *name;
    const char *description;
    const char *mime_type;
} ResourceTemplateDef;

static const ResourceTemplateDef TEMPLATES[] = {
    {
        "xray://spec/topic/{name}",
        "Xray Syntax Topic",
        "Look up a specific Xray language syntax topic by name. "
        "Topics: variables, types, functions, control_flow, class, struct, "
        "interface, enum, generics, collections, string, channel, coroutine, "
        "concurrency_rules, modules, testing, operators, builtin_functions.",
        "text/markdown"
    },
    {
        "xray://stdlib/{module}",
        "Xray Stdlib Module",
        "Detailed information about a specific standard library module.",
        "text/markdown"
    },
    {NULL, NULL, NULL, NULL}
};

/* --------------------------------------------------------------------------
 * URI template matching (RFC 6570 Level 1: simple {variable})
 * -------------------------------------------------------------------------- */

/* Match a URI against a template. If matched, extract the variable value.
 * Returns the extracted variable string or NULL if no match.
 * The returned pointer points into the `uri` string (no allocation). */
static const char *match_template(const char *uri, const char *tmpl,
                                    int *var_len) {
    XR_DCHECK(uri != NULL, "match_template: NULL uri");
    XR_DCHECK(tmpl != NULL, "match_template: NULL tmpl");
    XR_DCHECK(var_len != NULL, "match_template: NULL var_len");

    /* Find '{' in template */
    const char *lbrace = strchr(tmpl, '{');
    if (!lbrace) return NULL;

    /* Prefix must match */
    size_t prefix_len = (size_t)(lbrace - tmpl);
    if (strncmp(uri, tmpl, prefix_len) != 0) return NULL;

    /* Find '}' in template */
    const char *rbrace = strchr(lbrace, '}');
    if (!rbrace) return NULL;

    /* Extract the variable value from URI */
    const char *var_start = uri + prefix_len;

    /* Suffix after '}' must match */
    const char *suffix = rbrace + 1;
    size_t suffix_len = strlen(suffix);

    if (suffix_len == 0) {
        /* No suffix: rest of URI is the variable */
        *var_len = (int)strlen(var_start);
    } else {
        /* Find suffix in remaining URI */
        const char *suf_pos = strstr(var_start, suffix);
        if (!suf_pos) return NULL;
        *var_len = (int)(suf_pos - var_start);
    }

    /* Variable must be non-empty */
    if (*var_len <= 0) return NULL;
    return var_start;
}

/* --------------------------------------------------------------------------
 * Template resource readers
 * -------------------------------------------------------------------------- */

/* Read xray://spec/topic/{name} — returns topic content from knowledge base */
static const char *read_topic_resource(XmcpServer *server, const char *name,
                                        int name_len) {
    if (!server->knowledge) return NULL;

    /* Copy name to stack for NUL termination */
    char buf[128];
    if (name_len >= (int)sizeof(buf)) return NULL;
    memcpy(buf, name, (size_t)name_len);
    buf[name_len] = '\0';

    return xmcp_knowledge_lookup_topic(server->knowledge, buf);
}

/* Read xray://stdlib/{module} — returns module info from knowledge base */
static char *read_stdlib_resource(XmcpServer *server, const char *module,
                                    int module_len) {
    if (!server->knowledge) return NULL;

    char buf[128];
    if (module_len >= (int)sizeof(buf)) return NULL;
    memcpy(buf, module, (size_t)module_len);
    buf[module_len] = '\0';

    return xmcp_knowledge_search_stdlib(server->knowledge, buf, buf);
}

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
 * resources/templates/list handler
 * -------------------------------------------------------------------------- */

XrJsonValue *xmcp_handle_resource_templates_list(XmcpServer *server) {
    (void)server;

    XrJsonValue *result = xlsp_json_new_object();
    XrJsonValue *templates = xlsp_json_new_array();

    for (int i = 0; TEMPLATES[i].uri_template != NULL; i++) {
        XrJsonValue *t = xlsp_json_new_object();
        XLSP_JSON_SET_STRING(t, "uriTemplate", TEMPLATES[i].uri_template);
        XLSP_JSON_SET_STRING(t, "name", TEMPLATES[i].name);
        XLSP_JSON_SET_STRING(t, "description", TEMPLATES[i].description);
        XLSP_JSON_SET_STRING(t, "mimeType", TEMPLATES[i].mime_type);
        xlsp_json_array_push(templates, t);
    }

    xlsp_json_object_set(result, "resourceTemplates", templates);
    return result;
}

/* --------------------------------------------------------------------------
 * resources/read handler (static + template URIs)
 * -------------------------------------------------------------------------- */

XrJsonValue *xmcp_handle_resources_read(XmcpServer *server, XrJsonValue *params) {
    XR_DCHECK(params != NULL, "xmcp_handle_resources_read: NULL params");

    const char *uri = xlsp_json_get_string(params, "uri");
    if (!uri) {
        XrJsonValue *r = xlsp_json_new_object();
        xlsp_json_object_set(r, "contents", xlsp_json_new_array());
        return r;
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
        const char *var = match_template(uri, "xray://spec/topic/{name}",
                                           &var_len);
        if (var) {
            text = read_topic_resource(server, var, var_len);
        }
    }
    if (!text && !dyn_text) {
        int var_len = 0;
        const char *var = match_template(uri, "xray://stdlib/{module}",
                                           &var_len);
        if (var) {
            dyn_text = read_stdlib_resource(server, var, var_len);
        }
    }

    /* Build result */
    XrJsonValue *result = xlsp_json_new_object();
    XrJsonValue *contents = xlsp_json_new_array();

    const char *final_text = text ? text : dyn_text;
    if (final_text) {
        XrJsonValue *item = xlsp_json_new_object();
        XLSP_JSON_SET_STRING(item, "uri", uri);
        XLSP_JSON_SET_STRING(item, "mimeType", mime);
        xlsp_json_object_set(item, "text", xlsp_json_new_string(final_text));
        xlsp_json_array_push(contents, item);
    }

    xlsp_json_object_set(result, "contents", contents);
    xr_free(dyn_text);
    return result;
}
