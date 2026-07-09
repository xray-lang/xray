/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_internal.h - Private HTTP native module boundary
 */

#ifndef XR_STDLIB_HTTP_INTERNAL_H
#define XR_STDLIB_HTTP_INTERNAL_H

#include "http.h"
#include "http2.h"
#include "http_client.h"
#include "http_parser.h"
#include "../net/conn_pool.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/runtime/xisolate_internal.h"

struct XrClosure;

#define XR_ROUTER_MAX_PARAMS 16

typedef struct XrH2Pool XrH2Pool;

typedef struct XrH2Request {
    const char *method;
    const char *path;
    const char *authority;
    const char *scheme;
    XrHttpHeader *headers;
    int header_count;
    const char *body;
    size_t body_len;
} XrH2Request;

typedef struct XrH2Response {
    int status;
    XrHttpHeader *headers;
    int header_count;
    char *body;
    size_t body_len;
    XrH2ErrorCode error;
    char *error_msg;
} XrH2Response;

XrH2Pool *http2_client_pool_create(void);
void http2_client_pool_destroy(XrH2Pool *pool);
XrH2Response *http2_client_request(XrH2Pool *pool, const char *url, const XrH2Request *req);
void http2_client_response_free(XrH2Response *resp);

typedef struct {
    const char *key;
    size_t key_len;
    const char *value;
    size_t value_len;
} XrRouteParam;

typedef struct {
    XrRouteParam params[XR_ROUTER_MAX_PARAMS];
    int count;
} XrRouteParams;

typedef enum {
    XR_ROUTE_NONE = 0,
    XR_ROUTE_DYNAMIC,
    XR_ROUTE_STATIC,
    XR_ROUTE_WEBSOCKET,
} XrRouteKind;

typedef struct XrRouterNode {
    char *path;
    size_t path_len;

    XrRouteKind kind;
    void *user_data;

    char *static_response;
    size_t static_response_len;

    struct XrRouterNode **children;
    int child_count;
    int child_cap;

    struct XrRouterNode *param_child;
    struct XrRouterNode *wildcard_child;

    char *param_name;
    size_t param_name_len;
} XrRouterNode;

typedef struct {
    XrRouterNode *trees[XR_HTTP_METHOD_UNKNOWN + 1];
} XrRouter;

XrRouter *http_router_new(void);
void http_router_free(XrRouter *router);
bool http_router_add(XrRouter *router, XrHttpMethod method, const char *path, void *user_data);
bool http_router_add_static(XrRouter *router, XrHttpMethod method, const char *path,
                            const char *response, size_t response_len);
XrRouteKind http_router_find(XrRouter *router, XrHttpMethod method, const char *path,
                             size_t path_len, XrRouteParams *params, void **user_data,
                             const char **static_response, size_t *static_response_len);
bool http_router_add_websocket(XrRouter *router, const char *path, void *user_data);

typedef struct XrHttpServer {
    int listen_fd;
    volatile bool running;

    XrRouter *router;

    struct XrClosure **route_closures;
    int route_closure_count;
    int route_closure_capacity;
} XrHttpServer;

// Per-Isolate HTTP module context, stored in module's native_handle.
typedef struct XrHttpContext {
    /* === Server === */
    struct XrHttpServer *server;
    XrVMRuntime *server_isolate;

    /* === Server Runtime State === */
    _Atomic int current_conns;

    /* === Connection Pools (per-isolate) === */
    XrConnPool *conn_pool;
    XrH2Pool *h2_client_pool;

} XrHttpContext;

XrHttpContext *http_get_context(XrVMRuntime *X);

XrHttpServer *http_server_new(void);
void http_server_free(XrHttpServer *server);
void http_server_route(XrHttpServer *server, XrHttpMethod method, const char *path,
                       struct XrClosure *handler);
void http_server_stop(XrHttpServer *server);

XrCFuncResult http_listen_impl(XrVMRuntime *X, XrValue *args, int nargs, XrValue *result);

#endif
