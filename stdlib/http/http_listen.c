/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_listen.c - Pure C HTTP listen + connection handler
 *
 * KEY CONCEPT:
 *   Pure C HTTP server using stackful coroutines. Accept loop and
 *   connection handlers run on private C stacks. Yield/resume is a
 *   stack-pointer swap (~20ns), preserving all locals across IO waits.
 *
 * WHY THIS DESIGN:
 *   - Eliminates VM overhead for accept loop and connection management
 *   - Prebuilt routes never touch the VM (zero allocation fast path)
 *   - Dynamic routes call user closures via xr_stackful_call_closure
 *   - Natural blocking loops instead of continuation state machines
 */

#include "http.h"
#include "http_server.h"
#include "http_parser.h"
#include "http_router.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/coro/xworker.h"
#include "../../src/coro/xsocket.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/coro/xnetpoll.h"
#include "../../src/vm/xvm.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/object/xjson_serde.h"
#include "../../src/runtime/gc/xgc_internal.h"
#include "../../src/base/xchecks.h"
#include "../../src/base/xarena.h"
#include "../../src/base/xmalloc.h"
#include "../net/xnetbuf.h"
#include "../ws/ws.h"
#include "../../src/os/os_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== External Declarations ========== */

extern void xr_coro_spawn(XrayIsolate *X, XrCoroutine *coro);
extern struct XrCoroutine *xr_current_coro(XrayIsolate *X);
extern XrValue xr_string_value(XrString *str);
extern XrString *xr_string_intern(XrayIsolate *X, const char *str, size_t len, uint32_t hash);
extern XrCoroutine *xr_coro_create_cfunc(XrayIsolate *X,
                                         XrCFuncResult (*cfunc)(XrayIsolate *, XrValue *, int,
                                                                XrValue *),
                                         XrValue *args, int argc, const char *name);

/* ========== Pre-built Error Responses ========== */

static const char RESP_400[] = "HTTP/1.1 400 Bad Request\r\n"
                               "Content-Type: text/plain\r\n"
                               "Content-Length: 11\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "Bad Request";

static const char RESP_404[] = "HTTP/1.1 404 Not Found\r\n"
                               "Content-Type: text/plain\r\n"
                               "Content-Length: 9\r\n"
                               "Connection: keep-alive\r\n"
                               "\r\n"
                               "Not Found";

static const char RESP_500[] = "HTTP/1.1 500 Internal Server Error\r\n"
                               "Content-Type: text/plain\r\n"
                               "Content-Length: 21\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "Internal Server Error";

static const char RESP_413[] = "HTTP/1.1 413 Payload Too Large\r\n"
                               "Content-Type: text/plain\r\n"
                               "Content-Length: 17\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "Payload Too Large";

static const char RESP_503[] = "HTTP/1.1 503 Service Unavailable\r\n"
                               "Content-Length: 19\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "Service Unavailable";

#define MAX_BODY_SIZE 1048576
#define CONN_YIELD_BATCH 32
#define CONN_READ_BUF_SIZE 8192

/* ========== Fast Content-Length Formatting ========== */

// Write decimal digits of n into buf (no NUL). Returns number of chars.
static int uint_to_buf(char *buf, size_t n) {
    char tmp[20];
    int len = 0;
    if (n == 0) {
        buf[0] = '0';
        return 1;
    }
    while (n > 0) {
        tmp[len++] = '0' + (char) (n % 10);
        n /= 10;
    }
    for (int i = 0; i < len; i++)
        buf[i] = tmp[len - 1 - i];
    return len;
}

/* Pre-built status line + Content-Type + Content-Length label.
 * Only the CL *value*, \r\n, Connection, and \r\n\r\n are appended at runtime. */
#define HDR_200_TEXT                                                                               \
    "HTTP/1.1 200 OK\r\n"                                                                          \
    "Content-Type: text/plain; charset=utf-8\r\n"                                                  \
    "Content-Length: "
#define HDR_200_JSON                                                                               \
    "HTTP/1.1 200 OK\r\n"                                                                          \
    "Content-Type: application/json; charset=utf-8\r\n"                                            \
    "Content-Length: "
#define HDR_CONN_KA "\r\nConnection: keep-alive\r\n\r\n"

static const char hdr_200_text[] = HDR_200_TEXT;
static const char hdr_200_json[] = HDR_200_JSON;
static const char hdr_conn_ka[] = HDR_CONN_KA;

/* ========== Helper Functions ========== */

static XrValue make_string_val(XrayIsolate *X, const char *s, size_t len) {
    if (!s || len == 0)
        return xr_null();
    return xr_string_value(xr_string_intern(X, s, len, 0));
}

static XrValue make_cstring_val(XrayIsolate *X, const char *s) {
    if (!s)
        return xr_null();
    return make_string_val(X, s, strlen(s));
}

/*
 * Raw HTTP response buffer (malloc'd, caller must free).
 * Avoids GC allocations so response data survives IO yield safepoints.
 */
typedef struct {
    char *data;
    size_t len;
} HttpRawResponse;

/*
 * Build HTTP response header into caller-provided buf[512].
 * Fast path for 200+text/plain and 200+json uses prebuilt prefix + memcpy.
 * Returns header length written into buf.
 */
