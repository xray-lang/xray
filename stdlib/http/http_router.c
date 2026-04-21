/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_router.c - High-performance HTTP router implementation
 *
 * KEY CONCEPT:
 *   Uses Radix Tree for O(k) route matching
 */

#include "../../src/base/xmalloc.h"
#include "http_router.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========== Node Operations ========== */

static XrRouterNode* node_new(const char *path, size_t path_len)
{
    XrRouterNode *node = (XrRouterNode*)xr_calloc(1, sizeof(XrRouterNode));
    if (!node) return NULL;
    
    if (path && path_len > 0) {
        node->path = (char*)xr_malloc(path_len + 1);
        memcpy(node->path, path, path_len);
        node->path[path_len] = '\0';
        node->path_len = path_len;
    }
    
    return node;
}

static void node_free(XrRouterNode *node)
{
    if (!node) return;
    
    // Free child nodes
    for (int i = 0; i < node->child_count; i++) {
        node_free(node->children[i]);
    }
    xr_free(node->children);
    
    node_free(node->param_child);
    node_free(node->wildcard_child);
    
    xr_free(node->path);
    xr_free(node->param_name);
    xr_free(node->prebuilt_response);  // Free prebuilt response
    xr_free(node);
}

static void node_add_child(XrRouterNode *parent, XrRouterNode *child)
{
    if (parent->child_count >= parent->child_cap) {
        int new_cap = parent->child_cap == 0 ? 4 : parent->child_cap * 2;
        parent->children = (XrRouterNode**)xr_realloc(parent->children, 
                                                    sizeof(XrRouterNode*) * new_cap);
        parent->child_cap = new_cap;
    }
    parent->children[parent->child_count++] = child;
}

// Find common prefix length
static size_t common_prefix_len(const char *a, size_t a_len, const char *b, size_t b_len)
{
    size_t max_len = a_len < b_len ? a_len : b_len;
    size_t i = 0;
    while (i < max_len && a[i] == b[i]) {
        i++;
    }
    return i;
}

// Find child node
static XrRouterNode* find_child(XrRouterNode *node, char c)
{
    for (int i = 0; i < node->child_count; i++) {
        if (node->children[i]->path_len > 0 && node->children[i]->path[0] == c) {
            return node->children[i];
        }
    }
    return NULL;
}

/* ========== Route Insertion ========== */

