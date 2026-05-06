/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * ws_binding.c - WebSocket xray module binding (pure C)
 *
 * KEY CONCEPT:
 *   Pure C WebSocket module: client and server both implemented entirely
 *   in C with stackful coroutine support. No script layer (ws.xr) needed.
 *
 * WHY THIS DESIGN:
 *   - Consistent with http module (pure C, stackful coroutine server)
 *   - Eliminates mixed script/C maintenance overhead
 *   - Server uses coroutine-per-connection model via xr_coro_create_stackful
 */

#include "ws.h"
#include "../common.h"
#include "../../src/base/xmalloc.h"
#include "../../src/module/xmodule.h"
#include "../../src/vm/xvm.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/object/xmap.h"
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

extern void *xr_isolate_get_symbol_table(void *isolate);

/* ========== External Declarations ========== */

extern XrValue xr_string_value(XrString *str);
extern XrString *xr_string_intern(XrayIsolate *X, const char *str, size_t len, uint32_t hash);
struct XrCoroutine;
extern struct XrCoroutine *xr_current_coro(XrayIsolate *X);
extern XrModule *xr_module_create_native(XrayIsolate *isolate, const char *name);

// Profiling counters (compile-time toggle)
#define WS_PROFILE 0
#if WS_PROFILE
#include "../../src/os/os_time.h"
static _Atomic uint64_t ws_prof_recv_fast = 0;  // recv fast path hits (no yield)
static _Atomic uint64_t ws_prof_recv_slow = 0;  // recv slow path (yield to kqueue)
static _Atomic uint64_t ws_prof_send_fast = 0;  // send completed immediately
static _Atomic uint64_t ws_prof_send_slow = 0;  // send needed yield
static _Atomic uint64_t ws_prof_recv_ns = 0;    // total ns in recv C function
static _Atomic uint64_t ws_prof_send_ns = 0;    // total ns in send C function
static _Atomic uint64_t ws_prof_alloc_ns = 0;   // total ns in make_recv_result

static inline uint64_t ws_now_ns(void) {
    return xr_time_monotonic_ns();
}

static void ws_prof_dump(void) {
    uint64_t rf = atomic_load(&ws_prof_recv_fast);
    uint64_t rs = atomic_load(&ws_prof_recv_slow);
    uint64_t sf = atomic_load(&ws_prof_send_fast);
    uint64_t ss = atomic_load(&ws_prof_send_slow);
    uint64_t an = atomic_load(&ws_prof_alloc_ns);
    uint64_t total = rf + rs;
    if (total == 0)
        return;
    fprintf(stderr, "\n=== WS Profiling ===\n");
    fprintf(stderr, "recv: fast=%llu slow=%llu (fast%%=%.1f%%)\n", (unsigned long long) rf,
            (unsigned long long) rs, total ? 100.0 * rf / total : 0.0);
    fprintf(stderr, "send: fast=%llu slow=%llu (fast%%=%.1f%%)\n", (unsigned long long) sf,
            (unsigned long long) ss, (sf + ss) ? 100.0 * sf / (sf + ss) : 0.0);
    fprintf(stderr, "alloc: total=%.1fms avg=%.0fns/msg\n", an / 1e6, rf ? (double) an / rf : 0.0);
    fprintf(stderr, "====================\n");
}

#include <signal.h>
static void ws_prof_signal(int sig) {
    (void) sig;
    ws_prof_dump();
}
static int ws_prof_registered = 0;
#endif

/* ========== Helper Functions ========== */