static int build_response_header(char *buf, int status, const char *content_type, size_t body_len) {
    const char *prefix = NULL;
    size_t prefix_len = 0;

    // Fast path: prebuilt prefix for the two most common combos
    if (status == 200) {
        if (!content_type || strncmp(content_type, "text/plain", 10) == 0) {
            prefix = hdr_200_text;
            prefix_len = sizeof(hdr_200_text) - 1;
        } else if (strncmp(content_type, "application/json", 16) == 0) {
            prefix = hdr_200_json;
            prefix_len = sizeof(hdr_200_json) - 1;
        }
    }

    if (prefix) {
        // memcpy prefix + fast CL digit + Connection trailer
        memcpy(buf, prefix, prefix_len);
        int pos = (int) prefix_len;
        pos += uint_to_buf(buf + pos, body_len);
        memcpy(buf + pos, hdr_conn_ka, sizeof(hdr_conn_ka) - 1);
        pos += (int) (sizeof(hdr_conn_ka) - 1);
        return pos;
    }

    // Slow path: snprintf for uncommon status codes / content types
    if (!content_type)
        content_type = "text/plain; charset=utf-8";
    const char *status_text = "OK";
    if (status == 404)
        status_text = "Not Found";
    else if (status == 500)
        status_text = "Internal Server Error";
    else if (status == 201)
        status_text = "Created";
    else if (status == 204)
        status_text = "No Content";

    return snprintf(buf, 512,
                    "HTTP/1.1 %d %s\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n",
                    status, status_text, content_type, body_len);
}

static HttpRawResponse format_response_raw(int status, const char *content_type, const char *body,
                                           size_t body_len) {
    HttpRawResponse resp = {NULL, 0};
    char header[512];
    int header_len = build_response_header(header, status, content_type, body_len);

    resp.len = header_len + body_len;
    resp.data = (char *) xr_malloc(resp.len);
    if (!resp.data) {
        resp.len = 0;
        return resp;
    }

    memcpy(resp.data, header, header_len);
    if (body_len > 0 && body)
        memcpy(resp.data + header_len, body, body_len);
    return resp;
}

// Arena-based format_response: allocates from per-request arena (no free needed)
static HttpRawResponse format_response_arena(XrArena *arena, int status, const char *content_type,
                                             const char *body, size_t body_len) {
    HttpRawResponse resp = {NULL, 0};
    char header[512];
    int header_len = build_response_header(header, status, content_type, body_len);

    resp.len = header_len + body_len;
    resp.data = (char *) xr_arena_alloc_raw(arena, resp.len);
    if (!resp.data) {
        resp.len = 0;
        return resp;
    }

    memcpy(resp.data, header, header_len);
    if (body_len > 0 && body)
        memcpy(resp.data + header_len, body, body_len);
    return resp;
}

// GC-based format_response for module API (http.response)
static XrValue format_response(XrayIsolate *X, int status, const char *content_type,
                               const char *body, size_t body_len) {
    HttpRawResponse raw = format_response_raw(status, content_type, body, body_len);
    if (!raw.data)
        return xr_null();
    XrString *str = xr_string_new(X, raw.data, raw.len);
    xr_free(raw.data);
    return xr_string_value(str);
}

/*
 * Process handler return value to raw HTTP response buffer (malloc'd).
 * C equivalent of http.xr _processResult().
 * Returns malloc'd buffer — caller must free.
 */
static HttpRawResponse process_handler_result_raw(XrayIsolate *X, XrValue result) {
    // null -> 204 No Content
    if (XR_IS_NULL(result)) {
        return format_response_raw(204, NULL, "", 0);
    }

    // string -> 200 text/plain
    if (XR_IS_STRING(result)) {
        XrString *str = XR_TO_STRING(result);
        return format_response_raw(200, NULL, XR_STRING_CHARS(str), str->length);
    }

    // Array -> [status, contentType, body]
    if (XR_IS_PTR(result) && XR_GC_GET_TYPE((XrGCHeader *) XR_TO_PTR(result)) == XR_TARRAY) {
        XrArray *arr = (XrArray *) XR_TO_PTR(result);
        int arr_len = arr->length;
        XrValue *data = (XrValue *) arr->data;
        int status = 200;
        const char *ct = NULL;
        const char *body = "";
        size_t body_len = 0;

        if (arr_len >= 1 && data) {
            XrValue v0 = data[0];
            if (XR_IS_INT(v0))
                status = (int) XR_TO_INT(v0);
        }
        if (arr_len >= 2 && data) {
            XrValue v1 = data[1];
            if (XR_IS_STRING(v1)) {
                XrString *s = XR_TO_STRING(v1);
                ct = XR_STRING_CHARS(s);
            }
        }
        if (arr_len >= 3 && data) {
            XrValue v2 = data[2];
            if (XR_IS_STRING(v2)) {
                XrString *s = XR_TO_STRING(v2);
                body = XR_STRING_CHARS(s);
                body_len = s->length;
            }
        }
        return format_response_raw(status, ct, body, body_len);
    }

    // int/float/bool -> convert to string
    if (XR_IS_INT(result)) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "%lld", (long long) XR_TO_INT(result));
        return format_response_raw(200, NULL, buf, len);
    }
    if (XR_IS_FLOAT(result)) {
        char buf[64];
        int len = snprintf(buf, sizeof(buf), "%g", XR_TO_FLOAT(result));
        return format_response_raw(200, NULL, buf, len);
    }
    if (XR_IS_BOOL(result)) {
        const char *s = XR_TO_BOOL(result) ? "true" : "false";
        return format_response_raw(200, NULL, s, strlen(s));
    }

    // other -> JSON stringify
    size_t json_len = 0;
    char *json_str = xr_json_stringify_to_cstr(X, result, &json_len);
    if (json_str && json_len > 0) {
        HttpRawResponse resp = format_response_raw(200, "application/json", json_str, json_len);
        xr_free(json_str);
        return resp;
    }
    if (json_str)
        xr_free(json_str);
    return format_response_raw(200, NULL, "", 0);
}

/* ======================================================================
 * Stackless Connection Handler — continuation-based async state machine
 *
 * Each connection runs as a cfunc coroutine. State persists in a
 * heap-allocated HttpConnCtx. Each yield point is a continuation
 * boundary — no private C stack needed.
 * ====================================================================== */

