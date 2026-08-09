/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * ws_binding.c - WebSocket native runtime helpers
 *
 * KEY CONCEPT:
 *   Connection I/O, HTTP integration, and permessage-deflate remain native
 *   runtime boundaries for now. stdlib/ws/ws.xr extends the module with pure
 *   protocol helpers (accept-key, frame assembly, masking) that are safe to
 *   compile in AOT.
 */

#include "ws_internal.h"
#include "../../stdlib/common.h"
#include "../../stdlib/stdlib_cache.h"
#include "../../src/base/xmalloc.h"
#include "../../src/module/xmodule.h"
#include "../../src/vm/xvm.h"
#include "../../src/vm/xvm_closure.h"
#include "../../src/vm/xvm_coro_api.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/closure/xclosure.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/coro/xnetpoll.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/coro/xworker.h"  // For XrRuntime
#include "../../src/coro/xsocket.h"  // For xr_socket_listen, xr_socket_set_nonblock
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/symbol/xsymbol_table.h"
#include "../../src/os/os_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

/* ========== External Declarations ========== */

extern XrValue xr_string_value(XrString *str);
extern XrString *xr_string_intern(XrVMRuntime *X, const char *str, size_t len, uint32_t hash);
struct XrCoroutine;
extern struct XrCoroutine *xr_current_coro(XrVMRuntime *X);
extern XrModule *xr_module_create_native(XrVMRuntime *isolate, const char *name);

/* ========== Helper Functions ========== */

// Non-interned string for WS message data (avoids rwlock + hash table lookup)
static XrValue ws_make_string(XrVMRuntime *X, const char *str, size_t len) {
    if (!str)
        return xr_null();
    if (len == 0)
        return xrs_string_value_c(X, "");
    XrString *s = xr_string_new(X, str, len);
    if (!s)
        return xr_null();
    return xr_string_value(s);
}

static XrObjectInstance *ws_shape_new(XrVMRuntime *X, const char *name) {
    XrClass *cls = xr_stdlib_record_class_get(X, "ws", name);
    return cls ? xr_object_instance_new_with_class(xr_current_coro(X), cls) : NULL;
}

static XrObjectInstance *ws_conn_new(XrVMRuntime *X) {
    return ws_shape_new(X, "WsConn");
}

static XrObjectInstance *ws_message_new(XrVMRuntime *X) {
    return ws_shape_new(X, "WsMessage");
}

// Get raw data from XrString or Array<uint8>
static const char *get_data_arg(XrValue v, size_t *out_len) {
    if (XR_IS_STRING(v)) {
        XrString *s = XR_TO_STRING(v);
        if (out_len)
            *out_len = s->length;
        return s->data;
    }
    if (XR_IS_ARRAY(v)) {
        XrArray *arr = XR_TO_ARRAY(v);
        if (arr->elem_type == XR_ELEM_U8) {
            if (out_len)
                *out_len = (size_t) arr->length;
            return (const char *) arr->data;
        }
    }
    return NULL;
}

/* ========== WebSocket Storage via Per-Isolate Context ========== */

/*
 * Per-isolate WebSocket context. Each isolate owns its connection registry and
 * server listen state so module teardown can clean native resources without
 * relying on global process state.
 */

// Include hashmap header for proper type declarations
#include "../../src/base/xhashmap.h"

#define WS_CONN_INIT_CAP 64

typedef struct XrWsContext {
    // Connection registry (protected by conn_mutex for multi-worker safety)
    XrWebSocket **conn_array;  // conn_array[id] = ws, id starts from 1
    int array_capacity;        // current capacity of conn_array
    _Atomic int next_id;       // Connection ID counter (atomic for multi-worker)
    xr_mutex_t conn_mutex;     // Protects conn_array grow/store/remove

    // Free-list for ID recycling (protected by conn_mutex)
    int *free_ids;      // Stack of recycled IDs
    int free_count;     // Number of recycled IDs
    int free_capacity;  // Capacity of free_ids array

    // Per-isolate cached SymbolIds (immutable after init, no sync needed)
    SymbolId sym_wsid;
    SymbolId sym_data;
    SymbolId sym_binary;
    SymbolId sym_error;
    SymbolId sym_state;
    SymbolId sym_url;

    // Class transitions (xr_class_build_json_chain) replace the previous
    // explicit shape cache; the runtime memoises identical field
    // sequences automatically.

    // Server state
    int listen_fd;  // Listen socket fd (-1 if not listening)
    volatile bool server_running;
} XrWsContext;

// Initialize per-isolate cached symbols and shapes
static void ws_ctx_init_cache(XrVMRuntime *X, XrWsContext *ctx) {
    XrSymbolTable *table = (XrSymbolTable *) xr_isolate_get_symbol_table(X);
    if (!table)
        return;
    ctx->sym_wsid = xr_symbol_register_in_table(table, "_wsId");
    ctx->sym_data = xr_symbol_register_in_table(table, "data");
    ctx->sym_binary = xr_symbol_register_in_table(table, "binary");
    ctx->sym_error = xr_symbol_register_in_table(table, "error");
    ctx->sym_state = xr_symbol_register_in_table(table, "state");
    ctx->sym_url = xr_symbol_register_in_table(table, "url");
}

// Get or create per-isolate WebSocket context
static XrWsContext *get_ws_context(XrVMRuntime *X) {
    if (!X || !X->module_registry)
        return NULL;

    XrModuleRegistry *registry = (XrModuleRegistry *) X->module_registry;
    if (!registry->loaded_modules)
        return NULL;

    XrModule *mod = (XrModule *) xr_hashmap_get((XrHashMap *) registry->loaded_modules, "ws");
    if (!mod)
        return NULL;

    XrWsContext *ctx = (XrWsContext *) mod->native_handle;
    if (!ctx) {
        ctx = (XrWsContext *) xr_calloc(1, sizeof(XrWsContext));
        if (!ctx)
            return NULL;

        ctx->conn_array = (XrWebSocket **) xr_calloc(WS_CONN_INIT_CAP, sizeof(XrWebSocket *));
        if (!ctx->conn_array) {
            xr_free(ctx);
            return NULL;
        }
        ctx->array_capacity = WS_CONN_INIT_CAP;
        atomic_store(&ctx->next_id, 1);
        xr_mutex_init(&ctx->conn_mutex);

        ctx->free_ids = (int *) xr_malloc(WS_CONN_INIT_CAP * sizeof(int));
        ctx->free_capacity = ctx->free_ids ? WS_CONN_INIT_CAP : 0;
        ctx->free_count = 0;

        ctx->listen_fd = -1;
        ctx->server_running = false;

        ws_ctx_init_cache(X, ctx);
        mod->native_handle = ctx;
    }

    return ctx;
}