static bool insert_route(XrRouterNode *node, const char *path, size_t path_len,
                         XrRouteHandler handler, void *user_data,
                         const char *static_response, size_t static_response_len,
                         char *prebuilt_response, size_t prebuilt_response_len)
{
    // Empty path: set current node's handler
    if (path_len == 0) {
        node->handler = handler;
        node->user_data = user_data;
        node->static_response = static_response;
        node->static_response_len = static_response_len;
        node->prebuilt_response = prebuilt_response;
        node->prebuilt_response_len = prebuilt_response_len;
        return true;
    }
    
    // Check parameter route :param
    if (path[0] == ':') {
        // Extract parameter name
        size_t param_end = 1;
        while (param_end < path_len && path[param_end] != '/') {
            param_end++;
        }
        
        if (!node->param_child) {
            node->param_child = node_new(NULL, 0);
            node->param_child->is_param = true;
            node->param_child->param_name = (char*)xr_malloc(param_end);
            memcpy(node->param_child->param_name, path + 1, param_end - 1);
            node->param_child->param_name[param_end - 1] = '\0';
            node->param_child->param_name_len = param_end - 1;
        }
        
        if (param_end >= path_len) {
            node->param_child->handler = handler;
            node->param_child->user_data = user_data;
            node->param_child->static_response = static_response;
            node->param_child->static_response_len = static_response_len;
            node->param_child->prebuilt_response = prebuilt_response;
            node->param_child->prebuilt_response_len = prebuilt_response_len;
            return true;
        }
        
        return insert_route(node->param_child, path + param_end, path_len - param_end,
                           handler, user_data, static_response, static_response_len,
                           prebuilt_response, prebuilt_response_len);
    }
    
    // Check wildcard route *wildcard
    if (path[0] == '*') {
        if (!node->wildcard_child) {
            node->wildcard_child = node_new(NULL, 0);
            node->wildcard_child->is_wildcard = true;
            
            if (path_len > 1) {
                node->wildcard_child->param_name = (char*)xr_malloc(path_len);
                memcpy(node->wildcard_child->param_name, path + 1, path_len - 1);
                node->wildcard_child->param_name[path_len - 1] = '\0';
                node->wildcard_child->param_name_len = path_len - 1;
            }
        }
        
        node->wildcard_child->handler = handler;
        node->wildcard_child->user_data = user_data;
        node->wildcard_child->static_response = static_response;
        node->wildcard_child->static_response_len = static_response_len;
        node->wildcard_child->prebuilt_response = prebuilt_response;
        node->wildcard_child->prebuilt_response_len = prebuilt_response_len;
        return true;
    }
    
    // Static route - first check if need to split at : or *
    size_t static_end = 0;
    while (static_end < path_len && path[static_end] != ':' && path[static_end] != '*') {
        static_end++;
    }
    
    // If path contains : or *, only process static part
    if (static_end < path_len && static_end > 0) {
        // Recursively insert static part, then continue with parameter part
        XrRouterNode *child = find_child(node, path[0]);
        
        if (!child) {
            // Create static part node
            child = node_new(path, static_end);
            node_add_child(node, child);
        } else {
            size_t prefix_len = common_prefix_len(child->path, child->path_len, path, static_end);
            if (prefix_len < child->path_len) {
                // Need to split node
                XrRouterNode *split = node_new(child->path + prefix_len, child->path_len - prefix_len);
                split->handler = child->handler;
                split->user_data = child->user_data;
                split->static_response = child->static_response;
                split->static_response_len = child->static_response_len;
                split->prebuilt_response = child->prebuilt_response;
                split->prebuilt_response_len = child->prebuilt_response_len;
                split->children = child->children;
                split->child_count = child->child_count;
                split->child_cap = child->child_cap;
                split->param_child = child->param_child;
                split->wildcard_child = child->wildcard_child;
                
                child->path = (char*)xr_realloc(child->path, prefix_len + 1);
                child->path[prefix_len] = '\0';
                child->path_len = prefix_len;
                child->handler = NULL;
                child->user_data = NULL;
                child->static_response = NULL;
                child->static_response_len = 0;
                child->prebuilt_response = NULL;
                child->prebuilt_response_len = 0;
                child->children = NULL;
                child->child_count = 0;
                child->child_cap = 0;
                child->param_child = NULL;
                child->wildcard_child = NULL;
                
                node_add_child(child, split);
            }
            if (prefix_len < static_end) {
                // Need to continue down
                return insert_route(child, path + prefix_len, path_len - prefix_len,
                                   handler, user_data, static_response, static_response_len,
                                   prebuilt_response, prebuilt_response_len);
            }
        }
        
        // Continue with parameter part
        return insert_route(child, path + static_end, path_len - static_end,
                           handler, user_data, static_response, static_response_len,
                           prebuilt_response, prebuilt_response_len);
    }
    
    XrRouterNode *child = find_child(node, path[0]);
    
    if (!child) {
        // No matching child node, create new node
        child = node_new(path, path_len);
        child->handler = handler;
        child->user_data = user_data;
        child->static_response = static_response;
        child->static_response_len = static_response_len;
        child->prebuilt_response = prebuilt_response;
        child->prebuilt_response_len = prebuilt_response_len;
        node_add_child(node, child);
        return true;
    }
    
    // Found matching child node, calculate common prefix
    size_t prefix_len = common_prefix_len(child->path, child->path_len, path, path_len);
    
    if (prefix_len == child->path_len) {
        // Child node path is prefix of new path
        return insert_route(child, path + prefix_len, path_len - prefix_len,
                           handler, user_data, static_response, static_response_len,
                           prebuilt_response, prebuilt_response_len);
    }
    
    // Need to split node
    XrRouterNode *split = node_new(child->path + prefix_len, child->path_len - prefix_len);
    split->handler = child->handler;
    split->user_data = child->user_data;
    split->static_response = child->static_response;
    split->static_response_len = child->static_response_len;
    split->prebuilt_response = child->prebuilt_response;
    split->prebuilt_response_len = child->prebuilt_response_len;
    split->children = child->children;
    split->child_count = child->child_count;
    split->child_cap = child->child_cap;
    split->param_child = child->param_child;
    split->wildcard_child = child->wildcard_child;
    
    // Update original node
    child->path = (char*)xr_realloc(child->path, prefix_len + 1);
    child->path[prefix_len] = '\0';
    child->path_len = prefix_len;
    child->handler = NULL;
    child->user_data = NULL;
    child->static_response = NULL;
    child->static_response_len = 0;
    child->prebuilt_response = NULL;
    child->prebuilt_response_len = 0;
    child->children = NULL;
    child->child_count = 0;
    child->child_cap = 0;
    child->param_child = NULL;
    child->wildcard_child = NULL;
    
    node_add_child(child, split);
    
    if (prefix_len == path_len) {
        child->handler = handler;
        child->user_data = user_data;
        child->static_response = static_response;
        child->static_response_len = static_response_len;
        child->prebuilt_response = prebuilt_response;
        child->prebuilt_response_len = prebuilt_response_len;
    } else {
        return insert_route(child, path + prefix_len, path_len - prefix_len,
                           handler, user_data, static_response, static_response_len,
                           prebuilt_response, prebuilt_response_len);
    }
    
    return true;
}