// Forward declarations
static XrCFuncResult http_conn_loop(XrayIsolate *X, int status, void *ctx, XrValue *result);
static XrCFuncResult http_conn_write_cont(XrayIsolate *X, int status, void *ctx, XrValue *result);
static XrCFuncResult http_conn_handler_done(XrayIsolate *X, int status, void *ctx, XrValue *result);
static XrCFuncResult http_conn_ws_handler_done(XrayIsolate *X, int status, void *ctx,
                                               XrValue *result);
static XrCFuncResult http_conn_read_body(XrayIsolate *X, int status, void *ctx, XrValue *result);
static XrCFuncResult http_conn_read_chunked(XrayIsolate *X, int status, void *ctx, XrValue *result);
static XrCFuncResult http_conn_read_large_body(XrayIsolate *X, int status, void *ctx,
                                               XrValue *result);
static XrCFuncResult http_listen_cont(XrayIsolate *X, int status, void *ctx, XrValue *result);

/*
 * Connection context — all state that survives across yield points.
 * Replaces stack-local variables from the old stackful handler.
 */
typedef struct HttpConnCtx {
    XrayIsolate *X;
    int fd;
    XrRouter *router;
    XrHttpContext *http_ctx;
    XrRuntime *runtime;

    // Read buffer
    char *read_buf;
    int buf_used;
    int scan_from;

    // Connection state
    int request_count;
    int batch_count;
    int max_requests;
    int read_timeout_ms;

    // Per-request arena
    XrArena req_arena;
    bool arena_inited;

    // Current request parse state
    int header_end;
    int parsed;
    int content_length;

    // Write state (for async write_all)
    const char *write_ptr;
    size_t write_remaining;
    XrContinuation write_done_cont;

    // Dynamic route state
    XrJson *req_json;
    XrClosure *handler;

    // Response data (malloc'd, needs free after write)
    char *response_data;

    // Chunked decoder
    XrNetBuffer *chunk_buf;
    XrChunkedDecoder decoder;

    // Large body read
    char *body_buf;
    int body_read;
    int body_total;
} HttpConnCtx;

// Free context and close connection
static XrCFuncResult http_conn_cleanup(HttpConnCtx *ctx) {
    int fd = ctx->fd;
    if (ctx->arena_inited)
        xr_arena_destroy(&ctx->req_arena);
    xr_free(ctx->read_buf);
    xr_free(ctx->response_data);
    if (ctx->chunk_buf)
        xr_netbuf_release(ctx->chunk_buf);
    {
        XrRuntime *rt = ctx->runtime;
        if (rt) {
            /* Use fdmap_get instead of netpoll_open to avoid creating
             * a throwaway PollDesc for fds that were never IO-yielded.
             * Creating + immediately closing a PollDesc leaves a race
             * window where another worker can poll a stale kqueue event
             * referencing the already-freed PollDesc (use-after-free). */
            XrPollDesc *pd = xr_fdmap_get(&rt->netpoll, fd);
            if (pd && !atomic_load(&pd->closing)) {
                xr_netpoll_close(&rt->netpoll, pd);
            }
        }
    }
    xr_closesocket(fd);
    if (ctx->http_ctx)
        atomic_fetch_sub(&ctx->http_ctx->current_conns, 1);
    xr_free(ctx);
    return XR_CFUNC_DONE;
}

/*
 * Generic async write helper. Writes ctx->write_ptr / write_remaining,
 * yields for EAGAIN, then calls ctx->write_done_cont when complete.
 */
static XrCFuncResult http_conn_write_cont(XrayIsolate *X, int status, void *user_ctx,
                                          XrValue *result) {
    HttpConnCtx *ctx = (HttpConnCtx *) user_ctx;
    if (status != XR_RESUME_IO_READY)
        return http_conn_cleanup(ctx);

    while (ctx->write_remaining > 0) {
        ssize_t n = write(ctx->fd, ctx->write_ptr, ctx->write_remaining);
        if (n > 0) {
            ctx->write_ptr += n;
            ctx->write_remaining -= n;
            continue;
        }
        if (n == 0)
            return http_conn_cleanup(ctx);
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return xr_yield_for_io(X, ctx->fd, XR_WAIT_WRITE, -1, http_conn_write_cont, ctx,
                                   result);
        }
        return http_conn_cleanup(ctx);
    }
    // Write complete — call the stored return continuation
    return ctx->write_done_cont(X, XR_RESUME_IO_READY, ctx, result);
}

// Start an async write, calling done_cont when finished
static XrCFuncResult http_conn_start_write(XrayIsolate *X, HttpConnCtx *ctx, const char *data,
                                           size_t len, XrContinuation done_cont, XrValue *result) {
    ctx->write_ptr = data;
    ctx->write_remaining = len;
    ctx->write_done_cont = done_cont;
    return http_conn_write_cont(X, XR_RESUME_IO_READY, ctx, result);
}

/*
 * Handle dynamic route — stackless version.
 * Parses request, reads body if needed, then calls handler closure.
 * May yield for body read or closure execution.
 */
