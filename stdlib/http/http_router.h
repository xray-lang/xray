/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_router.h - High-performance HTTP router
 *
 * KEY CONCEPT:
 *   Radix tree based routing with O(k) lookup. Supports :param and
 *   *wildcard patterns. Separate route tree per HTTP method.
 */

#ifndef XR_STDLIB_HTTP_ROUTER_H
#define XR_STDLIB_HTTP_ROUTER_H

#include "http_parser.h"
#include <stdbool.h>

/* ========== Route Parameters ========== */

#define XR_ROUTER_MAX_PARAMS    16  // Max parameter count

typedef struct {
    const char *key;        // Parameter name (without :)
    size_t key_len;
    const char *value;      // Parameter value
    size_t value_len;
} XrRouteParam;

typedef struct {
    XrRouteParam params[XR_ROUTER_MAX_PARAMS];
    int count;
} XrRouteParams;

/* ========== Route Handler ========== */

struct XrHttpConn;
typedef void (*XrRouteHandler)(struct XrHttpConn *conn, void *user_data, XrRouteParams *params);

/* ========== Route Node ========== */

typedef struct XrRouterNode {
    char *path;                     // Path segment
    size_t path_len;
    
    XrRouteHandler handler;         // Handler function
    void *user_data;                // User data
    
    // Static response (optional, for zero-copy response)
    const char *static_response;
    size_t static_response_len;
    
    // Prebuilt complete HTTP response (including headers, for ultra-fast response)
    char *prebuilt_response;
    size_t prebuilt_response_len;
    
    struct XrRouterNode **children; // Child node array
    int child_count;
    int child_cap;
    
    // Special nodes
    struct XrRouterNode *param_child;    // :param child node
    struct XrRouterNode *wildcard_child; // *wildcard child node
    
    char *param_name;               // Parameter name (for :param node)
    size_t param_name_len;
    
    bool is_param;                  // Is parameter node
    bool is_wildcard;               // Is wildcard node
    bool is_websocket;              // WebSocket upgrade route
} XrRouterNode;

/* ========== Router ========== */

typedef struct {
    XrRouterNode *trees[XR_HTTP_METHOD_UNKNOWN + 1];  // One tree per method
} XrRouter;

/* ========== API ========== */

/*
 * Create router
 */
XrRouter* xr_router_new(void);

/*
 * Free router
 */
void xr_router_free(XrRouter *router);

/*
 * Add route
 *
 * Supported path formats:
 *   /static/path      - Static path
 *   /user/:id         - Parameter path
 *   /files/{*filepath}  - Wildcard path
 */
bool xr_router_add(XrRouter *router, XrHttpMethod method, const char *path,
                   XrRouteHandler handler, void *user_data);

/*
 * Add static response route
 */
bool xr_router_add_static(XrRouter *router, XrHttpMethod method, const char *path,
                          const char *response, size_t response_len);

/*
 * Find route
 *
 * Returns: matched handler, NULL if not found
 * params: output parameters, can be NULL
 * prebuilt_response/prebuilt_response_len: prebuilt complete HTTP response (optional)
 */
XrRouteHandler xr_router_find(XrRouter *router, XrHttpMethod method,
                               const char *path, size_t path_len,
                               XrRouteParams *params, void **user_data,
                               const char **static_response, size_t *static_response_len,
                               const char **prebuilt_response, size_t *prebuilt_response_len);

/*
 * Add WebSocket upgrade route (handler receives upgraded WS connection).
 * Sentinel handler value distinguishes WS from HTTP routes in find results.
 */
#define XR_ROUTE_HANDLER_WEBSOCKET  ((XrRouteHandler)2)

bool xr_router_add_websocket(XrRouter *router, const char *path,
                              void *user_data);

// Convenience macros
#define xr_router_get(r, path, handler, data) \
    xr_router_add(r, XR_HTTP_METHOD_GET, path, handler, data)

#define xr_router_post(r, path, handler, data) \
    xr_router_add(r, XR_HTTP_METHOD_POST, path, handler, data)

#define xr_router_put(r, path, handler, data) \
    xr_router_add(r, XR_HTTP_METHOD_PUT, path, handler, data)

#define xr_router_delete(r, path, handler, data) \
    xr_router_add(r, XR_HTTP_METHOD_DELETE, path, handler, data)

#endif