// Free WebSocket module context.
static void ws_context_destroy(void *handle) {
    XrWsContext *ctx = (XrWsContext *) handle;
    if (!ctx)
        return;

    if (ctx->conn_array) {
        for (int i = 0; i < ctx->array_capacity; i++) {
            XrWebSocket *ws = ctx->conn_array[i];
            if (ws) {
                ws_conn_close(ws, WS_CLOSE_GOING_AWAY, NULL);
                ws_free(ws);
            }
        }
        xr_free(ctx->conn_array);
    }

    xr_free(ctx->free_ids);
    xr_mutex_destroy(&ctx->conn_mutex);
    xr_free(ctx);
}

// Grow connection array when needed
static bool ws_conn_array_grow(XrWsContext *ctx, int needed_id) {
    if (needed_id < ctx->array_capacity)
        return true;
    int new_cap = ctx->array_capacity;
    while (new_cap <= needed_id)
        new_cap *= 2;
    XrWebSocket **new_arr =
        (XrWebSocket **) xr_realloc(ctx->conn_array, new_cap * sizeof(XrWebSocket *));
    if (!new_arr)
        return false;
    memset(new_arr + ctx->array_capacity, 0,
           (new_cap - ctx->array_capacity) * sizeof(XrWebSocket *));
    ctx->conn_array = new_arr;
    ctx->array_capacity = new_cap;
    return true;
}

static int store_ws(XrVMRuntime *X, XrWebSocket *ws) {
    XrWsContext *ctx = get_ws_context(X);
    if (!ctx)
        return -1;

    int id;

    // Mutex protects conn_array grow + store + free-list (multi-worker safety)
    xr_mutex_lock(&ctx->conn_mutex);

    // Try recycling a free ID first
    if (ctx->free_count > 0) {
        id = ctx->free_ids[--ctx->free_count];
    } else {
        id = atomic_fetch_add(&ctx->next_id, 1);
    }

    if (!ws_conn_array_grow(ctx, id)) {
        xr_mutex_unlock(&ctx->conn_mutex);
        return -1;
    }
    ctx->conn_array[id] = ws;
    xr_mutex_unlock(&ctx->conn_mutex);

    return id;
}

// O(1) array lookup from per-isolate context
static inline XrWebSocket *get_ws_from_ctx(XrWsContext *ctx, int id) {
    if (!ctx || id < 1 || id >= ctx->array_capacity)
        return NULL;
    return ctx->conn_array[id];
}

static void remove_ws(XrWsContext *ctx, int id) {
    if (!ctx || id < 1 || id >= ctx->array_capacity)
        return;
    xr_mutex_lock(&ctx->conn_mutex);
    ctx->conn_array[id] = NULL;
    // Push ID onto free-list for recycling
    if (ctx->free_count < ctx->free_capacity) {
        ctx->free_ids[ctx->free_count++] = id;
    } else if (ctx->free_capacity > 0) {
        int new_cap = ctx->free_capacity * 2;
        int *new_ids = (int *) xr_realloc(ctx->free_ids, new_cap * sizeof(int));
        if (new_ids) {
            ctx->free_ids = new_ids;
            ctx->free_capacity = new_cap;
            ctx->free_ids[ctx->free_count++] = id;
        }
    }
    xr_mutex_unlock(&ctx->conn_mutex);
}

/* ========== WebSocket API Implementation ========== */

/*
 * ws.connect(url: string, options?: WsConnectOptions?) -> WsConn?
 *
 * Connect to WebSocket server.
 * Returns: { _wsId: int, url: string, state: string, error?: string }
 *
 * The returned object can be used with ws.send(), ws.recv(), ws.close()
 */
// State for the yieldable client handshake. The XrWebSocket itself carries the
// connect phase machine progress (see ws.c); this only holds cfunc-level data
// needed to build the result handle after the coroutine resumes.
typedef struct WsConnectState {
    XrWebSocket *ws;
    char *url;  // owned copy, for the result handle's url field
    size_t url_len;
    int timeout_ms;
} WsConnectState;

// Build the success handle: { _wsId, url, state: "open" }.
static XrCFuncResult ws_connect_finish_ok(XrVMRuntime *X, WsConnectState *state, XrValue *result) {
    XrWsContext *ctx = get_ws_context(X);
    int id = store_ws(X, state->ws);
    XrObjectInstance *r = ws_conn_new(X);
    if (ctx) {
        xr_object_instance_set(X, r, ctx->sym_wsid, xr_int(id));
        xr_object_instance_set(X, r, ctx->sym_url, ws_make_string(X, state->url, state->url_len));
        xr_object_instance_set(X, r, ctx->sym_state, xrs_string_value_c(X, "open"));
    }
    *result = xr_object_instance_value(r);
    xr_free(state->url);
    xr_free(state);
    return XR_CFUNC_DONE;
}

// Build the failure handle: { _wsId: -1, error, state: "closed" } and free ws.
static XrCFuncResult ws_connect_finish_err(XrVMRuntime *X, WsConnectState *state, XrWsError err,
                                           XrValue *result) {
    XrWsContext *ctx = get_ws_context(X);
    (void) ctx;
    (void) err;
    *result = xr_null();
    ws_free(state->ws);
    xr_free(state->url);
    xr_free(state);
    return XR_CFUNC_DONE;
}

static XrCFuncResult ws_connect_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                         void *cbx, XrValue *result);