static XrCFuncResult handle_dynamic_route(XrayIsolate *X, HttpConnCtx *ctx, XrValue *result) {
    const char *method_str = NULL, *path_str = NULL;
    size_t method_len = 0, path_len = 0;
    int minor_ver = 1;
    XrHttpHeader headers[32];
    size_t num_headers = 32;

    int parsed =
        xr_http_parse_request_ex(ctx->read_buf, ctx->buf_used, &method_str, &method_len, &path_str,
                                 &path_len, &minor_ver, headers, &num_headers, 0);

    if (parsed <= 0) {
        return http_conn_start_write(X, ctx, RESP_400, sizeof(RESP_400) - 1,
                                     (XrContinuation) http_conn_cleanup, result);
    }
    ctx->parsed = parsed;

    size_t pure_path_len = path_len;
    for (size_t i = 0; i < path_len; i++) {
        if (path_str[i] == '?') {
            pure_path_len = i;
            break;
        }
    }

    XrHttpMethod method = xr_http_method_from_string(method_str, method_len);
    XrRouteParams params;
    params.count = 0;
    void *user_data = NULL;
    const char *static_resp = NULL, *prebuilt_resp = NULL;
    size_t static_len = 0, prebuilt_len = 0;

    XrRouteHandler found_handler =
        xr_router_find(ctx->router, method, path_str, pure_path_len, &params, &user_data,
                       &static_resp, &static_len, &prebuilt_resp, &prebuilt_len);

    if (prebuilt_resp && prebuilt_len > 0) {
        return http_conn_start_write(X, ctx, prebuilt_resp, prebuilt_len, http_conn_loop, result);
    }

    if (static_resp && static_len > 0) {
        HttpRawResponse r =
            format_response_arena(&ctx->req_arena, 200, NULL, static_resp, static_len);
        return http_conn_start_write(X, ctx, r.data, r.len, http_conn_loop, result);
    }

    // WebSocket upgrade route
    if (found_handler == XR_ROUTE_HANDLER_WEBSOCKET && user_data) {
        char *hdr_copy = xr_arena_strndup(&ctx->req_arena, ctx->read_buf, ctx->buf_used);
        if (!hdr_copy) {
            return http_conn_start_write(X, ctx, RESP_500, sizeof(RESP_500) - 1,
                                         (XrContinuation) http_conn_cleanup, result);
        }
        XrValue ws_conn = xr_ws_upgrade_and_wrap(X, ctx->fd, hdr_copy);
        if (XR_IS_NULL(ws_conn)) {
            return http_conn_start_write(X, ctx, RESP_400, sizeof(RESP_400) - 1,
                                         (XrContinuation) http_conn_cleanup, result);
        }
        XrClosure *ws_handler = (XrClosure *) user_data;
        return xr_yield_call_closure(X, ws_handler, &ws_conn, 1, http_conn_ws_handler_done, ctx,
                                     result);
    }

    if (user_data) {
        ctx->handler = (XrClosure *) user_data;
        XrCoroutine *coro = xr_current_coro(X);

        XrJson *req = xr_json_new(coro, 8);
        if (!req) {
            return http_conn_start_write(X, ctx, RESP_500, sizeof(RESP_500) - 1,
                                         (XrContinuation) http_conn_cleanup, result);
        }
        ctx->req_json = req;

        char mbuf[16] = {0};
        if (method_len < sizeof(mbuf))
            memcpy(mbuf, method_str, method_len);
        xr_json_set_by_key(X, req, "method", make_cstring_val(X, mbuf));

        char *pbuf = xr_arena_strndup(&ctx->req_arena, path_str, pure_path_len);
        if (pbuf) {
            xr_json_set_by_key(X, req, "path", make_cstring_val(X, pbuf));
        }

        XrJson *hdrs = xr_json_new(coro, 16);
        ctx->content_length = -1;
        bool is_chunked = false;
        for (size_t i = 0; i < num_headers; i++) {
            char key_buf[128];
            size_t kl = headers[i].name_len;
            if (kl >= sizeof(key_buf))
                kl = sizeof(key_buf) - 1;
            memcpy(key_buf, headers[i].name, kl);
            key_buf[kl] = '\0';
            XrValue val = make_string_val(X, headers[i].value, headers[i].value_len);
            xr_json_set_by_key(X, hdrs, key_buf, val);
            if (kl == 14 && strncasecmp(key_buf, "Content-Length", 14) == 0) {
                ctx->content_length = atoi(headers[i].value);
            } else if (kl == 17 && strncasecmp(key_buf, "Transfer-Encoding", 17) == 0) {
                if (headers[i].value_len >= 7 && strncasecmp(headers[i].value, "chunked", 7) == 0) {
                    is_chunked = true;
                }
            }
        }
        xr_json_set_by_key(X, req, "headers", xr_json_value(hdrs));

        // Read body if needed, then call handler
        if (is_chunked && parsed > 0) {
            ctx->chunk_buf = xr_netbuf_acquire(8192);
            if (!ctx->chunk_buf) {
                return http_conn_start_write(X, ctx, RESP_500, sizeof(RESP_500) - 1,
                                             (XrContinuation) http_conn_cleanup, result);
            }
            int body_in_buf = ctx->buf_used - parsed;
            if (body_in_buf > 0) {
                char *wp = xr_netbuf_reserve(ctx->chunk_buf, body_in_buf);
                if (wp) {
                    memcpy(wp, ctx->read_buf + parsed, body_in_buf);
                    xr_netbuf_advance(ctx->chunk_buf, body_in_buf);
                }
            }
            memset(&ctx->decoder, 0, sizeof(ctx->decoder));
            ctx->decoder.consume_trailer = true;
            return http_conn_read_chunked(X, XR_RESUME_IO_READY, ctx, result);
        } else if (ctx->content_length > 0 && ctx->content_length <= MAX_BODY_SIZE && parsed > 0) {
            int total_needed = parsed + ctx->content_length;
            if (total_needed <= CONN_READ_BUF_SIZE) {
                if (ctx->buf_used < total_needed) {
                    ctx->body_total = total_needed;
                    return http_conn_read_body(X, XR_RESUME_IO_READY, ctx, result);
                }
                if (ctx->buf_used >= total_needed) {
                    xr_json_set_by_key(
                        X, req, "body",
                        make_string_val(X, ctx->read_buf + parsed, ctx->content_length));
                }
            } else {
                ctx->body_buf = (char *) xr_arena_alloc_raw(&ctx->req_arena, ctx->content_length);
                if (ctx->body_buf) {
                    int body_in_buf = ctx->buf_used - parsed;
                    if (body_in_buf > ctx->content_length)
                        body_in_buf = ctx->content_length;
                    if (body_in_buf > 0)
                        memcpy(ctx->body_buf, ctx->read_buf + parsed, body_in_buf);
                    ctx->body_read = body_in_buf;
                    ctx->body_total = ctx->content_length;
                    if (ctx->body_read < ctx->body_total) {
                        return http_conn_read_large_body(X, XR_RESUME_IO_READY, ctx, result);
                    }
                    xr_json_set_by_key(X, req, "body",
                                       make_string_val(X, ctx->body_buf, ctx->content_length));
                }
            }
        } else if (ctx->content_length > MAX_BODY_SIZE) {
            xr_json_set_by_key(X, req, "streaming", xr_bool(true));
            xr_json_set_by_key(X, req, "contentLength", XR_FROM_INT((int64_t) ctx->content_length));
            xr_json_set_by_key(X, req, "_fd", xr_int(ctx->fd));
            int body_in_buf = ctx->buf_used - parsed;
            if (body_in_buf > 0) {
                xr_json_set_by_key(X, req, "_bodyPrefix",
                                   make_string_val(X, ctx->read_buf + parsed, body_in_buf));
            }
        }

        if (params.count > 0) {
            XrJson *params_obj = xr_json_new(coro, 4);
            for (int i = 0; i < params.count && i < XR_ROUTER_MAX_PARAMS; i++) {
                if (params.params[i].key && params.params[i].value) {
                    char kb[64];
                    size_t kbl = params.params[i].key_len;
                    if (kbl >= sizeof(kb))
                        kbl = sizeof(kb) - 1;
                    memcpy(kb, params.params[i].key, kbl);
                    kb[kbl] = '\0';
                    xr_json_set_by_key(
                        X, params_obj, kb,
                        make_string_val(X, params.params[i].value, params.params[i].value_len));
                }
            }
            xr_json_set_by_key(X, req, "params", xr_json_value(params_obj));
        }

        // Call handler closure — resumes http_conn_handler_done on return
        XrValue req_val = xr_json_value(req);
        return xr_yield_call_closure(X, ctx->handler, &req_val, 1, http_conn_handler_done, ctx,
                                     result);
    }

    // No route matched
    return http_conn_start_write(X, ctx, RESP_404, sizeof(RESP_404) - 1, http_conn_loop, result);
}