/* ========== Route Lookup ========== */

static XrRouteHandler find_route(XrRouterNode *node, const char *path, size_t path_len,
                                  XrRouteParams *params, void **user_data,
                                  const char **static_response, size_t *static_response_len,
                                  const char **prebuilt_response, size_t *prebuilt_response_len)
{
    if (!node) return NULL;
    
    // Empty path: return current node's handler or static response
    if (path_len == 0) {
        // Has handler or prebuilt response (static route) means match success
        if (node->handler || node->prebuilt_response) {
            if (user_data) *user_data = node->user_data;
            if (static_response) *static_response = node->static_response;
            if (static_response_len) *static_response_len = node->static_response_len;
            if (prebuilt_response) *prebuilt_response = node->prebuilt_response;
            if (prebuilt_response_len) *prebuilt_response_len = node->prebuilt_response_len;
            // Static route has no handler, return special marker
            return node->handler ? node->handler : (XrRouteHandler)1;
        }
        return NULL;
    }
    
    // Try static match
    XrRouterNode *child = find_child(node, path[0]);
    if (child) {
        size_t prefix_len = common_prefix_len(child->path, child->path_len, path, path_len);
        if (prefix_len == child->path_len) {
            XrRouteHandler h = find_route(child, path + prefix_len, path_len - prefix_len,
                                          params, user_data, static_response, static_response_len,
                                          prebuilt_response, prebuilt_response_len);
            if (h) return h;
        }
    }
    
    // Try parameter match
    if (node->param_child) {
        // Extract parameter value (to next / or path end)
        size_t value_end = 0;
        while (value_end < path_len && path[value_end] != '/') {
            value_end++;
        }
        
        XrRouteHandler h = find_route(node->param_child, path + value_end, path_len - value_end,
                                      params, user_data, static_response, static_response_len,
                                      prebuilt_response, prebuilt_response_len);
        if (h) {
            // Add parameter
            if (params && params->count < XR_ROUTER_MAX_PARAMS) {
                XrRouteParam *p = &params->params[params->count++];
                p->key = node->param_child->param_name;
                p->key_len = node->param_child->param_name_len;
                p->value = path;
                p->value_len = value_end;
            }
            return h;
        }
    }
    
    // Try wildcard match
    if (node->wildcard_child && node->wildcard_child->handler) {
        if (params && params->count < XR_ROUTER_MAX_PARAMS) {
            XrRouteParam *p = &params->params[params->count++];
            p->key = node->wildcard_child->param_name;
            p->key_len = node->wildcard_child->param_name_len;
            p->value = path;
            p->value_len = path_len;
        }
        if (user_data) *user_data = node->wildcard_child->user_data;
        if (static_response) *static_response = node->wildcard_child->static_response;
        if (static_response_len) *static_response_len = node->wildcard_child->static_response_len;
        if (prebuilt_response) *prebuilt_response = node->wildcard_child->prebuilt_response;
        if (prebuilt_response_len) *prebuilt_response_len = node->wildcard_child->prebuilt_response_len;
        return node->wildcard_child->handler;
    }
    
    return NULL;
}

/* ========== Public API ========== */

XrRouter* xr_router_new(void)
{
    XrRouter *router = (XrRouter*)xr_calloc(1, sizeof(XrRouter));
    return router;
}

void xr_router_free(XrRouter *router)
{
    if (!router) return;
    
    for (int i = 0; i <= XR_HTTP_METHOD_UNKNOWN; i++) {
        node_free(router->trees[i]);
    }
    xr_free(router);
}