// Advance the handshake; finish on success/error, otherwise yield for the next
// I/O event the phase machine is waiting on.
static XrCFuncResult ws_connect_drive(XrVMRuntime *X, WsConnectState *state, XrValue *result) {
    int ev = ws_connect_pump(state->ws);
    if (ev == 0)
        return ws_connect_finish_ok(X, state, result);
    if (ev < 0)
        return ws_connect_finish_err(X, state, (XrWsError) (-ev), result);
    return xr_yield_for_io(X, state->ws->fd, ev, state->timeout_ms, ws_connect_continue, state,
                           result);
}

static XrCFuncResult ws_connect_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                         void *cbx, XrValue *result) {
    (void) resume_value;
    WsConnectState *state = (WsConnectState *) cbx;
    if (status == XR_RESUME_TIMEOUT)
        return ws_connect_finish_err(X, state, WS_ERR_TIMEOUT, result);
    if (status == XR_RESUME_CANCELLED)
        return ws_connect_finish_err(X, state, WS_ERR_CONNECT, result);
    return ws_connect_drive(X, state, result);
}

/*
 * ws.connect(url, options?) -> WsConn? (yieldable)
 *
 * DNS is resolved synchronously (getaddrinfo self-hands-off P while it blocks);
 * the TCP connect, optional TLS handshake and the WebSocket upgrade exchange all
 * run non-blocking and suspend the coroutine via netpoll. No worker thread is
 * pinned and no P is handed off for the handshake's duration, so there is no
 * cross-thread contention on the handed-off P's timer wheel.
 */
static XrCFuncResult ws_connect_yieldable(XrVMRuntime *X, XrValue *args, int argc,
                                          XrValue *result) {
    XrWsContext *ctx = get_ws_context(X);
    if (argc < 1 || !ctx) {
        *result = xr_null();
        return XR_CFUNC_DONE;
    }

    size_t url_len;
    const char *url = xrs_string_arg(args[0], &url_len);
    if (!url) {
        *result = xr_null();
        return XR_CFUNC_DONE;
    }

    char *url_copy = (char *) xr_malloc(url_len + 1);
    if (!url_copy) {
        *result = xr_null();
        return XR_CFUNC_DONE;
    }
    memcpy(url_copy, url, url_len);
    url_copy[url_len] = '\0';

    XrWsConfig config;
    ws_config_init(&config);
    config.url = url_copy;

    if (argc >= 2 && XR_IS_PTR(args[1])) {
        XrObjectInstance *opts = (XrObjectInstance *) XR_TO_PTR(args[1]);
        if (opts) {
            XrValue v;
            v = xr_object_instance_get_by_key(X, opts, "timeout");
            if (XR_IS_INT(v))
                config.connect_timeout_ms = (int) XR_TO_INT(v);
            v = xr_object_instance_get_by_key(X, opts, "pingInterval");
            if (XR_IS_INT(v))
                config.ping_interval_ms = (int) XR_TO_INT(v);
            v = xr_object_instance_get_by_key(X, opts, "pongTimeout");
            if (XR_IS_INT(v))
                config.pong_timeout_ms = (int) XR_TO_INT(v);
            v = xr_object_instance_get_by_key(X, opts, "maxMessageSize");
            if (XR_IS_INT(v))
                config.max_message_size = (size_t) XR_TO_INT(v);
        }
    }

    XrWsError url_err = ws_url_validate(config.url);
    if (url_err != WS_OK) {
        *result = xr_null();
        xr_free(url_copy);
        return XR_CFUNC_DONE;
    }

    XrWebSocket *ws = ws_new(&config);
    xr_free(url_copy);

    if (!ws) {
        *result = xr_null();
        return XR_CFUNC_DONE;
    }

    // Bind the scheduler isolate so the handshake cooperates with netpoll.
    ws_set_isolate(ws, X);

    WsConnectState *state = (WsConnectState *) xr_malloc(sizeof(WsConnectState));
    if (!state) {
        ws_free(ws);
        *result = xr_null();
        return XR_CFUNC_ERROR;
    }
    state->ws = ws;
    state->url_len = url_len;
    state->url = (char *) xr_malloc(url_len + 1);
    if (!state->url) {
        ws_free(ws);
        xr_free(state);
        *result = xr_null();
        return XR_CFUNC_ERROR;
    }
    memcpy(state->url, url, url_len);
    state->url[url_len] = '\0';
    state->timeout_ms = config.connect_timeout_ms > 0 ? config.connect_timeout_ms : 10000;

    int ev = ws_connect_start(ws);
    if (ev < 0)
        return ws_connect_finish_err(X, state, (XrWsError) (-ev), result);
    return xr_yield_for_io(X, ws->fd, ev, state->timeout_ms, ws_connect_continue, state, result);
}

/* ========== Yieldable send implementation ========== */

// State for yieldable send operation
typedef struct WsSendState {
    int ws_id;        // WebSocket connection ID
    char *data;       // Data to send (copy for coroutine safety)
    size_t len;       // Data length
    bool binary;      // Binary or text
    bool data_owned;  // Whether we own the data buffer
} WsSendState;

// Forward declaration
static XrCFuncResult ws_send_step(XrVMRuntime *X, WsSendState *state, XrValue *result);

// Continuation for send
static XrCFuncResult ws_send_continue(XrVMRuntime *X, int status, XrValue resume_value, void *ctx,
                                      XrValue *result) {
    (void) resume_value;
    WsSendState *state = (WsSendState *) ctx;

    // Handle timeout/cancel
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        if (state->data_owned && state->data)
            xr_free(state->data);
        xr_free(state);
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    return ws_send_step(X, state, result);
}