/*
 * Continuation: read content-length body into read_buf.
 */
static XrCFuncResult http_conn_read_body(XrayIsolate *X, int status, void *user_ctx,
                                         XrValue *result) {
    HttpConnCtx *ctx = (HttpConnCtx *) user_ctx;
    if (status != XR_RESUME_IO_READY)
        return http_conn_cleanup(ctx);

    while (ctx->buf_used < ctx->body_total) {
        if (ctx->buf_used >= CONN_READ_BUF_SIZE)
            break;
        ssize_t n =
            read(ctx->fd, ctx->read_buf + ctx->buf_used, CONN_READ_BUF_SIZE - ctx->buf_used);
        if (n > 0) {
            ctx->buf_used += (int) n;
            continue;
        }
        if (n == 0)
            return http_conn_cleanup(ctx);
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return xr_yield_for_io(X, ctx->fd, XR_WAIT_READ, -1, http_conn_read_body, ctx, result);
        }
        return http_conn_cleanup(ctx);
    }
    if (ctx->buf_used >= ctx->body_total) {
        xr_json_set_by_key(X, ctx->req_json, "body",
                           make_string_val(X, ctx->read_buf + ctx->parsed, ctx->content_length));
    }
    // Body read complete — call handler
    XrValue req_val = xr_json_value(ctx->req_json);
    return xr_yield_call_closure(X, ctx->handler, &req_val, 1, http_conn_handler_done, ctx, result);
}

/*
 * Continuation: read chunked transfer-encoded body.
 */
static XrCFuncResult http_conn_read_chunked(XrayIsolate *X, int status, void *user_ctx,
                                            XrValue *result) {
    HttpConnCtx *ctx = (HttpConnCtx *) user_ctx;
    if (status != XR_RESUME_IO_READY)
        goto chunk_error;

    for (;;) {
        size_t decode_sz = ctx->chunk_buf->size;
        if (decode_sz > 0) {
            ssize_t rc = xr_http_decode_chunked(&ctx->decoder, ctx->chunk_buf->bytes, &decode_sz);
            ctx->chunk_buf->size = decode_sz;
            if (rc >= 0)
                goto chunk_done;
            if (rc == -1)
                goto chunk_error;
        }
        if (ctx->chunk_buf->size > (size_t) MAX_BODY_SIZE)
            goto chunk_error;

        char *wp = xr_netbuf_reserve(ctx->chunk_buf, 4096);
        if (!wp)
            goto chunk_error;
        ssize_t n = read(ctx->fd, wp, 4096);
        if (n > 0) {
            xr_netbuf_advance(ctx->chunk_buf, n);
            continue;
        }
        if (n == 0)
            goto chunk_error;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return xr_yield_for_io(X, ctx->fd, XR_WAIT_READ, -1, http_conn_read_chunked, ctx,
                                   result);
        }
        goto chunk_error;
    }

