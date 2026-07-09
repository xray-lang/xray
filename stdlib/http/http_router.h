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

#include "../../src/base/xdefs.h"
#include "http_parser.h"
#include <stdbool.h>

/* ========== Route Parameters ========== */

#define XR_ROUTER_MAX_PARAMS 16  // Max parameter count

typedef struct {
    const char *key;  // Parameter name (without :)
    size_t key_len;
    const char *value;  // Parameter value
    size_t value_len;
} XrRouteParam;

typedef struct {
    XrRouteParam params[XR_ROUTER_MAX_PARAMS];
    int count;
} XrRouteParams;

/* ========== Route Endpoint ========== */

typedef enum {
    XR_ROUTE_NONE = 0,
    XR_ROUTE_DYNAMIC,
    XR_ROUTE_STATIC,
    XR_ROUTE_WEBSOCKET,
} XrRouteKind;

/* ========== Route Node ========== */

typedef struct XrRouterNode {
    char *path;  // Path segment
    size_t path_len;

    XrRouteKind kind;
    void *user_data;  // Closure or websocket user data.

    // Static response body owned by this route node.
    char *static_response;
    size_t static_response_len;

    struct XrRouterNode **children;  // Child node array
    int child_count;
    int child_cap;

    // Special nodes
    struct XrRouterNode *param_child;     // :param child node
    struct XrRouterNode *wildcard_child;  // *wildcard child node

    char *param_name;  // Parameter name (for :param node)
    size_t param_name_len;
} XrRouterNode;

/* ========== Router ========== */

typedef struct {
    XrRouterNode *trees[XR_HTTP_METHOD_UNKNOWN + 1];  // One tree per method
} XrRouter;

/* ========== API ========== */

/*
 * Create router
 */
XR_FUNC XrRouter *xr_router_new(void);

/*
 * Free router
 */
XR_FUNC void xr_router_free(XrRouter *router);

/*
 * Add route
 *
 * Supported path formats:
 *   /static/path      - Static path
 *   /user/:id         - Parameter path
 *   /files/{*filepath}  - Wildcard path
 */
XR_FUNC bool xr_router_add(XrRouter *router, XrHttpMethod method, const char *path,
                           void *user_data);

/*
 * Add static response route
 */
XR_FUNC bool xr_router_add_static(XrRouter *router, XrHttpMethod method, const char *path,
                                  const char *response, size_t response_len);

/*
 * Find route
 *
 * Returns: matched route kind, XR_ROUTE_NONE if not found
 * params: output parameters, can be NULL
 */
XR_FUNC XrRouteKind xr_router_find(XrRouter *router, XrHttpMethod method, const char *path,
                                   size_t path_len, XrRouteParams *params, void **user_data,
                                   const char **static_response, size_t *static_response_len);

/*
 * Add WebSocket upgrade route (handler receives upgraded WS connection).
 */
XR_FUNC bool xr_router_add_websocket(XrRouter *router, const char *path, void *user_data);

#endif