// Non-interned string for WS message data (avoids rwlock + hash table lookup)
static XrValue ws_make_string(XrayIsolate *X, const char *str, size_t len) {
    if (!str)
        return xr_null();
    if (len == 0)
        return xrs_string_value_c(X, "");
    XrString *s = xr_string_new(X, str, len);
    if (!s)
        return xr_null();
    return xr_string_value(s);
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
 * Per-isolate WebSocket context - similar to XrHttpContext.
 * Each isolate has its own connection map and statistics.
 * This allows multi-isolate support and proper resource cleanup.
 */

// Include hashmap header for proper type declarations
#include "../../src/base/xhashmap.h"

#define WS_CONN_INIT_CAP 64

typedef struct XrWsContext {
    // Connection registry (protected by conn_mutex for multi-worker safety)
    XrWebSocket **conn_array;  // conn_array[id] = ws, id starts from 1
    int array_capacity;        // current capacity of conn_array
    _Atomic int next_id;       // Connection ID counter (atomic for multi-worker)
    _Atomic int conn_count;    // Active connection count
    int max_conns;             // Max connections (0 = unlimited)
    xr_mutex_t conn_mutex;     // Protects conn_array grow/store/remove

    // Free-list for ID recycling (protected by conn_mutex)
    int *free_ids;      // Stack of recycled IDs
    int free_count;     // Number of recycled IDs
    int free_capacity;  // Capacity of free_ids array

    // Statistics (atomic for multi-worker safety)
    _Atomic uint64_t total_msgs_sent;
    _Atomic uint64_t total_msgs_recv;
    _Atomic uint64_t total_bytes_sent;
    _Atomic uint64_t total_bytes_recv;

    // Per-isolate cached SymbolIds (immutable after init, no sync needed)
    SymbolId sym_wsid;
    SymbolId sym_data;
    SymbolId sym_binary;
    SymbolId sym_error;
    SymbolId sym_state;
    SymbolId sym_url;
    SymbolId sym_is_svr;

    // Per-isolate cached Shapes (immutable after init, no sync needed)
    XrShape *shape_recv_ok;   // fields: [data, binary]
    XrShape *shape_recv_err;  // fields: [error]

    // Server state
    int listen_fd;  // Listen socket fd (-1 if not listening)
    volatile bool server_running;
} XrWsContext;

// Initialize per-isolate cached symbols and shapes
static void ws_ctx_init_cache(XrayIsolate *X, XrWsContext *ctx) {
    XrSymbolTable *table = (XrSymbolTable *) xr_isolate_get_symbol_table(X);
    if (!table)
        return;
    ctx->sym_wsid = xr_symbol_register_in_table(table, "_wsId");
    ctx->sym_data = xr_symbol_register_in_table(table, "data");
    ctx->sym_binary = xr_symbol_register_in_table(table, "binary");
    ctx->sym_error = xr_symbol_register_in_table(table, "error");
    ctx->sym_state = xr_symbol_register_in_table(table, "state");
    ctx->sym_url = xr_symbol_register_in_table(table, "url");
    ctx->sym_is_svr = xr_symbol_register_in_table(table, "_isServer");

    // P5: Build Shape transition chains for recv result
    XrShape *base = xr_shape_new(X, 4);
    if (base) {
        XrShape *s1 = xr_shape_transition(X, base, ctx->sym_data);
        if (s1) {
            ctx->shape_recv_ok = xr_shape_transition(X, s1, ctx->sym_binary);
        }
    }
    XrShape *err_base = xr_shape_new(X, 4);
    if (err_base) {
        ctx->shape_recv_err = xr_shape_transition(X, err_base, ctx->sym_error);
    }
}

// Get or create per-isolate WebSocket context
static XrWsContext *get_ws_context(XrayIsolate *X) {
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

// Free WebSocket module context
void xr_ws_module_context_free(XrWsContext *ctx) {
    if (!ctx)
        return;

    if (ctx->conn_array) {
        for (int i = 0; i < ctx->array_capacity; i++) {
            XrWebSocket *ws = ctx->conn_array[i];
            if (ws) {
                xr_ws_close(ws, WS_CLOSE_GOING_AWAY, NULL);
                xr_ws_free(ws);
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

static int store_ws(XrayIsolate *X, XrWebSocket *ws) {
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

    atomic_fetch_add(&ctx->conn_count, 1);
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
    atomic_fetch_sub(&ctx->conn_count, 1);
}

/* ========== WebSocket API Implementation ========== */

/*
 * ws.connect(url: string, options?: Json) -> Json
 *
 * Connect to WebSocket server.
 * Returns: { _wsId: int, url: string, state: string, error?: string }
 *
 * The returned object can be used with ws.send(), ws.recv(), ws.close()
 */
static XrValue ws_connect(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();

    XrWsContext *ctx = get_ws_context(X);
    if (!ctx)
        return xr_null();

    size_t url_len;
    const char *url = xrs_string_arg(args[0], &url_len);
    if (!url)
        return xr_null();

    // Copy URL
    char *url_copy = (char *) xr_malloc(url_len + 1);
    if (!url_copy)
        return xr_null();
    memcpy(url_copy, url, url_len);
    url_copy[url_len] = '\0';

    // Create config
    XrWsConfig config;
    xr_ws_config_init(&config);
    config.url = url_copy;

    // Parse options from args[1] if provided
    if (argc >= 2 && XR_IS_PTR(args[1])) {
        XrJson *opts = (XrJson *) XR_TO_PTR(args[1]);
        if (opts) {
            XrValue v;
            v = xr_json_get_by_key(X, opts, "timeout");
            if (XR_IS_INT(v))
                config.connect_timeout_ms = (int) XR_TO_INT(v);
            v = xr_json_get_by_key(X, opts, "pingInterval");
            if (XR_IS_INT(v))
                config.ping_interval_ms = (int) XR_TO_INT(v);
            v = xr_json_get_by_key(X, opts, "pongTimeout");
            if (XR_IS_INT(v))
                config.pong_timeout_ms = (int) XR_TO_INT(v);
            v = xr_json_get_by_key(X, opts, "maxMessageSize");
            if (XR_IS_INT(v))
                config.max_message_size = (size_t) XR_TO_INT(v);
        }
    }

    // Create WebSocket
    XrWebSocket *ws = xr_ws_new(&config);
    xr_free(url_copy);

    XrJson *result = xr_json_new(xr_current_coro(X), 4);

    if (!ws) {
        xr_json_set(X, result, ctx->sym_wsid, xr_int(-1));
        xr_json_set(X, result, ctx->sym_error, xrs_string_value_c(X, "Failed to create WebSocket"));
        xr_json_set(X, result, ctx->sym_state, xrs_string_value_c(X, "closed"));
        return xr_json_value(result);
    }

    // Bind the scheduler isolate so xr_ws_connect / send / recv cooperate
    // with netpoll instead of blocking the worker on poll(5000).
    xr_ws_set_isolate(ws, X);

    // Connect
    XrWsError err = xr_ws_connect(ws);

    if (err != WS_OK) {
        xr_json_set(X, result, ctx->sym_wsid, xr_int(-1));
        xr_json_set(X, result, ctx->sym_error, xrs_string_value_c(X, xr_ws_error_string(err)));
        xr_json_set(X, result, ctx->sym_state, xrs_string_value_c(X, "closed"));
        xr_ws_free(ws);
        return xr_json_value(result);
    }

    // Store connection (no limit)
    int id = store_ws(X, ws);

    xr_json_set(X, result, ctx->sym_wsid, xr_int(id));
    xr_json_set(X, result, ctx->sym_url, ws_make_string(X, url, url_len));
    xr_json_set(X, result, ctx->sym_state, xrs_string_value_c(X, "open"));

    return xr_json_value(result);
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
static XrCFuncResult ws_send_step(XrayIsolate *X, WsSendState *state, XrValue *result);

// Continuation for send
static XrCFuncResult ws_send_continue(XrayIsolate *X, int status, void *ctx, XrValue *result) {
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
static XrCFuncResult ws_send_step(XrayIsolate *X, WsSendState *state, XrValue *result) {
    XrWsContext *ctx = get_ws_context(X);
    XrWebSocket *ws = get_ws_from_ctx(ctx, state->ws_id);
    if (!ws || xr_ws_get_state(ws) != WS_STATE_OPEN) {
        if (state->data_owned && state->data)
            xr_free(state->data);
        xr_free(state);
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    XrWsOpcode opcode = state->binary ? WS_OPCODE_BINARY : WS_OPCODE_TEXT;
    int ret = xr_ws_send_frame_try(ws, opcode, state->data, state->len);

    if (ret == 0) {
        if (ctx) {
            ctx->total_msgs_sent++;
            ctx->total_bytes_sent += state->len;
        }
        if (state->data_owned && state->data)
            xr_free(state->data);
        xr_free(state);
        *result = xr_bool(true);
        return XR_CFUNC_DONE;
    }

    if (ret == -2) {
        // Would block - yield and wait for write
        return xr_yield_for_io(X, ws->fd, XR_WAIT_WRITE, 5000, ws_send_continue, state, result);
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
static XrCFuncResult ws_send_yieldable(XrayIsolate *X, XrValue *args, int argc, XrValue *result) {
    if (argc < 2) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    // Get connection
    if (!XR_IS_JSON(args[0])) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    XrWsContext *ctx = get_ws_context(X);
    if (!ctx) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    XrJson *conn = (XrJson *) XR_TO_PTR(args[0]);
    XrValue id_val = xr_json_get(X, conn, ctx->sym_wsid);

    if (!XR_IS_INT(id_val)) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    int id = (int) XR_TO_INT(id_val);
    XrWebSocket *ws = get_ws_from_ctx(ctx, id);

    if (!ws || xr_ws_get_state(ws) != WS_STATE_OPEN) {
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
    int ret = xr_ws_send_frame_try(ws, opcode, msg, msg_len);

    if (ret == 0) {
        ctx->total_msgs_sent++;
        ctx->total_bytes_sent += msg_len;
#if WS_PROFILE
        ws_prof_send_fast++;
#endif
        *result = xr_bool(true);
        return XR_CFUNC_DONE;
    }

    if (ret == -1) {
        // Error
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

#if WS_PROFILE
    ws_prof_send_slow++;
#endif

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

// Yieldable C function pointer type
typedef XrCFuncResult (*XrYieldableCFunctionPtr)(XrayIsolate *, XrValue *, int, XrValue *);

// State for yieldable recv operation
// Following the same pattern as NetReadState in net.c
// IMPORTANT: Do NOT store XrValue across yield points - GC may invalidate them!
typedef struct WsRecvState {
    int ws_id;           // WebSocket connection ID (primitive, GC-safe)
    int64_t timeout_ms;  // Timeout in milliseconds (-1 = infinite)
} WsRecvState;

// Helper to create result JSON from message
// NOTE: No XrValue parameters - all values must be reconstructed to be GC-safe
static XrValue make_recv_result(XrayIsolate *X, XrWsContext *ctx, XrWebSocket *ws,
                                XrWsMessage *msg) {
    XrCoroutine *coro = xr_current_coro(X);

    if (!msg) {
        // P5: Error result with pre-cached Shape (fast-mode, 1 field)
        XrJson *result = (ctx && ctx->shape_recv_err)
                             ? xr_json_new_with_shape(coro, ctx->shape_recv_err)
                             : xr_json_new(coro, 4);
        if (!result)
            return xr_null();

        const char *err_msg =
            (!ws || xr_ws_get_state(ws) != WS_STATE_OPEN) ? "Connection closed" : "Receive failed";
        if (ctx && ctx->shape_recv_err)
            xr_json_set_field(result, 0, xrs_string_value_c(X, err_msg));
        else
            xr_json_set(X, result, ctx ? ctx->sym_error : 0, xrs_string_value_c(X, err_msg));
        return xr_json_value(result);
    }

    // P5: Success result with pre-cached Shape (fast-mode, 2 fields: data, binary)
    XrJson *result = (ctx && ctx->shape_recv_ok) ? xr_json_new_with_shape(coro, ctx->shape_recv_ok)
                                                 : xr_json_new(coro, 4);
    if (!result) {
        xr_ws_message_free(msg);
        return xr_null();
    }

    XrValue data_val;
    if (msg->is_text) {
        data_val = ws_make_string(X, msg->data, msg->len);
    } else {
        XrArray *bytes_arr = xr_array_with_capacity_typed(coro, (int) msg->len, XR_ELEM_U8);
        if (bytes_arr && msg->len > 0) {
            memcpy(bytes_arr->data, msg->data, msg->len);
            bytes_arr->length = (int32_t) msg->len;
        }
        data_val = bytes_arr ? xr_value_from_array(bytes_arr) : xr_null();
    }

    if (ctx && ctx->shape_recv_ok) {
        xr_json_set_field(result, 0, data_val);                // fields[0] = data
        xr_json_set_field(result, 1, xr_bool(!msg->is_text));  // fields[1] = binary
    } else {
        xr_json_set(X, result, ctx ? ctx->sym_data : 0, data_val);
        xr_json_set(X, result, ctx ? ctx->sym_binary : 0, xr_bool(!msg->is_text));
    }
    xr_ws_message_free(msg);

    return xr_json_value(result);
}

// Forward declaration
static XrCFuncResult ws_recv_step(XrayIsolate *X, WsRecvState *state, XrValue *result);

// Continuation function for ws.recv (matches XrContinuation signature)
static XrCFuncResult ws_recv_continue(XrayIsolate *X, int status, void *cont_ctx, XrValue *result) {
    WsRecvState *state = (WsRecvState *) cont_ctx;

    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        XrWsContext *ctx = get_ws_context(X);
        XrJson *res = xr_json_new(xr_current_coro(X), 4);
        xr_json_set(X, res, ctx ? ctx->sym_error : 0, xrs_string_value_c(X, "Timeout"));
        *result = xr_json_value(res);
        xr_free(state);
        return XR_CFUNC_DONE;
    }

    return ws_recv_step(X, state, result);
}

// Single step of recv operation (following net_read_step pattern)
static XrCFuncResult ws_recv_step(XrayIsolate *X, WsRecvState *state, XrValue *result) {
    XrWsContext *ctx = get_ws_context(X);
    XrWebSocket *ws = get_ws_from_ctx(ctx, state->ws_id);
    if (!ws) {
        *result = make_recv_result(X, ctx, NULL, NULL);
        xr_free(state);
        return XR_CFUNC_DONE;
    }

    if (xr_ws_get_state(ws) != WS_STATE_OPEN) {
        *result = make_recv_result(X, ctx, ws, NULL);
        xr_free(state);
        return XR_CFUNC_DONE;
    }

    bool need_more = false;
    XrWsMessage *msg = xr_ws_recv_try(ws, &need_more);

    if (msg) {
        if (ctx) {
            ctx->total_msgs_recv++;
            ctx->total_bytes_recv += msg->len;
        }
        *result = make_recv_result(X, ctx, ws, msg);
        xr_free(state);
        return XR_CFUNC_DONE;
    }

    if (!need_more) {
        *result = make_recv_result(X, ctx, ws, NULL);
        xr_free(state);
        return XR_CFUNC_DONE;
    }

    return xr_yield_for_io(X, ws->fd, XR_WAIT_READ, state->timeout_ms, ws_recv_continue, state,
                           result);
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
static XrCFuncResult ws_recv_yieldable(XrayIsolate *X, XrValue *args, int argc, XrValue *result) {
    if (argc < 1 || !XR_IS_JSON(args[0])) {
        *result = xr_null();
        return XR_CFUNC_ERROR;
    }

    XrWsContext *ctx = get_ws_context(X);
    if (!ctx) {
        *result = xr_null();
        return XR_CFUNC_ERROR;
    }

    XrJson *conn = (XrJson *) XR_TO_PTR(args[0]);
    XrValue id_val = xr_json_get(X, conn, ctx->sym_wsid);

    if (!XR_IS_INT(id_val)) {
        XrJson *res = xr_json_new(xr_current_coro(X), 4);
        xr_json_set(X, res, ctx->sym_error, xrs_string_value_c(X, "Invalid connection object"));
        *result = xr_json_value(res);
        return XR_CFUNC_DONE;
    }

    int id = (int) XR_TO_INT(id_val);
    XrWebSocket *ws = get_ws_from_ctx(ctx, id);

    if (!ws) {
        XrJson *res = xr_json_new(xr_current_coro(X), 4);
        xr_json_set(X, res, ctx->sym_error, xrs_string_value_c(X, "Connection not found"));
        *result = xr_json_value(res);
        return XR_CFUNC_DONE;
    }

    // Fast path: try recv without allocating state or yielding
    if (xr_ws_get_state(ws) == WS_STATE_OPEN) {
        bool need_more = false;
        XrWsMessage *msg = xr_ws_recv_try(ws, &need_more);

        if (msg) {
            ctx->total_msgs_recv++;
            ctx->total_bytes_recv += msg->len;
#if WS_PROFILE
            uint64_t t0 = ws_now_ns();
#endif
            *result = make_recv_result(X, ctx, ws, msg);
#if WS_PROFILE
            ws_prof_alloc_ns += ws_now_ns() - t0;
            ws_prof_recv_fast++;
#endif
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

#if WS_PROFILE
    ws_prof_recv_slow++;
#endif
    int64_t timeout_ms = -1;
    if (argc >= 2 && XR_IS_INT(args[1])) {
        timeout_ms = XR_TO_INT(args[1]);
    }

    // Slow path: no data available, allocate state and yield to kqueue
    WsRecvState *state = (WsRecvState *) xr_malloc(sizeof(WsRecvState));
    if (!state) {
        XrJson *res = xr_json_new(xr_current_coro(X), 4);
        xr_json_set(X, res, ctx->sym_error, xrs_string_value_c(X, "Out of memory"));
        *result = xr_json_value(res);
        return XR_CFUNC_ERROR;
    }

    state->ws_id = id;
    state->timeout_ms = timeout_ms;

    return xr_yield_for_io(X, ws->fd, XR_WAIT_READ, state->timeout_ms, ws_recv_continue, state,
                           result);
}

/*
 * ws.recvData continuation: returns string directly (no Json wrapper).
 */
static XrCFuncResult ws_recvdata_continue(XrayIsolate *X, int status, void *cont_ctx,
                                          XrValue *result) {
    WsRecvState *state = (WsRecvState *) cont_ctx;

    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED || status == XR_RESUME_ERROR) {
        xr_free(state);
        *result = xr_null();
        return XR_CFUNC_DONE;
    }

    XrWsContext *ctx = get_ws_context(X);
    if (!ctx) {
        xr_free(state);
        *result = xr_null();
        return XR_CFUNC_DONE;
    }

    XrWebSocket *ws = get_ws_from_ctx(ctx, state->ws_id);
    if (!ws || xr_ws_get_state(ws) != WS_STATE_OPEN) {
        xr_free(state);
        *result = xr_null();
        return XR_CFUNC_DONE;
    }

    bool need_more = false;
    XrWsMessage *msg = xr_ws_recv_try(ws, &need_more);
    if (msg) {
        ctx->total_msgs_recv++;
        ctx->total_bytes_recv += msg->len;
        *result = ws_make_string(X, msg->data, msg->len);
        xr_ws_message_free(msg);
        xr_free(state);
        return XR_CFUNC_DONE;
    }
    if (!need_more) {
        xr_free(state);
        *result = xr_null();
        return XR_CFUNC_DONE;
    }

    // Still need more data — re-yield
    return xr_yield_for_io(X, ws->fd, XR_WAIT_READ, state->timeout_ms, ws_recvdata_continue, state,
                           result);
}

/*
 * ws.recvData(conn, timeout?) -> string?
 *
 * High-performance recv: returns message data as string directly.
 * No Json wrapper allocation — eliminates the biggest per-message overhead.
 * Returns null on close/error/timeout.
 */
static XrCFuncResult ws_recvdata(XrayIsolate *X, XrValue *args, int argc, XrValue *result) {
    if (argc < 1 || !XR_IS_JSON(args[0])) {
        *result = xr_null();
        return XR_CFUNC_DONE;
    }

    XrWsContext *ctx = get_ws_context(X);
    if (!ctx) {
        *result = xr_null();
        return XR_CFUNC_DONE;
    }

    XrJson *conn = (XrJson *) XR_TO_PTR(args[0]);
    XrValue id_val = xr_json_get(X, conn, ctx->sym_wsid);
    if (!XR_IS_INT(id_val)) {
        *result = xr_null();
        return XR_CFUNC_DONE;
    }

    int id = (int) XR_TO_INT(id_val);
    XrWebSocket *ws = get_ws_from_ctx(ctx, id);
    if (!ws || xr_ws_get_state(ws) != WS_STATE_OPEN) {
        *result = xr_null();
        return XR_CFUNC_DONE;
    }

    // Fast path: data already in kernel buffer
    bool need_more = false;
    XrWsMessage *msg = xr_ws_recv_try(ws, &need_more);
    if (msg) {
        ctx->total_msgs_recv++;
        ctx->total_bytes_recv += msg->len;
        *result = ws_make_string(X, msg->data, msg->len);
        xr_ws_message_free(msg);
        return XR_CFUNC_DONE;
    }
    if (!need_more) {
        *result = xr_null();
        return XR_CFUNC_DONE;
    }

    // Slow path: yield via continuation protocol
    int64_t timeout_ms = -1;
    if (argc >= 2 && XR_IS_INT(args[1])) {
        timeout_ms = XR_TO_INT(args[1]);
    }

    WsRecvState *state = (WsRecvState *) xr_malloc(sizeof(WsRecvState));
    if (!state) {
        *result = xr_null();
        return XR_CFUNC_ERROR;
    }
    state->ws_id = id;
    state->timeout_ms = timeout_ms;

    return xr_yield_for_io(X, ws->fd, XR_WAIT_READ, timeout_ms, ws_recvdata_continue, state,
                           result);
}

/*
 * ws.close(conn: Json, code?: int, reason?: string) -> bool
 *
 * Close WebSocket connection.
 */
static XrValue ws_close(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_bool(false);

    XrWsContext *ctx = get_ws_context(X);
    if (!ctx)
        return xr_bool(false);

    if (!XR_IS_JSON(args[0]))
        return xr_bool(false);

    XrJson *conn = (XrJson *) XR_TO_PTR(args[0]);
    XrValue id_val = xr_json_get(X, conn, ctx->sym_wsid);

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
    // NOTE: xr_ws_recv_try may have already set state to CLOSED but leaves fd open
    // for us to clean up here.
    if (ws->fd >= 0) {
        XrRuntime *runtime = X ? (XrRuntime *) X->vm.runtime : NULL;
        if (runtime) {
            XrPollDesc *pd = xr_fdmap_get(&runtime->netpoll, ws->fd);
            if (pd) {
                xr_netpoll_close(&runtime->netpoll, pd);
            }
        }
    }

    // xr_ws_close sends close frame if state is OPEN, otherwise no-op
    xr_ws_close(ws, code, reason);
    xr_ws_free(ws);
    remove_ws(ctx, id);

    xr_json_set(X, conn, ctx->sym_state, xrs_string_value_c(X, "closed"));
    xr_json_set(X, conn, ctx->sym_wsid, xr_int(-1));

    return xr_bool(true);
}

/*
 * ws.ping(conn: Json) -> bool
 *
 * Send ping frame.
 */
static XrValue ws_ping(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_JSON(args[0])) {
        return xr_bool(false);
    }

    XrWsContext *ctx = get_ws_context(X);
    if (!ctx)
        return xr_bool(false);

    XrJson *conn = (XrJson *) XR_TO_PTR(args[0]);
    XrValue id_val = xr_json_get(X, conn, ctx->sym_wsid);

    if (!XR_IS_INT(id_val))
        return xr_bool(false);

    XrWebSocket *ws = get_ws_from_ctx(ctx, (int) XR_TO_INT(id_val));
    if (!ws)
        return xr_bool(false);

    return xr_bool(xr_ws_ping(ws) == WS_OK);
}

/*
 * ws.state(conn: Json) -> string
 *
 * Get connection state: "connecting", "open", "closing", "closed"
 */
static XrValue ws_state(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_JSON(args[0])) {
        return xrs_string_value_c(X, "closed");
    }

    XrWsContext *ctx = get_ws_context(X);
    if (!ctx)
        return xrs_string_value_c(X, "closed");

    XrJson *conn = (XrJson *) XR_TO_PTR(args[0]);
    XrValue id_val = xr_json_get(X, conn, ctx->sym_wsid);

    if (!XR_IS_INT(id_val))
        return xrs_string_value_c(X, "closed");

    XrWebSocket *ws = get_ws_from_ctx(ctx, (int) XR_TO_INT(id_val));
    if (!ws)
        return xrs_string_value_c(X, "closed");

    switch (xr_ws_get_state(ws)) {
        case WS_STATE_CONNECTING:
            return xrs_string_value_c(X, "connecting");
        case WS_STATE_OPEN:
            return xrs_string_value_c(X, "open");
        case WS_STATE_CLOSING:
            return xrs_string_value_c(X, "closing");
        case WS_STATE_CLOSED:
            return xrs_string_value_c(X, "closed");
        default:
            return xrs_string_value_c(X, "unknown");
    }
}

/*
 * ws.isOpen(conn: Json) -> bool
 *
 * Check if connection is open.
 */
static XrValue ws_is_open(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_JSON(args[0])) {
        return xr_bool(false);
    }

    XrWsContext *ctx = get_ws_context(X);
    if (!ctx)
        return xr_bool(false);

    XrJson *conn = (XrJson *) XR_TO_PTR(args[0]);
    XrValue id_val = xr_json_get(X, conn, ctx->sym_wsid);

    if (!XR_IS_INT(id_val))
        return xr_bool(false);

    XrWebSocket *ws = get_ws_from_ctx(ctx, (int) XR_TO_INT(id_val));
    if (!ws)
        return xr_bool(false);

    return xr_bool(xr_ws_get_state(ws) == WS_STATE_OPEN);
}

/* ========== WebSocket Server API Implementation ========== */

/*
 * Create server connection object from upgraded WebSocket
 * Used by ws.serve() when a client connects
 */
static XrValue ws_wrap_server_conn(XrayIsolate *X, XrWebSocket *ws) {
    if (!ws)
        return xr_null();

    XrWsContext *ctx = get_ws_context(X);
    if (!ctx)
        return xr_null();

    int id = store_ws(X, ws);

    XrJson *result = xr_json_new(xr_current_coro(X), 4);
    xr_json_set(X, result, ctx->sym_wsid, xr_int(id));
    xr_json_set(X, result, ctx->sym_state, xrs_string_value_c(X, "open"));
    xr_json_set(X, result, ctx->sym_is_svr, xr_bool(true));

    return xr_json_value(result);
}

/* ========== WebSocket Server (Stackless) ========== */

#define WS_UPGRADE_BUF_SIZE 4096
#define WS_HTTP_BACKLOG 1024

extern void xr_coro_spawn(XrayIsolate *X, XrCoroutine *coro);
extern XrCoroutine *xr_coro_create_cfunc(XrayIsolate *X,
                                         XrCFuncResult (*cfunc)(XrayIsolate *, XrValue *, int,
                                                                XrValue *),
                                         XrValue *args, int argc, const char *name);

/* ========== Pure C Echo Server (ws.echoServe) — Stackless ========== */

// Forward declarations for echo continuations
static XrCFuncResult ws_echo_conn_upgrade_cont(XrayIsolate *X, int status, void *ctx,
                                               XrValue *result);
static XrCFuncResult ws_echo_conn_loop(XrayIsolate *X, int status, void *ctx, XrValue *result);
static XrCFuncResult ws_echo_listen_cont(XrayIsolate *X, int status, void *ctx, XrValue *result);

/*
 * Context for echo connection — persists across yields.
 * Replaces stack-local variables from the old stackful handler.
 */
typedef struct {
    XrayIsolate *X;
    int fd;
    XrWebSocket *ws;
    XrWsContext *ws_ctx;
    XrRuntime *runtime;
    char *upgrade_buf;
    int upgrade_buf_used;
} WsEchoConnCtx;

/*
 * Echo connection entry — cfunc coroutine entry point.
 *
 * Stackless replacement for ws_echo_conn_stackful.
 * Zero VM dispatch, zero GC allocation in hot path.
 * args[0] = client fd (int).
 */
static XrCFuncResult ws_echo_conn_init(XrayIsolate *X, XrValue *args, int argc, XrValue *result) {
    (void) result;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return XR_CFUNC_DONE;

    int fd = (int) XR_TO_INT(args[0]);

    WsEchoConnCtx *ctx = (WsEchoConnCtx *) xr_calloc(1, sizeof(WsEchoConnCtx));
    if (!ctx) {
        xr_closesocket(fd);
        return XR_CFUNC_DONE;
    }

    ctx->X = X;
    ctx->fd = fd;
    ctx->runtime = (XrRuntime *) X->vm.runtime;
    ctx->upgrade_buf = (char *) xr_malloc(WS_UPGRADE_BUF_SIZE);
    if (!ctx->upgrade_buf) {
        xr_closesocket(fd);
        xr_free(ctx);
        return XR_CFUNC_DONE;
    }
    ctx->upgrade_buf_used = 0;

    // Start reading upgrade request
    return ws_echo_conn_upgrade_cont(X, XR_RESUME_IO_READY, ctx, result);
}

/*
 * Continuation: read HTTP upgrade headers, then upgrade to WebSocket.
 * On EAGAIN, yields for read and re-enters this function.
 */
static XrCFuncResult ws_echo_conn_upgrade_cont(XrayIsolate *X, int status, void *user_ctx,
                                               XrValue *result) {
    WsEchoConnCtx *ctx = (WsEchoConnCtx *) user_ctx;
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
        return xr_yield_for_io(X, ctx->fd, XR_WAIT_READ, 5000, ws_echo_conn_upgrade_cont, ctx,
                               result);
    }

    if (!xr_ws_is_upgrade_request(ctx->upgrade_buf)) {
        const char *r400 = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n"
                           "Connection: close\r\n\r\n";
        write(ctx->fd, r400, strlen(r400));
        goto fail;
    }

    // Upgrade to WebSocket
    ctx->ws = xr_ws_upgrade(X, ctx->fd, ctx->upgrade_buf);
    xr_free(ctx->upgrade_buf);
    ctx->upgrade_buf = NULL;
    if (!ctx->ws)
        goto cleanup;

    // Disable auto-ping for echo: eliminates clock_gettime per recv
    ctx->ws->config.ping_interval_ms = 0;

    // Cache PollDesc to avoid fdmap lookup per yield
    if (ctx->runtime) {
        ctx->ws->cached_pd = xr_netpoll_open(&ctx->runtime->netpoll, ctx->fd);
    }

    ctx->ws_ctx = get_ws_context(X);

    // Enter echo loop
    return ws_echo_conn_loop(X, XR_RESUME_IO_READY, ctx, result);

fail:
    xr_free(ctx->upgrade_buf);
cleanup: {
    XrRuntime *rt = (XrRuntime *) X->vm.runtime;
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
 * Echo hot loop continuation — the performance-critical path.
 *
 * Called when socket is ready for read or write.
 * Hot path per message:
 *   recv() → parse WS frame → unmask → send() WS frame
 *   No XrJson, no XrString, no bytecode, no context switch.
 */
static XrCFuncResult ws_echo_conn_loop(XrayIsolate *X, int status, void *user_ctx,
                                       XrValue *result) {
    WsEchoConnCtx *ctx = (WsEchoConnCtx *) user_ctx;
    if (status != XR_RESUME_IO_READY)
        goto cleanup;

    while (ctx->ws->state == WS_STATE_OPEN) {
        bool need_more = false;
        XrWsMessage *msg = xr_ws_recv_try(ctx->ws, &need_more);

        if (msg) {
            // Cork: buffer all echo replies, flush once
            xr_ws_cork(ctx->ws);
            int err = 0;
            bool has_ctx = (ctx->ws_ctx != NULL);

            do {
                XrWsOpcode opcode = msg->is_text ? WS_OPCODE_TEXT : WS_OPCODE_BINARY;
                if (has_ctx) {
                    ctx->ws_ctx->total_msgs_recv++;
                    ctx->ws_ctx->total_bytes_recv += msg->len;
                }
                if (xr_ws_send_frame_try(ctx->ws, opcode, msg->data, msg->len) < 0) {
                    err = 1;
                }
                if (has_ctx && !err) {
                    ctx->ws_ctx->total_msgs_sent++;
                    ctx->ws_ctx->total_bytes_sent += msg->len;
                }
                xr_ws_message_recycle(ctx->ws, msg);
                if (err)
                    break;

                // Drain more complete frames from rbuf (no syscall)
                bool nm2 = false;
                msg = xr_ws_recv_try(ctx->ws, &nm2);
            } while (msg);

            // Uncork: single send for all buffered echo frames
            int ret = xr_ws_uncork(ctx->ws);
            if (ret == -2) {
                // Partial send — yield for write, resume loop
                return xr_yield_for_io(X, ctx->ws->fd, XR_WAIT_WRITE, 5000, ws_echo_conn_loop, ctx,
                                       result);
            }
            if (err || ret == -1)
                break;
            continue;
        }

        if (!need_more)
            break;

        // No data yet — yield for read
        return xr_yield_for_io(X, ctx->ws->fd, XR_WAIT_READ, -1, ws_echo_conn_loop, ctx, result);
    }

cleanup:
    if (ctx->ws) {
        if (ctx->ws->cached_pd && ctx->runtime) {
            xr_netpoll_close(&ctx->runtime->netpoll, (XrPollDesc *) ctx->ws->cached_pd);
            ctx->ws->cached_pd = NULL;
        }
        xr_ws_close(ctx->ws, WS_CLOSE_NORMAL, NULL);
        xr_ws_free(ctx->ws);
    }
    xr_free(ctx);
    return XR_CFUNC_DONE;
}

/* ========== WebSocket Server (ws.serve) — Stackless ========== */

// Forward declarations for conn handler continuations
static XrCFuncResult ws_conn_upgrade_cont(XrayIsolate *X, int status, void *ctx, XrValue *result);
static XrCFuncResult ws_conn_handler_done(XrayIsolate *X, int status, void *ctx, XrValue *result);
static XrCFuncResult ws_serve_listen_cont(XrayIsolate *X, int status, void *ctx, XrValue *result);

/*
 * Context for WS connection handler — persists across yields.
 * Replaces stack-local variables from the old stackful handler.
 */
typedef struct {
    XrayIsolate *X;
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
static XrCFuncResult ws_conn_init(XrayIsolate *X, XrValue *args, int argc, XrValue *result) {
    (void) result;
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_PTR(args[1]))
        return XR_CFUNC_DONE;

    int fd = (int) XR_TO_INT(args[0]);

    WsConnCtx *ctx = (WsConnCtx *) xr_calloc(1, sizeof(WsConnCtx));
    if (!ctx) {
        xr_closesocket(fd);
        return XR_CFUNC_DONE;
    }

    ctx->X = X;
    ctx->fd = fd;
    ctx->handler = (XrClosure *) XR_TO_PTR(args[1]);
    ctx->runtime = (XrRuntime *) X->vm.runtime;
    ctx->upgrade_buf = (char *) xr_malloc(WS_UPGRADE_BUF_SIZE);
    if (!ctx->upgrade_buf) {
        xr_closesocket(fd);
        xr_free(ctx);
        return XR_CFUNC_DONE;
    }
    ctx->upgrade_buf_used = 0;

    // Start reading upgrade request
    return ws_conn_upgrade_cont(X, XR_RESUME_IO_READY, ctx, result);
}

/*
 * Continuation: read HTTP upgrade headers, validate, upgrade to WS,
 * then call user handler closure via xr_yield_call_closure.
 */
static XrCFuncResult ws_conn_upgrade_cont(XrayIsolate *X, int status, void *user_ctx,
                                          XrValue *result) {
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

    if (!xr_ws_is_upgrade_request(ctx->upgrade_buf)) {
        const char *r400 = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n"
                           "Connection: close\r\n\r\n";
        ssize_t ret = write(ctx->fd, r400, strlen(r400));
        (void) ret;
        goto fail;
    }

    // Upgrade to WebSocket
    {
        XrWebSocket *ws = xr_ws_upgrade(X, ctx->fd, ctx->upgrade_buf);
        xr_free(ctx->upgrade_buf);
        ctx->upgrade_buf = NULL;
        if (!ws)
            goto cleanup;

        ctx->conn = ws_wrap_server_conn(X, ws);
        if (XR_IS_NULL(ctx->conn)) {
            xr_ws_close(ws, WS_CLOSE_SERVER_ERROR, NULL);
            xr_ws_free(ws);
            goto cleanup;
        }

        // Call user handler closure — resumes ws_conn_handler_done on return
        return xr_yield_call_closure(X, ctx->handler, &ctx->conn, 1, ws_conn_handler_done, ctx,
                                     result);
    }

fail:
    xr_free(ctx->upgrade_buf);
cleanup: {
    XrRuntime *rt = (XrRuntime *) X->vm.runtime;
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
static XrCFuncResult ws_conn_handler_done(XrayIsolate *X, int status, void *user_ctx,
                                          XrValue *result) {
    (void) status;
    (void) result;
    WsConnCtx *ctx = (WsConnCtx *) user_ctx;

    // Always close when handler exits (safe even if already closed)
    XrWsContext *ws_ctx = get_ws_context(X);
    if (ws_ctx && !XR_IS_NULL(ctx->conn)) {
        XrJson *conn_obj = (XrJson *) XR_TO_PTR(ctx->conn);
        XrValue id_val = xr_json_get(X, conn_obj, ws_ctx->sym_wsid);
        if (XR_IS_INT(id_val)) {
            int id = (int) XR_TO_INT(id_val);
            XrWebSocket *w = get_ws_from_ctx(ws_ctx, id);
            if (w) {
                if (w->fd >= 0 && ctx->runtime) {
                    XrPollDesc *pd = xr_fdmap_get(&ctx->runtime->netpoll, w->fd);
                    if (pd)
                        xr_netpoll_close(&ctx->runtime->netpoll, pd);
                }
                xr_ws_close(w, WS_CLOSE_NORMAL, NULL);
                xr_ws_free(w);
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
    XrayIsolate *X;
    int listen_fd;
    XrValue handler_val;  // closure XrValue (for passing to conn coroutines)
} WsServeListenCtx;

/*
 * WS serve listen entry — cfunc coroutine entry point.
 * args[0] = listen fd (int), args[1] = handler closure (ptr).
 */
static XrCFuncResult ws_serve_listen_init(XrayIsolate *X, XrValue *args, int argc,
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
static XrCFuncResult ws_serve_listen_cont(XrayIsolate *X, int status, void *user_ctx,
                                          XrValue *result) {
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
        XrCoroutine *coro = xr_coro_create_cfunc(X, ws_conn_init, conn_args, 2, "ws.conn");
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
static XrCFuncResult ws_serve_wait_cont(XrayIsolate *X, int status, void *cont_ctx,
                                        XrValue *result) {
    (void) status;
    XrWsContext *ctx = (XrWsContext *) cont_ctx;
    if (!ctx || !ctx->server_running) {
        *result = xr_bool(true);
        return XR_CFUNC_DONE;
    }
    return xr_yield_for_timeout(X, 1000, ws_serve_wait_cont, ctx, result);
}

/*
 * Echo listen context — persists across accept loop yields.
 */
typedef struct {
    XrayIsolate *X;
    int listen_fd;
} WsEchoListenCtx;

/*
 * Echo listen entry — cfunc coroutine entry point.
 * args[0] = listen fd (int).
 */
static XrCFuncResult ws_echo_listen_init(XrayIsolate *X, XrValue *args, int argc, XrValue *result) {
    (void) result;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return XR_CFUNC_DONE;

    WsEchoListenCtx *ctx = (WsEchoListenCtx *) xr_calloc(1, sizeof(WsEchoListenCtx));
    if (!ctx)
        return XR_CFUNC_DONE;

    ctx->X = X;
    ctx->listen_fd = (int) XR_TO_INT(args[0]);

    // Yield for first accept readiness
    return xr_yield_for_io(X, ctx->listen_fd, XR_WAIT_READ, -1, ws_echo_listen_cont, ctx, result);
}

/*
 * Echo accept loop continuation — accepts all pending connections,
 * spawns a cfunc coroutine per connection, then yields for next batch.
 */
static XrCFuncResult ws_echo_listen_cont(XrayIsolate *X, int status, void *user_ctx,
                                         XrValue *result) {
    WsEchoListenCtx *ctx = (WsEchoListenCtx *) user_ctx;

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

        // Spawn stackless echo connection coroutine
        XrValue conn_args[1] = {xr_int(client_fd)};
        XrCoroutine *coro = xr_coro_create_cfunc(X, ws_echo_conn_init, conn_args, 1, "ws.echo");
        if (coro) {
            xr_coro_spawn(X, coro);
        } else {
            xr_closesocket(client_fd);
        }
    }

    // Yield for next accept batch
    return xr_yield_for_io(X, ctx->listen_fd, XR_WAIT_READ, -1, ws_echo_listen_cont, ctx, result);
}

/*
 * ws.echoServe(port: int) -> bool
 *
 * Pure C echo server: zero VM dispatch, zero GC allocation per message.
 * Architecturally equivalent to HTTP's prebuilt route fast path.
 */
static XrCFuncResult ws_echo_serve_yieldable(XrayIsolate *X, XrValue *args, int argc,
                                             XrValue *result) {
    if (argc < 1 || !XR_IS_INT(args[0])) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    int port = (int) XR_TO_INT(args[0]);

    XrWsContext *ctx = get_ws_context(X);
    if (!ctx) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    if (ctx->server_running) {
        fprintf(stderr, "ws.echoServe: server already running\n");
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    int listen_fd = xr_socket_listen("0.0.0.0", port, WS_HTTP_BACKLOG);
    if (listen_fd < 0) {
        fprintf(stderr, "ws.echoServe: cannot listen on port %d\n", port);
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    ctx->listen_fd = listen_fd;
    ctx->server_running = true;

    printf("=== xray WebSocket Echo Server (pure C) ===\n");
    printf("Port: %d\n", port);
    printf("Listening...\n");

    // Spawn stackless accept loop coroutine
    XrValue listen_args[1] = {xr_int(listen_fd)};
    XrCoroutine *listen_coro =
        xr_coro_create_cfunc(X, ws_echo_listen_init, listen_args, 1, "ws.echo.listen");
    if (!listen_coro) {
        xr_closesocket(listen_fd);
        ctx->listen_fd = -1;
        ctx->server_running = false;
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }
    xr_coro_spawn(X, listen_coro);

    return xr_yield_for_timeout(X, 1000, ws_serve_wait_cont, ctx, result);
}

/*
 * ws.serve(port: int, handler: fn(conn): void) -> bool
 *
 * Start WebSocket server. Creates listen socket, spawns accept loop,
 * blocks caller until server stops.
 */
static XrCFuncResult ws_serve_yieldable(XrayIsolate *X, XrValue *args, int argc, XrValue *result) {
    if (argc < 2) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }
    if (!XR_IS_INT(args[0])) {
        *result = xr_bool(false);
        return XR_CFUNC_DONE;
    }

    int port = (int) XR_TO_INT(args[0]);

    // args[1] must be a closure
    if (!XR_IS_PTR(args[1])) {
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
        xr_coro_create_cfunc(X, ws_serve_listen_init, listen_args, 2, "ws.listen");
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
static XrValue ws_stop_server(XrayIsolate *X, XrValue *args, int argc) {
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

/*
 * ws.isServerRunning() -> bool
 */
static XrValue ws_is_server_running(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrWsContext *ctx = get_ws_context(X);
    if (!ctx)
        return xr_bool(false);
    return xr_bool(ctx->server_running);
}

/* ========== HTTP Server Integration ========== */

/*
 * Upgrade HTTP connection to WebSocket and wrap as script-visible Json.
 * Called from http_listen.c when a WS route matches an Upgrade request.
 * Performs the full handshake (101 response) and returns a conn object
 * identical to what ws._acceptWs() returns.
 */
XrValue xr_ws_upgrade_and_wrap(XrayIsolate *X, int fd, const char *request_headers) {
    XrWebSocket *ws = xr_ws_upgrade(X, fd, request_headers);
    if (!ws)
        return xr_null();
    return ws_wrap_server_conn(X, ws);
}

/* ========== Module Registration ========== */

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module ws
// @handle WsConn { const wsid: int, url: string, state: string }
// @handle WsMessage { const data: string, const binary: bool }

XR_DEFINE_BUILTIN(ws_connect, "connect", "(url: string, options?: Json): WsConn?",
                  "Connect to a WebSocket server")
XR_DEFINE_BUILTIN(ws_send_yieldable, "send", "(conn: WsConn, data: string, binary?: bool): bool",
                  "Send data over WebSocket connection")
XR_DEFINE_BUILTIN(ws_recv_yieldable, "recv", "(conn: WsConn, timeout?: int): WsMessage?",
                  "Receive data from WebSocket connection")
XR_DEFINE_BUILTIN(ws_close, "close", "(conn: WsConn, code?: int, reason?: string?): bool",
                  "Close a WebSocket connection")
XR_DEFINE_BUILTIN(ws_ping, "ping", "(conn: WsConn): bool", "Send a ping frame")
XR_DEFINE_BUILTIN(ws_state, "state", "(conn: WsConn): string", "Get connection state")
XR_DEFINE_BUILTIN(ws_is_open, "isOpen", "(conn: WsConn): bool", "Check if connection is open")
XR_DEFINE_BUILTIN(ws_recvdata, "recvData", "(conn: WsConn, timeout?: int): string?",
                  "High-performance recv returning data string directly (no Json wrapper)")
XR_DEFINE_BUILTIN(ws_echo_serve_yieldable, "echoServe", "(port: int): bool",
                  "Pure C echo server with zero VM/GC overhead per message")

XR_FUNC XrModule *xr_load_module_ws(XrayIsolate *isolate) {
    // 1. Create Native module
    XrModule *mod = xr_module_create_native(isolate, "ws");
    if (!mod)
        return NULL;

#if WS_PROFILE
    if (!ws_prof_registered) {
        signal(SIGUSR1, ws_prof_signal);
        ws_prof_registered = 1;
    }
#endif

    // WebSocket client functions (directly exported, no script wrapper needed)
    XRS_EXPORT_SLOW(mod, isolate, "connect", ws_connect);
    XRS_EXPORT_YIELDABLE(mod, isolate, "send", ws_send_yieldable);
    XRS_EXPORT_YIELDABLE(mod, isolate, "recv", ws_recv_yieldable);
    XRS_EXPORT(mod, isolate, "close", ws_close);
    XRS_EXPORT(mod, isolate, "ping", ws_ping);
    XRS_EXPORT(mod, isolate, "state", ws_state);
    XRS_EXPORT(mod, isolate, "isOpen", ws_is_open);

    // High-performance variants (recvData returns string directly, no Json wrapper)
    XRS_EXPORT_YIELDABLE(mod, isolate, "recvData", ws_recvdata);
    XRS_EXPORT_YIELDABLE(mod, isolate, "sendData", ws_send_yieldable);

    // WebSocket server (pure C, no script layer needed)
    XRS_EXPORT_YIELDABLE(mod, isolate, "serve", ws_serve_yieldable);
    XRS_EXPORT_YIELDABLE(mod, isolate, "echoServe", ws_echo_serve_yieldable);
    XRS_EXPORT(mod, isolate, "stopServer", ws_stop_server);
    XRS_EXPORT(mod, isolate, "isServerRunning", ws_is_server_running);

    return mod;
}