chunk_done:
    if (ctx->chunk_buf->size > 0) {
        xr_json_set_by_key(X, ctx->req_json, "body",
                           make_string_val(X, ctx->chunk_buf->bytes, ctx->chunk_buf->size));
    }
    xr_netbuf_release(ctx->chunk_buf);
    ctx->chunk_buf = NULL;
    {
        XrValue req_val = xr_json_value(ctx->req_json);
        return xr_yield_call_closure(X, ctx->handler, &req_val, 1, http_conn_handler_done, ctx,
                                     result);
    }

chunk_error:
    if (ctx->chunk_buf) {
        xr_netbuf_release(ctx->chunk_buf);
        ctx->chunk_buf = NULL;
    }
    return http_conn_start_write(X, ctx, RESP_400, sizeof(RESP_400) - 1,
                                 (XrContinuation) http_conn_cleanup, result);
}

/*
 * Continuation: read large body (> read_buf size) into arena buffer.
 */
static XrCFuncResult http_conn_read_large_body(XrayIsolate *X, int status, void *user_ctx,
                                               XrValue *result) {
    HttpConnCtx *ctx = (HttpConnCtx *) user_ctx;
    if (status != XR_RESUME_IO_READY)
        return http_conn_cleanup(ctx);

    while (ctx->body_read < ctx->body_total) {
        ssize_t n = read(ctx->fd, ctx->body_buf + ctx->body_read, ctx->body_total - ctx->body_read);
        if (n > 0) {
            ctx->body_read += (int) n;
            continue;
        }
        if (n == 0)
            break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return xr_yield_for_io(X, ctx->fd, XR_WAIT_READ, -1, http_conn_read_large_body, ctx,
                                   result);
        }
        break;
    }
    if (ctx->body_read >= ctx->body_total) {
        xr_json_set_by_key(X, ctx->req_json, "body",
                           make_string_val(X, ctx->body_buf, ctx->body_total));
    }
    XrValue req_val = xr_json_value(ctx->req_json);
    return xr_yield_call_closure(X, ctx->handler, &req_val, 1, http_conn_handler_done, ctx, result);
}

/*
 * Continuation: called when user handler closure returns.
 * Process result and write HTTP response.
 */
static XrCFuncResult http_conn_handler_done(XrayIsolate *X, int status, void *user_ctx,
                                            XrValue *result) {
    (void) status;
    HttpConnCtx *ctx = (HttpConnCtx *) user_ctx;
    XrValue closure_result = xr_get_closure_result(X);

    xr_free(ctx->response_data);
    HttpRawResponse r = process_handler_result_raw(X, closure_result);
    ctx->response_data = r.data;
    if (!r.data)
        return http_conn_cleanup(ctx);

    // After writing response, continue to next request in keep-alive loop
    return http_conn_start_write(X, ctx, r.data, r.len, http_conn_loop, result);
}

/*
 * Continuation: called when WS handler closure returns.
 * Connection lifecycle is over — cleanup.
 */
static XrCFuncResult http_conn_ws_handler_done(XrayIsolate *X, int status, void *user_ctx,
                                               XrValue *result) {
    (void) X;
    (void) status;
    (void) result;
    HttpConnCtx *ctx = (HttpConnCtx *) user_ctx;
    return http_conn_cleanup(ctx);
}

/*
 * Find end of HTTP request header (\r\n\r\n) in buffer.
 * scan_from: start scanning from this offset (avoids re-scanning old data).
 * Returns offset past the \r\n\r\n, or 0 if not found.
 */
static int find_request_end(const char *buf, int len, int scan_from) {
    int start = (scan_from > 3) ? scan_from - 3 : 0;
    for (int i = start; i <= len - 4; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return i + 4;
        }
    }
    return 0;
}

/*
 * Connection entry — cfunc coroutine entry point.
 * args[0] = client fd (int), args[1] = XrHttpContext* (ptr).
 */
static XrCFuncResult http_conn_init(XrayIsolate *X, XrValue *args, int argc, XrValue *result) {
    (void) result;
    if (argc < 2 || !XR_IS_INT(args[0]))
        return XR_CFUNC_DONE;

    int fd = (int) XR_TO_INT(args[0]);
    XrHttpContext *hctx = (XrHttpContext *) XR_TO_PTR(args[1]);

    HttpConnCtx *ctx = (HttpConnCtx *) xr_calloc(1, sizeof(HttpConnCtx));
    if (!ctx) {
        xr_closesocket(fd);
        return XR_CFUNC_DONE;
    }

    ctx->X = X;
    ctx->fd = fd;
    ctx->http_ctx = hctx;
    ctx->router = hctx->server ? hctx->server->router : NULL;
    ctx->runtime = (XrRuntime *) X->vm.runtime;
    ctx->max_requests = atomic_load(&hctx->max_requests_per_conn);
    ctx->read_timeout_ms = atomic_load(&hctx->read_timeout_ms);

    ctx->read_buf = (char *) xr_malloc(CONN_READ_BUF_SIZE);
    if (!ctx->read_buf) {
        xr_closesocket(fd);
        if (hctx)
            atomic_fetch_sub(&hctx->current_conns, 1);
        xr_free(ctx);
        return XR_CFUNC_DONE;
    }

    xr_arena_init(&ctx->req_arena, 4096);
    ctx->arena_inited = true;

    // Start the main request loop
    return http_conn_loop(X, XR_RESUME_IO_READY, ctx, result);
}

/*
 * Main request loop continuation.
 * Reads headers, dispatches to prebuilt or dynamic route.
 * Called after: init, write complete (keep-alive), batch yield.
 */