// Single step of send operation
static XrCFuncResult ws_send_step(XrVMRuntime *X, WsSendState *state, XrValue *result) {
    XrWsContext *ctx = get_ws_context(X);
    XrWebSocket *ws = get_ws_from_ctx(ctx, state->ws_id);
    if (!ws || ws->state != WS_STATE_OPEN) {
        if (state->data_owned && state->data)
            xr_free(state->data);
        xr_free(state);
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    XrWsOpcode opcode = state->binary ? WS_OPCODE_BINARY : WS_OPCODE_TEXT;
    int ret = ws_conn_send_frame_try(ws, opcode, state->data, state->len);

    if (ret == 0) {
        if (state->data_owned && state->data)
            xr_free(state->data);
        xr_free(state);
        *result = xr_bool(true);
        return XR_CFUNC_DONE;
    }

    if (ret == -2) {
        // Would block - yield for the event the send path needs (TLS may want
        // read during a write, e.g. renegotiation).
        return xr_yield_for_io(X, ws->fd, ws->send_wait_event, 5000, ws_send_continue, state,
                               result);
    }

    // Error
    if (state->data_owned && state->data)
        xr_free(state->data);
    xr_free(state);
    *result = xr_bool(false);
    return XR_CFUNC_DONE;
}

/*
 * ws._send yieldable version
 * Uses netpoll for non-blocking send with coroutine yield.
 */
static XrCFuncResult ws_send_yieldable(XrVMRuntime *X, XrValue *args, int argc, XrValue *result) {
    if (argc < 2) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    // Get connection
    if (!xr_value_has_object_shape(args[0])) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    XrWsContext *ctx = get_ws_context(X);
    if (!ctx) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    XrObjectInstance *conn = (XrObjectInstance *) XR_TO_PTR(args[0]);
    XrValue id_val = xr_object_instance_get(X, conn, ctx->sym_wsid);

    if (!XR_IS_INT(id_val)) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    int id = (int) XR_TO_INT(id_val);
    XrWebSocket *ws = get_ws_from_ctx(ctx, id);

    if (!ws || ws->state != WS_STATE_OPEN) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    size_t msg_len;
    const char *msg = get_data_arg(args[1], &msg_len);
    if (!msg) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    bool binary = false;
    if (argc >= 3 && XR_IS_BOOL(args[2])) {
        binary = XR_TO_BOOL(args[2]);
    }

    XrWsOpcode opcode = binary ? WS_OPCODE_BINARY : WS_OPCODE_TEXT;
    int ret = ws_conn_send_frame_try(ws, opcode, msg, msg_len);

    if (ret == 0) {
        *result = xr_bool(true);
        return XR_CFUNC_DONE;
    }

    if (ret == -1) {
        // Error
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    // Would block - need to yield. Copy data since original may be invalid after yield
    WsSendState *state = (WsSendState *) xr_malloc(sizeof(WsSendState));
    if (!state) {
        *result = xr_bool(false);
        return XR_CFUNC_ERROR;
    }

    state->ws_id = id;
    state->data = (char *) xr_malloc(msg_len);
    if (!state->data) {
        xr_free(state);
        *result = xr_bool(false);
        return XR_CFUNC_ERROR;
    }
    memcpy(state->data, msg, msg_len);
    state->len = msg_len;
    state->binary = binary;
    state->data_owned = true;

    return xr_yield_for_io(X, ws->fd, XR_WAIT_WRITE, 5000, ws_send_continue, state, result);
}

/* ========== Yieldable recv implementation ========== */

// State for yieldable recv operation
// Following the same pattern as NetReadState in net.c
// IMPORTANT: Do NOT store XrValue across yield points; retain primitive ids and
// reconstruct RC handles after resume.
typedef struct WsRecvState {
    int ws_id;           // WebSocket connection ID (primitive, resume-safe)
    int64_t timeout_ms;  // Timeout in milliseconds (-1 = infinite)
} WsRecvState;

// Helper to create result JSON from message
// NOTE: No XrValue parameters; values are reconstructed after resume.
static XrValue make_recv_result(XrVMRuntime *X, XrWsContext *ctx, XrWebSocket *ws,
                                XrWsMessage *msg) {
    XrCoroutine *coro = xr_current_coro(X);
    XrObjectInstance *result = ws_message_new(X);
    if (!result) {
        if (msg)
            ws_message_free(msg);
        return xr_null();
    }

    /* Always populate every WsMessage field so callers can read msg.data /
     * msg.binary / msg.error without null guards. data is null when there
     * is no payload (error or empty frame), error is null on success. The
     * class transition cache makes repeated allocations cheap by reusing
     * the same hidden class. */
    XrValue data_val = xr_null();
    bool is_binary = false;
    XrValue error_val = xr_null();

    if (msg) {
        if (msg->is_text) {
            data_val = ws_make_string(X, msg->data, msg->len);
        } else {
            is_binary = true;
            XrArray *bytes_arr = xr_array_with_capacity_typed(coro, (int) msg->len, XR_ELEM_U8);
            if (bytes_arr && msg->len > 0) {
                memcpy(bytes_arr->data, msg->data, msg->len);
                bytes_arr->length = (int32_t) msg->len;
            }
            data_val = bytes_arr ? xr_value_from_array(bytes_arr) : xr_null();
        }
        ws_message_free(msg);
    } else {
        const char *err_msg =
            (!ws || ws->state != WS_STATE_OPEN) ? "Connection closed" : "Receive failed";
        error_val = xrs_string_value_c(X, err_msg);
    }

    xr_object_instance_set(X, result, ctx ? ctx->sym_data : 0, data_val);
    xr_object_instance_set(X, result, ctx ? ctx->sym_binary : 0, xr_bool(is_binary));
    xr_object_instance_set(X, result, ctx ? ctx->sym_error : 0, error_val);

    return xr_object_instance_value(result);
}

// Forward declaration
static XrCFuncResult ws_recv_step(XrVMRuntime *X, WsRecvState *state, XrValue *result);

// Continuation function for ws.recv (matches XrContinuation signature)
static XrCFuncResult ws_recv_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                      void *cont_ctx, XrValue *result) {
    (void) resume_value;
    WsRecvState *state = (WsRecvState *) cont_ctx;

    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        XrWsContext *ctx = get_ws_context(X);
        XrObjectInstance *res = ws_message_new(X);
        xr_object_instance_set(X, res, ctx ? ctx->sym_data : 0, xr_null());
        xr_object_instance_set(X, res, ctx ? ctx->sym_binary : 0, xr_bool(false));
        xr_object_instance_set(X, res, ctx ? ctx->sym_error : 0, xrs_string_value_c(X, "Timeout"));
        *result = xr_object_instance_value(res);
        xr_free(state);
        return XR_CFUNC_DONE;
    }

    return ws_recv_step(X, state, result);
}