bool xr_router_add(XrRouter *router, XrHttpMethod method, const char *path,
                   XrRouteHandler handler, void *user_data)
{
    if (!router || !path || !handler) return false;
    if (method < 0 || method > XR_HTTP_METHOD_UNKNOWN) return false;
    
    // Ensure path starts with /
    if (path[0] != '/') return false;
    
    // Create root node
    if (!router->trees[method]) {
        router->trees[method] = node_new("/", 1);
    }
    
    // Insert route (skip leading /)
    return insert_route(router->trees[method], path + 1, strlen(path) - 1,
                       handler, user_data, NULL, 0, NULL, 0);
}

/*
 * Pre-generate complete HTTP response (including headers)
 * Avoid snprintf on every request, directly memcpy instead
 */
static char* prebuild_http_response(const char *body, size_t body_len, size_t *out_len) {
    // Auto-detect Content-Type
    const char *content_type = "text/plain; charset=utf-8";
    if (body_len > 0) {
        if ((body_len >= 9 && strncmp(body, "<!DOCTYPE", 9) == 0) ||
            (body_len >= 5 && strncmp(body, "<html", 5) == 0) ||
            (body_len >= 1 && body[0] == '<')) {
            content_type = "text/html; charset=utf-8";
        } else if (body_len >= 1 && (body[0] == '{' || body[0] == '[')) {
            content_type = "application/json; charset=utf-8";
        }
    }
    
    // Pre-allocate large enough buffer
    size_t header_max = 256;
    char *buf = (char*)xr_malloc(header_max + body_len);
    if (!buf) return NULL;
    
    // Generate complete response
    int header_len = snprintf(buf, header_max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        content_type, body_len);
    
    memcpy(buf + header_len, body, body_len);
    *out_len = header_len + body_len;
    return buf;
}

bool xr_router_add_static(XrRouter *router, XrHttpMethod method, const char *path,
                          const char *response, size_t response_len)
{
    if (!router || !path || !response) return false;
    if (method < 0 || method > XR_HTTP_METHOD_UNKNOWN) return false;
    if (path[0] != '/') return false;
    
    if (!router->trees[method]) {
        router->trees[method] = node_new("/", 1);
    }
    
    // Pre-generate complete HTTP response (including headers)
    size_t prebuilt_len = 0;
    char *prebuilt = prebuild_http_response(response, response_len, &prebuilt_len);
    
    // Insert route, save prebuilt response (static route needs no handler, directly return prebuilt_response)
    return insert_route(router->trees[method], path + 1, strlen(path) - 1,
                       NULL, NULL, response, response_len,
                       prebuilt, prebuilt_len);
}

XrRouteHandler xr_router_find(XrRouter *router, XrHttpMethod method,
                               const char *path, size_t path_len,
                               XrRouteParams *params, void **user_data,
                               const char **static_response, size_t *static_response_len,
                               const char **prebuilt_response, size_t *prebuilt_response_len)
{
    if (!router || !path) return NULL;
    if (method < 0 || method > XR_HTTP_METHOD_UNKNOWN) return NULL;
    
    XrRouterNode *root = router->trees[method];
    if (!root) return NULL;
    
    // Initialize parameters
    if (params) {
        params->count = 0;
    }
    
    // Skip leading /
    if (path_len > 0 && path[0] == '/') {
        path++;
        path_len--;
    }
    
    return find_route(root, path, path_len, params, user_data, 
                     static_response, static_response_len,
                     prebuilt_response, prebuilt_response_len);
}

/*
 * WS upgrade routes live on the GET tree (RFC 6455: upgrade is always GET).
 * Sentinel handler value (XrRouteHandler)2 distinguishes WS from HTTP routes.
 * Caller stores the WS closure in user_data.
 */
bool xr_router_add_websocket(XrRouter *router, const char *path,
                              void *user_data)
{
    if (!router || !path || path[0] != '/') return false;

    if (!router->trees[XR_HTTP_METHOD_GET]) {
        router->trees[XR_HTTP_METHOD_GET] = node_new("/", 1);
    }

    return insert_route(router->trees[XR_HTTP_METHOD_GET],
                        path + 1, strlen(path) - 1,
                        XR_ROUTE_HANDLER_WEBSOCKET, user_data,
                        NULL, 0, NULL, 0);
}