static XrCFuncResult http_conn_loop(XrayIsolate *X, int status, void *user_ctx, XrValue *result) {
    HttpConnCtx *ctx = (HttpConnCtx *) user_ctx;
    if (status != XR_RESUME_IO_READY)
        return http_conn_cleanup(ctx);

    // Reset arena for next request
    if (ctx->arena_inited && ctx->request_count > 0) {
        xr_arena_reset(&ctx->req_arena);
    }
    xr_free(ctx->response_data);
    ctx->response_data = NULL;

    // Shift leftover data from previous request
    if (ctx->header_end > 0 && ctx->buf_used > ctx->header_end) {
        int remaining = ctx->buf_used - ctx->header_end;
        memmove(ctx->read_buf, ctx->read_buf + ctx->header_end, remaining);
        ctx->buf_used = remaining;
    } else if (ctx->header_end > 0) {
        ctx->buf_used = 0;
    }
    ctx->header_end = 0;
    ctx->scan_from = 0;

    if (ctx->max_requests > 0 && ctx->request_count >= ctx->max_requests)
        return http_conn_cleanup(ctx);

    // Voluntary yield after batch to prevent worker starvation
    if (ctx->batch_count >= CONN_YIELD_BATCH) {
        ctx->batch_count = 0;
        return xr_yield(X, http_conn_loop, ctx);
    }

    // Try to find complete header in existing buffer
    int header_end = find_request_end(ctx->read_buf, ctx->buf_used, ctx->scan_from);
    if (header_end > 0)
        goto dispatch;

    // Need more data — read from socket
    for (;;) {
        if (ctx->buf_used >= CONN_READ_BUF_SIZE) {
            return http_conn_start_write(X, ctx, RESP_413, sizeof(RESP_413) - 1,
                                         (XrContinuation) http_conn_cleanup, result);
        }
        int old_used = ctx->buf_used;
        ssize_t n =
            read(ctx->fd, ctx->read_buf + ctx->buf_used, CONN_READ_BUF_SIZE - ctx->buf_used);
        if (n > 0) {
            ctx->buf_used += (int) n;
            header_end = find_request_end(ctx->read_buf, ctx->buf_used, old_used);
            if (header_end > 0)
                goto dispatch;
            continue;
        }
        if (n == 0)
            return http_conn_cleanup(ctx);
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return http_conn_cleanup(ctx);

        ctx->batch_count = 0;
        int64_t timeout = (ctx->request_count == 0) ? ctx->read_timeout_ms : -1;
        return xr_yield_for_io(X, ctx->fd, XR_WAIT_READ, timeout, http_conn_loop, ctx, result);
    }

dispatch:
    ctx->header_end = header_end;

    // Try prebuilt route (zero-alloc fast path)
    {
        const char *resp = NULL;
        size_t resp_len = 0;
        if (xr_http_try_prebuilt(ctx->router, ctx->read_buf, ctx->buf_used, &resp, &resp_len)) {
            ctx->request_count++;
            ctx->batch_count++;
            if (ctx->http_ctx)
                atomic_fetch_add(&ctx->http_ctx->total_requests, 1);
            return http_conn_start_write(X, ctx, resp, resp_len, http_conn_loop, result);
        }
    }

    // Dynamic route
    return handle_dynamic_route(X, ctx, result);
}

/* ======================================================================
 * Stackless Accept Loop
 * ====================================================================== */

typedef struct {
    XrayIsolate *X;
    int listen_fd;
    XrHttpContext *ctx;
} HttpListenCtx;

/*
 * HTTP listen entry — cfunc coroutine entry point.
 * args[0] = listen fd (int), args[1] = XrHttpContext* (ptr).
 */
static XrCFuncResult http_listen_init(XrayIsolate *X, XrValue *args, int argc, XrValue *result) {
    (void) result;
    if (argc < 2 || !XR_IS_INT(args[0]))
        return XR_CFUNC_DONE;

    HttpListenCtx *ctx = (HttpListenCtx *) xr_calloc(1, sizeof(HttpListenCtx));
    if (!ctx)
        return XR_CFUNC_DONE;

    ctx->X = X;
    ctx->listen_fd = (int) XR_TO_INT(args[0]);
    ctx->ctx = (XrHttpContext *) XR_TO_PTR(args[1]);

    return xr_yield_for_io(X, ctx->listen_fd, XR_WAIT_READ, -1, http_listen_cont, ctx, result);
}

/*
 * Accept loop continuation — accepts connections,
 * spawns cfunc conn coroutine per client, then yields for next batch.
 */
static XrCFuncResult http_listen_cont(XrayIsolate *X, int status, void *user_ctx, XrValue *result) {
    HttpListenCtx *lctx = (HttpListenCtx *) user_ctx;
    XrHttpContext *ctx = lctx->ctx;

    if (!ctx || !ctx->server || !ctx->server->running || status != XR_RESUME_IO_READY) {
        xr_free(lctx);
        return XR_CFUNC_DONE;
    }

    for (;;) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        int client_fd = accept(lctx->listen_fd, (struct sockaddr *) &addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            break;
        }

        xr_socket_set_nonblock(client_fd);
        xr_socket_set_nodelay(client_fd, true);
#ifdef TCP_NOTSENT_LOWAT
        {
            int lowat = 16384;
            setsockopt(client_fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, (const char *) &lowat,
                       sizeof(lowat));
        }
#endif

        // Check connection limit
        int max = atomic_load(&ctx->max_conns);
        if (max > 0) {
            int cur = atomic_fetch_add(&ctx->current_conns, 1);
            if (cur >= max) {
                atomic_fetch_sub(&ctx->current_conns, 1);
                ssize_t ret = xr_socket_send(client_fd, RESP_503, sizeof(RESP_503) - 1);
                (void) ret;
                shutdown(client_fd, XR_SHUT_WR);
                xr_closesocket(client_fd);
                continue;
            }
        } else {
            atomic_fetch_add(&ctx->current_conns, 1);
        }

        atomic_fetch_add(&ctx->total_conns_accepted, 1);

        // Spawn stackless connection coroutine
        XrValue conn_args[2] = {xr_int(client_fd), XR_FROM_PTR(ctx)};
        XrCoroutine *coro = xr_coro_create_cfunc(X, http_conn_init, conn_args, 2, "http.conn");
        if (coro) {
            xr_coro_spawn(X, coro);
        } else {
            atomic_fetch_sub(&ctx->current_conns, 1);
            xr_closesocket(client_fd);
        }
    }

    return xr_yield_for_io(X, lctx->listen_fd, XR_WAIT_READ, -1, http_listen_cont, lctx, result);
}