// Single step of recv operation (following net_read_step pattern)
static XrCFuncResult ws_recv_step(XrVMRuntime *X, WsRecvState *state, XrValue *result) {
    XrWsContext *ctx = get_ws_context(X);
    XrWebSocket *ws = get_ws_from_ctx(ctx, state->ws_id);
    if (!ws) {
        *result = make_recv_result(X, ctx, NULL, NULL);
        xr_free(state);
        return XR_CFUNC_DONE;
    }

    if (ws->state != WS_STATE_OPEN) {
        *result = make_recv_result(X, ctx, ws, NULL);
        xr_free(state);
        return XR_CFUNC_DONE;
    }

    bool need_more = false;
    XrWsMessage *msg = ws_conn_recv_try(ws, &need_more);

    if (msg) {
        *result = make_recv_result(X, ctx, ws, msg);
        xr_free(state);
        return XR_CFUNC_DONE;
    }

    if (!need_more) {
        *result = make_recv_result(X, ctx, ws, NULL);
        xr_free(state);
        return XR_CFUNC_DONE;
    }

    return xr_yield_for_io(X, ws->fd, ws->recv_wait_event, state->timeout_ms, ws_recv_continue,
                           state, result);
}

/*
 * ws._recv yieldable version
 * Uses netpoll for efficient I/O multiplexing.
 *
 * Fast path: if data is already in kernel buffer (common for localhost echo),
 * process immediately without allocating state or yielding to kqueue.
 * This avoids coroutine suspend/resume overhead (~2-5us per message).
 *
 * Parameters:
 *   args[0]: connection object (Json with _wsId)
 *   args[1]: timeout in milliseconds (optional, -1 = infinite)
 */
static XrCFuncResult ws_recv_yieldable(XrVMRuntime *X, XrValue *args, int argc, XrValue *result) {
    if (argc < 1 || !xr_value_has_object_shape(args[0])) {
        *result = xr_null();
        return XR_CFUNC_ERROR;
    }

    XrWsContext *ctx = get_ws_context(X);
    if (!ctx) {
        *result = xr_null();
        return XR_CFUNC_ERROR;
    }

    XrObjectInstance *conn = (XrObjectInstance *) XR_TO_PTR(args[0]);
    XrValue id_val = xr_object_instance_get(X, conn, ctx->sym_wsid);

    if (!XR_IS_INT(id_val)) {
        XrObjectInstance *res = ws_message_new(X);
        xr_object_instance_set(X, res, ctx->sym_data, xr_null());
        xr_object_instance_set(X, res, ctx->sym_binary, xr_bool(false));
        xr_object_instance_set(X, res, ctx->sym_error,
                               xrs_string_value_c(X, "Invalid connection object"));
        *result = xr_object_instance_value(res);
        return XR_CFUNC_DONE;
    }

    int id = (int) XR_TO_INT(id_val);
    XrWebSocket *ws = get_ws_from_ctx(ctx, id);

    if (!ws) {
        XrObjectInstance *res = ws_message_new(X);
        xr_object_instance_set(X, res, ctx->sym_data, xr_null());
        xr_object_instance_set(X, res, ctx->sym_binary, xr_bool(false));
        xr_object_instance_set(X, res, ctx->sym_error,
                               xrs_string_value_c(X, "Connection not found"));
        *result = xr_object_instance_value(res);
        return XR_CFUNC_DONE;
    }

    // Fast path: try recv without allocating state or yielding
    if (ws->state == WS_STATE_OPEN) {
        bool need_more = false;
        XrWsMessage *msg = ws_conn_recv_try(ws, &need_more);

        if (msg) {
            *result = make_recv_result(X, ctx, ws, msg);
            return XR_CFUNC_DONE;
        }

        if (!need_more) {
            *result = make_recv_result(X, ctx, ws, NULL);
            return XR_CFUNC_DONE;
        }
    } else {
        *result = make_recv_result(X, ctx, ws, NULL);
        return XR_CFUNC_DONE;
    }

    int64_t timeout_ms = -1;
    if (argc >= 2 && XR_IS_INT(args[1])) {
        timeout_ms = XR_TO_INT(args[1]);
    }

    // Slow path: no data available, allocate state and yield to kqueue
    WsRecvState *state = (WsRecvState *) xr_malloc(sizeof(WsRecvState));
    if (!state) {
        XrObjectInstance *res = ws_message_new(X);
        xr_object_instance_set(X, res, ctx->sym_data, xr_null());
        xr_object_instance_set(X, res, ctx->sym_binary, xr_bool(false));
        xr_object_instance_set(X, res, ctx->sym_error, xrs_string_value_c(X, "Out of memory"));
        *result = xr_object_instance_value(res);
        return XR_CFUNC_ERROR;
    }

    state->ws_id = id;
    state->timeout_ms = timeout_ms;

    return xr_yield_for_io(X, ws->fd, ws->recv_wait_event, state->timeout_ms, ws_recv_continue,
                           state, result);
}

/*
 * ws.close(conn: Json, code?: int, reason?: string) -> bool
 *
 * Close WebSocket connection.
 */
