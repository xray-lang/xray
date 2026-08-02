/* Private HTTP/1.x native boundary. HTTP/2 is owned by http2. */
#ifndef XR_STDLIB_HTTP_INTERNAL_H
#define XR_STDLIB_HTTP_INTERNAL_H

#include "http.h"
#include "http_client_internal.h"
#include "http_parser_internal.h"
#include "http_conn_pool.h"
#include "../../src/runtime/xisolate_internal.h"

// Per-Isolate HTTP/1.x context, stored in the module native handle.
typedef struct XrHttpContext {
    XrHttpConnPool *http_conn_pool;
} XrHttpContext;

XrHttpContext *http_get_context(XrVMRuntime *X);

#endif