/* Continuation: keep caller coroutine blocked while server is running.
 * Polls every second — acceptable for a server lifetime check. */
static XrCFuncResult http_listen_wait_cont(XrayIsolate *X, int status, void *ctx, XrValue *result) {
    (void) status;
    XrHttpContext *hctx = (XrHttpContext *) ctx;
    if (!hctx || !hctx->server || !hctx->server->running) {
        *result = xr_bool(true);
        return XR_CFUNC_DONE;
    }

    return xr_yield_for_timeout(X, 1000, http_listen_wait_cont, ctx, result);
}

// http.listen(port) -> bool (yieldable)
XrCFuncResult xr_http_listen_impl(XrayIsolate *X, XrValue *args, int nargs, XrValue *result) {
    if (nargs < 1 || !XR_IS_INT(args[0])) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    int port = (int) XR_TO_INT(args[0]);

    XrHttpContext *ctx = xr_http_get_context(X);
    if (!ctx) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    // Auto-create server instance
    if (!ctx->server) {
        ctx->server = xr_http_server_new(X);
        if (!ctx->server) {
            fprintf(stderr, "http.listen: failed to create server\n");
            *result = xr_bool(false);
            return XR_CFUNC_DONE;
        }
    }

    // Create listen socket
    int listen_fd = xr_socket_listen("0.0.0.0", port, XR_HTTP_BACKLOG);
    if (listen_fd < 0) {
        fprintf(stderr, "http.listen: cannot create listen socket on port %d\n", port);
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    ctx->server->listen_fd = listen_fd;
    ctx->server->port = (uint16_t) port;
    ctx->server->running = true;

    printf("=== xray HTTP Server ===\n");
    printf("Port: %d\n", port);
    printf("Listening...\n");

    // Spawn stackless accept loop coroutine
    XrValue listen_args[2] = {xr_int(listen_fd), XR_FROM_PTR(ctx)};
    XrCoroutine *listen_coro =
        xr_coro_create_cfunc(X, http_listen_init, listen_args, 2, "http.listen");
    if (!listen_coro) {
        xr_closesocket(listen_fd);
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }
    xr_coro_spawn(X, listen_coro);

    // Block caller until server stops (keeps script alive)
    return xr_yield_for_timeout(X, 1000, http_listen_wait_cont, ctx, result);
}

/* ======================================================================
 * Config and Response helpers (exported as module functions)
 * ====================================================================== */

// http.config(opts) -> void
XrValue xr_http_config_impl(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();

    XrHttpContext *ctx = xr_http_get_context(X);
    if (!ctx)
        return xr_null();

    // opts should be a Json object
    if (!xr_value_is_json(args[0]))
        return xr_null();
    XrJson *opts = xr_value_to_json(args[0]);

    XrValue v;
    v = xr_json_get_by_key(X, opts, "maxConns");
    if (XR_IS_INT(v))
        atomic_store(&ctx->max_conns, (int) XR_TO_INT(v));

    v = xr_json_get_by_key(X, opts, "maxRequestsPerConn");
    if (XR_IS_INT(v))
        atomic_store(&ctx->max_requests_per_conn, (int) XR_TO_INT(v));

    v = xr_json_get_by_key(X, opts, "idleTimeout");
    if (XR_IS_INT(v))
        atomic_store(&ctx->idle_timeout_ms, (int) XR_TO_INT(v));

    v = xr_json_get_by_key(X, opts, "readTimeout");
    if (XR_IS_INT(v))
        atomic_store(&ctx->read_timeout_ms, (int) XR_TO_INT(v));

    return xr_null();
}

// http.serverStats() -> Json { currentConns, totalRequests, totalConns }
XrValue xr_http_server_stats(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrHttpContext *ctx = xr_http_get_context(X);
    if (!ctx)
        return xr_null();

    XrJson *j = xr_json_new(xr_current_coro(X), 4);
    xr_json_set_by_key(X, j, "currentConns", XR_FROM_INT(atomic_load(&ctx->current_conns)));
    xr_json_set_by_key(X, j, "totalRequests",
                       XR_FROM_INT((int64_t) atomic_load(&ctx->total_requests)));
    xr_json_set_by_key(X, j, "totalConns",
                       XR_FROM_INT((int64_t) atomic_load(&ctx->total_conns_accepted)));
    return xr_json_value(j);
}

// http.response(status, body?, headers?) -> string
XrValue xr_http_response_impl(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();

    int status = XR_IS_INT(args[0]) ? (int) XR_TO_INT(args[0]) : 200;
    const char *body = "";
    size_t body_len = 0;
    const char *content_type = NULL;

    if (argc >= 2 && !XR_IS_NULL(args[1])) {
        if (XR_IS_STRING(args[1])) {
            XrString *s = XR_TO_STRING(args[1]);
            body = XR_STRING_CHARS(s);
            body_len = s->length;
        } else {
            // JSON stringify
            char *json_str = xr_json_stringify_to_cstr(X, args[1], &body_len);
            if (json_str) {
                content_type = "application/json; charset=utf-8";
                XrValue resp = format_response(X, status, content_type, json_str, body_len);
                xr_free(json_str);
                return resp;
            }
        }
    }

    return format_response(X, status, content_type, body, body_len);
}