static XrValue ws_close(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_bool(false);

    XrWsContext *ctx = get_ws_context(X);
    if (!ctx)
        return xr_bool(false);

    if (!xr_value_has_object_shape(args[0]))
        return xr_bool(false);

    XrObjectInstance *conn = (XrObjectInstance *) XR_TO_PTR(args[0]);
    XrValue id_val = xr_object_instance_get(X, conn, ctx->sym_wsid);

    if (!XR_IS_INT(id_val)) {
        return xr_bool(false);
    }

    int id = (int) XR_TO_INT(id_val);
    XrWebSocket *ws = get_ws_from_ctx(ctx, id);

    if (!ws) {
        return xr_bool(false);
    }

    int code = WS_CLOSE_NORMAL;
    const char *reason = NULL;

    if (argc >= 2 && XR_IS_INT(args[1])) {
        code = (int) XR_TO_INT(args[1]);
    }

    if (argc >= 3 && XR_IS_STRING(args[2])) {
        reason = xrs_string_arg(args[2], NULL);
    }

    // CRITICAL: Clean up netpoll registration BEFORE closing the socket.
    // This prevents stale XrPollDesc from being reused when the fd is recycled by the OS.
    // Without this, the next connection using the same fd number would inherit
    // the old XrPollDesc with stale coroutine pointers, causing hangs or crashes.
    // NOTE: ws_conn_recv_try may have already set state to CLOSED but leaves fd open
    // for us to clean up here.
    if (ws->fd >= 0) {
        XrRuntime *runtime = X ? (XrRuntime *) X->vm.scheduler : NULL;
        if (runtime) {
            XrPollDesc *pd = xr_fdmap_get(&runtime->netpoll, ws->fd);
            if (pd) {
                xr_netpoll_close(&runtime->netpoll, pd);
            }
        }
    }

    // ws_conn_close sends close frame if state is OPEN, otherwise no-op
    ws_conn_close(ws, code, reason);
    ws_free(ws);
    remove_ws(ctx, id);

    xr_object_instance_set(X, conn, ctx->sym_state, xrs_string_value_c(X, "closed"));
    xr_object_instance_set(X, conn, ctx->sym_wsid, xr_int(-1));

    return xr_bool(true);
}

/*
 * ws.ping(conn: Json) -> bool
 *
 * Send ping frame.
 */
static XrValue ws_ping(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1 || !xr_value_has_object_shape(args[0])) {
        return xr_bool(false);
    }

    XrWsContext *ctx = get_ws_context(X);
    if (!ctx)
        return xr_bool(false);

    XrObjectInstance *conn = (XrObjectInstance *) XR_TO_PTR(args[0]);
    XrValue id_val = xr_object_instance_get(X, conn, ctx->sym_wsid);

    if (!XR_IS_INT(id_val))
        return xr_bool(false);

    XrWebSocket *ws = get_ws_from_ctx(ctx, (int) XR_TO_INT(id_val));
    if (!ws)
        return xr_bool(false);

    return xr_bool(ws_conn_ping(ws) == WS_OK);
}

/* ========== WebSocket Server API Implementation ========== */

/*
 * Create server connection object from upgraded WebSocket.
 * url_str/url_len carries the request path (e.g. "/chat") so user handlers
 * can route on it; pass NULL/0 if no URL info is available.
 * Used by ws.serve() when a client connects.
 */
static XrValue ws_wrap_server_conn(XrVMRuntime *X, XrWebSocket *ws, const char *url_str,
                                   size_t url_len) {
    if (!ws)
        return xr_null();

    XrWsContext *ctx = get_ws_context(X);
    if (!ctx)
        return xr_null();

    int id = store_ws(X, ws);

    XrObjectInstance *result = ws_conn_new(X);
    xr_object_instance_set(X, result, ctx->sym_wsid, xr_int(id));
    XrValue url_val =
        (url_str && url_len > 0) ? ws_make_string(X, url_str, url_len) : ws_make_string(X, "", 0);
    xr_object_instance_set(X, result, ctx->sym_url, url_val);
    xr_object_instance_set(X, result, ctx->sym_state, xrs_string_value_c(X, "open"));

    return xr_object_instance_value(result);
}

/* ========== WebSocket Server (Stackless) ========== */

#define WS_UPGRADE_BUF_SIZE 4096
#define WS_HTTP_BACKLOG 1024

/* ========== WebSocket Server (ws.serve) — Stackless ========== */

// Forward declarations for conn handler continuations
static XrCFuncResult ws_conn_upgrade_cont(XrVMRuntime *X, int status, XrValue resume_value,
                                          void *ctx, XrValue *result);
static XrCFuncResult ws_conn_handler_done(XrVMRuntime *X, int status, XrValue resume_value,
                                          void *ctx, XrValue *result);
static XrCFuncResult ws_serve_listen_cont(XrVMRuntime *X, int status, XrValue resume_value,
                                          void *ctx, XrValue *result);

/*
 * Context for WS connection handler — persists across yields.
 * Replaces stack-local variables from the old stackful handler.
 */
typedef struct {
    XrVMRuntime *X;
    int fd;
    XrClosure *handler;
    XrRuntime *runtime;
    XrValue conn;  // wrapped WS connection object
    char *upgrade_buf;
    int upgrade_buf_used;
} WsConnCtx;

/*
 * WS connection entry — cfunc coroutine entry point.
 *
 * Stackless replacement for ws_conn_stackful.
 * args[0] = client fd (int), args[1] = handler closure (ptr).
 */
static XrCFuncResult ws_conn_init(XrVMRuntime *X, XrValue *args, int argc, XrValue *result) {
    (void) result;
    if (argc < 2 || !XR_IS_INT(args[0]))
        return XR_CFUNC_DONE;

    // The handler closure was already validated in ws_serve_yieldable; this
    // entry runs per accepted connection and ws_serve passes the same value
    // through unchanged. Re-check here so a corrupted dispatch path still
    // fails loudly instead of casting a non-closure to XrClosure*.
    XrClosure *handler = xr_vm_closure_from_arg(X, args[1], "ws.serve handler");
    if (!handler)
        return XR_CFUNC_DONE;

    int fd = (int) XR_TO_INT(args[0]);

    WsConnCtx *ctx = (WsConnCtx *) xr_calloc(1, sizeof(WsConnCtx));
    if (!ctx) {
        xr_closesocket(fd);
        return XR_CFUNC_DONE;
    }

    ctx->X = X;
    ctx->fd = fd;
    ctx->handler = handler;
    ctx->runtime = (XrRuntime *) X->vm.scheduler;
    ctx->upgrade_buf = (char *) xr_malloc(WS_UPGRADE_BUF_SIZE);
    if (!ctx->upgrade_buf) {
        xr_closesocket(fd);
        xr_free(ctx);
        return XR_CFUNC_DONE;
    }
    ctx->upgrade_buf_used = 0;

    // Start reading upgrade request
    return ws_conn_upgrade_cont(X, XR_RESUME_IO_READY, xr_null(), ctx, result);
}

/*
 * Continuation: read HTTP upgrade headers, validate, upgrade to WS,
 * then call user handler closure via xr_call_closure.
 */
static XrCFuncResult ws_conn_upgrade_cont(XrVMRuntime *X, int status, XrValue resume_value,
                                          void *user_ctx, XrValue *result) {
    (void) resume_value;
    WsConnCtx *ctx = (WsConnCtx *) user_ctx;
    if (status != XR_RESUME_IO_READY)
        goto fail;

    for (;;) {
        if (ctx->upgrade_buf_used >= WS_UPGRADE_BUF_SIZE - 1)
            goto fail;
        ssize_t n = read(ctx->fd, ctx->upgrade_buf + ctx->upgrade_buf_used,
                         WS_UPGRADE_BUF_SIZE - 1 - ctx->upgrade_buf_used);
        if (n > 0) {
            ctx->upgrade_buf_used += (int) n;
            ctx->upgrade_buf[ctx->upgrade_buf_used] = '\0';
            if (strstr(ctx->upgrade_buf, "\r\n\r\n"))
                break;
            continue;
        }
        if (n == 0)
            goto fail;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            goto fail;
        return xr_yield_for_io(X, ctx->fd, XR_WAIT_READ, 5000, ws_conn_upgrade_cont, ctx, result);
    }

    if (!ws_is_upgrade_request(ctx->upgrade_buf)) {
        const char *r400 = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n"
                           "Connection: close\r\n\r\n";
        (void) ws_write_all(X, ctx->fd, r400, strlen(r400));
        goto fail;
    }

    // Upgrade to WebSocket
    {
        /* Extract request path from the request line ("GET /chat HTTP/1.1")
         * before ws_upgrade_ex consumes the buffer, so the server-side conn
         * object can expose conn.url to the handler. */
        const char *url_str = NULL;
        size_t url_len = 0;
        {
            const char *p = ctx->upgrade_buf;
            const char *sp1 = strchr(p, ' ');
            if (sp1) {
                const char *path_start = sp1 + 1;
                const char *sp2 = strchr(path_start, ' ');
                if (sp2 && sp2 > path_start) {
                    url_str = path_start;
                    url_len = (size_t) (sp2 - path_start);
                }
            }
        }
        XrWebSocket *ws = ws_upgrade_ex(X, ctx->fd, ctx->upgrade_buf, NULL);
        XrValue conn_val = ws_wrap_server_conn(X, ws, url_str, url_len);
        xr_free(ctx->upgrade_buf);
        ctx->upgrade_buf = NULL;
        if (!ws)
            goto cleanup;

        ctx->conn = conn_val;
        if (XR_IS_NULL(ctx->conn)) {
            ws_conn_close(ws, WS_CLOSE_SERVER_ERROR, NULL);
            ws_free(ws);
            goto cleanup;
        }

        // Call user handler closure — resumes ws_conn_handler_done on return
        return xr_call_closure(X, ctx->handler, &ctx->conn, 1, ws_conn_handler_done, ctx, result);
    }

fail:
    xr_free(ctx->upgrade_buf);
cleanup: {
    XrRuntime *rt = (XrRuntime *) X->vm.scheduler;
    if (rt) {
        XrPollDesc *pd = xr_netpoll_open(&rt->netpoll, ctx->fd);
        if (pd)
            xr_netpoll_close(&rt->netpoll, pd);
    }
}
    xr_closesocket(ctx->fd);
    xr_free(ctx);
    return XR_CFUNC_DONE;
}

/*
 * Continuation: called when user handler closure returns.
 * Cleans up WebSocket connection and frees context.
 */
static XrCFuncResult ws_conn_handler_done(XrVMRuntime *X, int status, XrValue resume_value,
                                          void *user_ctx, XrValue *result) {
    (void) status;
    (void) resume_value;
    (void) result;
    WsConnCtx *ctx = (WsConnCtx *) user_ctx;

    // Always close when handler exits (safe even if already closed)
    XrWsContext *ws_ctx = get_ws_context(X);
    if (ws_ctx && !XR_IS_NULL(ctx->conn)) {
        XrObjectInstance *conn_obj = (XrObjectInstance *) XR_TO_PTR(ctx->conn);
        XrValue id_val = xr_object_instance_get(X, conn_obj, ws_ctx->sym_wsid);
        if (XR_IS_INT(id_val)) {
            int id = (int) XR_TO_INT(id_val);
            XrWebSocket *w = get_ws_from_ctx(ws_ctx, id);
            if (w) {
                if (w->fd >= 0 && ctx->runtime) {
                    XrPollDesc *pd = xr_fdmap_get(&ctx->runtime->netpoll, w->fd);
                    if (pd)
                        xr_netpoll_close(&ctx->runtime->netpoll, pd);
                }
                ws_conn_close(w, WS_CLOSE_NORMAL, NULL);
                ws_free(w);
                remove_ws(ws_ctx, id);
            }
        }
    }

    xr_free(ctx);
    return XR_CFUNC_DONE;
}

/*
 * WS serve listen context — persists across accept loop yields.
 */
typedef struct {
    XrVMRuntime *X;
    int listen_fd;
    XrValue handler_val;  // closure XrValue (for passing to conn coroutines)
} WsServeListenCtx;

/*
 * WS serve listen entry — cfunc coroutine entry point.
 * args[0] = listen fd (int), args[1] = handler closure (ptr).
 */
static XrCFuncResult ws_serve_listen_init(XrVMRuntime *X, XrValue *args, int argc,
                                          XrValue *result) {
    (void) result;
    if (argc < 2 || !XR_IS_INT(args[0]))
        return XR_CFUNC_DONE;

    WsServeListenCtx *ctx = (WsServeListenCtx *) xr_calloc(1, sizeof(WsServeListenCtx));
    if (!ctx)
        return XR_CFUNC_DONE;

    ctx->X = X;
    ctx->listen_fd = (int) XR_TO_INT(args[0]);
    ctx->handler_val = args[1];

    // Yield for first accept readiness
    return xr_yield_for_io(X, ctx->listen_fd, XR_WAIT_READ, -1, ws_serve_listen_cont, ctx, result);
}

/*
 * WS serve accept loop continuation — accepts connections,
 * spawns a cfunc conn coroutine per client, then yields for next batch.
 */
static XrCFuncResult ws_serve_listen_cont(XrVMRuntime *X, int status, XrValue resume_value,
                                          void *user_ctx, XrValue *result) {
    (void) resume_value;
    WsServeListenCtx *ctx = (WsServeListenCtx *) user_ctx;

    XrWsContext *ws_ctx = get_ws_context(X);
    if (!ws_ctx || !ws_ctx->server_running || status != XR_RESUME_IO_READY) {
        xr_free(ctx);
        return XR_CFUNC_DONE;
    }

    for (;;) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        int client_fd = accept(ctx->listen_fd, (struct sockaddr *) &addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            break;
        }

        xr_socket_set_nonblock(client_fd);
        xr_socket_set_nodelay(client_fd, true);
#ifdef SO_NOSIGPIPE
        {
            int flag = 1;
            setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *) &flag, sizeof(flag));
        }
#endif

        // Spawn stackless connection coroutine
        XrValue conn_args[2] = {xr_int(client_fd), ctx->handler_val};
        XrCoroutine *coro = xr_coro_create_vm_cfunc(X, ws_conn_init, conn_args, 2, "ws.conn");
        if (coro) {
            xr_coro_spawn(X, coro);
        } else {
            xr_closesocket(client_fd);
        }
    }

    // Yield for next accept batch
    return xr_yield_for_io(X, ctx->listen_fd, XR_WAIT_READ, -1, ws_serve_listen_cont, ctx, result);
}

// Continuation: keep caller coroutine blocked while server is running
static XrCFuncResult ws_serve_wait_cont(XrVMRuntime *X, int status, XrValue resume_value,
                                        void *cont_ctx, XrValue *result) {
    (void) status;
    (void) resume_value;
    XrWsContext *ctx = (XrWsContext *) cont_ctx;
    if (!ctx || !ctx->server_running) {
        *result = xr_bool(true);
        return XR_CFUNC_DONE;
    }
    return xr_yield_for_timeout(X, 1000, ws_serve_wait_cont, ctx, result);
}

/*
 * ws.serve(port: int, handler: fn(conn)) -> bool
 *
 * Start WebSocket server. Creates listen socket, spawns accept loop,
 * blocks caller until server stops.
 */
static XrCFuncResult ws_serve_yieldable(XrVMRuntime *X, XrValue *args, int argc, XrValue *result) {
    if (argc < 2) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }
    if (!XR_IS_INT(args[0])) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    int port = (int) XR_TO_INT(args[0]);

    // The handler arg must be a real closure -- XR_IS_PTR would also accept
    // any other heap object (string, array, json, ...) and the connection
    // path would later call xr_vm_call_closure on a garbage pointer.
    if (!xr_vm_closure_from_arg(X, args[1], "ws.serve")) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    XrWsContext *ctx = get_ws_context(X);
    if (!ctx) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    if (ctx->server_running) {
        fprintf(stderr, "ws.serve: server already running\n");
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    int listen_fd = xr_socket_listen("0.0.0.0", port, WS_HTTP_BACKLOG);
    if (listen_fd < 0) {
        fprintf(stderr, "ws.serve: cannot listen on port %d\n", port);
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    ctx->listen_fd = listen_fd;
    ctx->server_running = true;

    printf("=== xray WebSocket Server ===\n");
    printf("Port: %d\n", port);
    printf("Listening...\n");

    // Spawn stackless accept loop coroutine
    XrValue listen_args[2] = {xr_int(listen_fd), args[1]};
    XrCoroutine *listen_coro =
        xr_coro_create_vm_cfunc(X, ws_serve_listen_init, listen_args, 2, "ws.listen");
    if (!listen_coro) {
        xr_closesocket(listen_fd);
        ctx->listen_fd = -1;
        ctx->server_running = false;
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }
    xr_coro_spawn(X, listen_coro);

    // Block caller until server stops
    return xr_yield_for_timeout(X, 1000, ws_serve_wait_cont, ctx, result);
}

/*
 * ws.stopServer() -> void
 */
static XrValue ws_stop_server(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrWsContext *ctx = get_ws_context(X);
    if (!ctx || !ctx->server_running)
        return xr_null();

    ctx->server_running = false;

    if (ctx->listen_fd >= 0) {
        xr_closesocket(ctx->listen_fd);
        ctx->listen_fd = -1;
    }

    return xr_null();
}

/* ========== HTTP Server Integration ========== */

/*
 * Upgrade HTTP connection to WebSocket and wrap as script-visible Json.
 * Called from http_listen.c when a WS route matches an Upgrade request.
 * Performs the full handshake (101 response) and returns a conn object
 * identical to what ws._acceptWs() returns.
 */
XrValue ws_upgrade_and_wrap(XrVMRuntime *X, int fd, const char *request_headers) {
    /* Extract request path from "METHOD /path HTTP/1.1" before the upgrade
     * consumes/mutates the buffer; pass through to wrap so conn.url exists
     * for handlers routed via http.route + ws upgrade. */
    const char *url_str = NULL;
    size_t url_len = 0;
    if (request_headers) {
        const char *sp1 = strchr(request_headers, ' ');
        if (sp1) {
            const char *path_start = sp1 + 1;
            const char *sp2 = strchr(path_start, ' ');
            if (sp2 && sp2 > path_start) {
                url_str = path_start;
                url_len = (size_t) (sp2 - path_start);
            }
        }
    }
    XrWebSocket *ws = ws_upgrade_ex(X, fd, request_headers, NULL);
    if (!ws)
        return xr_null();
    return ws_wrap_server_conn(X, ws, url_str, url_len);
}

/* ========== Module Registration ========== */

#define XR_STDLIB_VM_BIND_MODULE_WS 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_WS

XR_FUNC XrModule *xr_load_module_ws(XrVMRuntime *isolate) {
    XrModule *mod = xr_module_create_native(isolate, "ws");
    if (!mod)
        return NULL;
    mod->native_handle_destroy = ws_context_destroy;

    xr_stdlib_vm_bind_ws_generated(isolate, mod);
    mod->requires_script = true;
    return mod;
}
