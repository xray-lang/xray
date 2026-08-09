/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * net.c - Raw network primitives behind the net.xr semantic layer
 *
 * KEY CONCEPT:
 *   This file holds only the yieldable socket/TLS/netpoll primitives and the
 *   typed handle plumbing. Everything with a policy dimension — address
 *   resolution order, dial fallback, timeout defaults, buffer sizing, error
 *   classification into NetError, copy pumps, datagram shaping — lives in
 *   stdlib/net/net.xr so VM and AOT execute the same Xray semantics.
 *
 *   Primitives never construct NetError values. They record a portable
 *   XrNetErrorKind code plus the raw errno on the handle (net.__lastCode /
 *   net.__lastErrno) or return the code directly when no handle exists yet
 *   (net.__connectFd, net.__tlsHandshake); the script layer classifies.
 */

#include "../../src/base/xmalloc.h"
#include "../common.h"
#include "net.h"
#include "io.h"
#include "tls.h"
#include "../stdlib_cache.h"
#include "../../src/io/xdns.h"
#include "../../src/io/xnet_handle.h"
#include "../../src/runtime/class/xclass.h"
#include "../../src/runtime/class/xclass_builder.h"
#include "../../src/runtime/class/xclass_system.h"
#include "../../src/runtime/class/xinstance.h"
#include "../../src/runtime/class/xenum.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xjson.h"

#include "../../src/module/xmodule.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/coro/xworker.h"
#include "../../src/coro/xnetpoll.h"
#include "../../src/vm/xvm.h"
#include "../../src/vm/xvm_coro_api.h"
#include "../../src/runtime/symbol/xsymbol_table.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/os/os_time.h"

// Import types and functions from xsocket.h (avoid header conflicts)
typedef struct {
    bool ready;
    int value;
    int error;
} XrIOTryResult;

extern XrIOTryResult xr_socket_accept_try(struct XrVMRuntime *X, int listen_fd);
extern XrIOTryResult xr_socket_read_try(struct XrVMRuntime *X, int fd, char *buf, int maxlen);
extern XrIOTryResult xr_socket_write_try(struct XrVMRuntime *X, int fd, const char *data,
                                         size_t len);
extern void xr_socket_close(struct XrVMRuntime *X, int fd);

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdint.h>
#include "../../src/os/os_net.h"
#include "../../src/os/os_thread.h"
#ifndef XR_OS_WINDOWS
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#endif

// ========== Internal Helpers ==========

#ifdef XR_OS_WINDOWS
static xr_once_t g_net_winsock_once = XR_ONCE_INITIALIZER;

static void net_winsock_init_once(void) {
    /* Winsock is process-scoped. Keep the module's reference until process
     * teardown so top-level `xray run` has the same network availability as
     * scheduler-backed `xray test`; netpoll may own additional balanced
     * references for its own lifetime. */
    (void) xr_winsock_init();
}

static void net_platform_init(void) {
    xr_once_call(&g_net_winsock_once, net_winsock_init_once);
}
#else
static void net_platform_init(void) {
}
#endif

/*
 * Close a socket fd with proper netpoll cleanup.
 * Handles: netpoll deregistration, shutdown, close.
 * Safe to call with fd < 0 or NULL isolate.
 */
static void net_close_fd(XrVMRuntime *X, int fd) {
    if (fd < 0)
        return;
    XrRuntime *runtime = X ? (XrRuntime *) X->vm.scheduler : NULL;
    if (runtime) {
        XrPollDesc *pd = xr_fdmap_get(&runtime->netpoll, fd);
        if (pd && !atomic_load(&pd->closing))
            xr_netpoll_close(&runtime->netpoll, pd);
    }
    shutdown(fd, XR_SHUT_WR);
    xr_closesocket(fd);
}

// ========== TLS client context ==========

#ifdef XR_ENABLE_TLS
static XrTlsContext *g_tls_client_ctx = NULL;
static xr_once_t g_tls_client_once = XR_ONCE_INITIALIZER;

static void tls_client_ctx_init(void) {
    g_tls_client_ctx = xr_tls_context_new_client();
}

static XrTlsContext *get_tls_client_ctx(void) {
    xr_once_call(&g_tls_client_once, tls_client_ctx_init);
    return g_tls_client_ctx;
}
#endif

// ========== Typed Handle Helpers ==========

/*
 * Handle type checks. Validates GC type is XR_TINSTANCE and the
 * class has the expected builtin_kind.
 */
static inline bool is_conn_handle(XrValue v) {
    if (!XR_IS_PTR(v) || XR_HEAP_TYPE(v) != XR_TINSTANCE)
        return false;
    XrInstance *inst = (XrInstance *) XR_VALUE_GCPTR(v);
    return inst->klass && inst->klass->builtin_kind == XR_BK_NETCONN;
}

static inline bool is_listener_handle(XrValue v) {
    if (!XR_IS_PTR(v) || XR_HEAP_TYPE(v) != XR_TINSTANCE)
        return false;
    XrInstance *inst = (XrInstance *) XR_VALUE_GCPTR(v);
    return inst->klass && inst->klass->builtin_kind == XR_BK_NETLISTENER;
}

static inline XrNetConn *unwrap_conn(XrValue v) {
    return is_conn_handle(v) ? (XrNetConn *) XR_VALUE_GCPTR(v) : NULL;
}

static inline XrNetListener *unwrap_listener(XrValue v) {
    return is_listener_handle(v) ? (XrNetListener *) XR_VALUE_GCPTR(v) : NULL;
}

static int64_t net_now_ms(void) {
    return (int64_t) (xr_time_monotonic_ns() / 1000000ULL);
}

static int64_t net_timeout_until(int64_t deadline_ms) {
    if (deadline_ms <= 0)
        return -1;
    int64_t remaining = deadline_ms - net_now_ms();
    return remaining <= 0 ? 1 : remaining;
}

/* Relative timeout parameter to absolute deadline; <0 means no deadline. */
static int64_t net_deadline_from_timeout(int64_t timeout_ms) {
    return timeout_ms < 0 ? 0 : net_now_ms() + timeout_ms;
}

static void net_conn_clear_error(XrNetConn *c) {
    if (!c)
        return;
    c->last_error = XR_NETERR_NONE;
    c->last_errno = 0;
}

static void net_listener_clear_error(XrNetListener *l) {
    if (!l)
        return;
    l->last_error = XR_NETERR_NONE;
    l->last_errno = 0;
}

/*
 * errno -> portable code normalization. This is the portability shim that
 * keeps OS-specific errno numbering out of the script layer; the semantic
 * mapping from codes to NetError variants lives in net.xr.
 */
static uint8_t net_error_from_errno(int err) {
    if (err == XR_ETIMEDOUT)
        return XR_NETERR_TIMEOUT;
    if (err == XR_ECONNRESET || err == XR_EPIPE)
        return XR_NETERR_RESET;
    if (err == XR_ECONNREFUSED)
        return XR_NETERR_REFUSED;
    if (err == XR_EBADF || err == XR_ENOTCONN)
        return XR_NETERR_CLOSED;
    return XR_NETERR_IO;
}

static uint8_t net_code_from_resume(int status) {
    return status == XR_RESUME_TIMEOUT ? XR_NETERR_TIMEOUT : XR_NETERR_CANCELLED;
}

static void net_conn_set_error(XrNetConn *c, uint8_t kind, int err) {
    if (!c)
        return;
    c->last_error = kind;
    c->last_errno = err;
}

static void net_listener_set_error(XrNetListener *l, uint8_t kind, int err) {
    if (!l)
        return;
    l->last_error = kind;
    l->last_errno = err;
}

static XrValue make_conn_handle(XrVMRuntime *X, int fd) {
    XrNetConn *c = xr_net_conn_new(X, fd, XR_NETCONN_TCP);
    if (!c)
        return XR_NULL_VAL;
    return XR_FROM_PTR(c);
}

static XrValue make_listener_handle(XrVMRuntime *X, int fd, int port_num) {
    XrNetListener *l = xr_net_listener_new(X, fd, port_num);
    if (!l)
        return XR_NULL_VAL;
    return XR_FROM_PTR(l);
}

static XrValue make_udp_handle(XrVMRuntime *X, int fd) {
    XrNetConn *c = xr_net_conn_new(X, fd, XR_NETCONN_UDP);
    if (!c)
        return XR_NULL_VAL;
    return XR_FROM_PTR(c);
}

/*
 * fd/tls accessors accept either an XrNetConn or an XrNetListener
 * because net.fd / net.close treat them uniformly. Returns -1 when
 * the handle is unknown or already closed.
 */
static int handle_get_fd(XrVMRuntime *X, XrValue handle) {
    (void) X;
    XrNetConn *c = unwrap_conn(handle);
    if (c)
        return c->closed ? -1 : c->fd;
    XrNetListener *l = unwrap_listener(handle);
    if (l)
        return l->closed ? -1 : l->fd;
    return -1;
}

static inline XrArray *net_as_bytes(XrValue v) {
    if (!XR_IS_ARRAY(v))
        return NULL;
    XrArray *arr = XR_TO_ARRAY(v);
    return (arr && arr->elem_type == XR_ELEM_U8) ? arr : NULL;
}

static inline XrArray *net_as_writable_bytes(XrValue v) {
    XrArray *arr = net_as_bytes(v);
    if (!arr || xr_array_is_slice(arr) || arr->capacity <= 0 || !arr->data)
        return NULL;
    return arr;
}

// ========== Yieldable net.__resolveAll ==========

/*
 * A numeric literal needs no resolver round-trip; net.xr calls __resolveAll
 * on every dial, so the literal case must stay allocation-plus-parse only.
 */
static bool net_is_literal_ip(const char *host) {
    struct in_addr v4;
    struct in6_addr v6;
    return inet_pton(AF_INET, host, &v4) == 1 || inet_pton(AF_INET6, host, &v6) == 1;
}

static XrValue net_resolved_addrs_value(XrVMRuntime *X, const char *host) {
    XrSockAddr resolved[XR_DNS_MAX_ADDRS];
    int count = xr_dns_resolve_all(X, host, resolved, XR_DNS_MAX_ADDRS, XR_AF_UNSPEC);
    XrArray *arr = xr_array_new(xr_current_coro(X));
    if (!arr)
        return XR_NULL_VAL;
    for (int i = 0; i < count; i++) {
        char text[INET6_ADDRSTRLEN];
        const char *formatted = NULL;
        if (resolved[i].family == AF_INET)
            formatted = inet_ntop(AF_INET, &resolved[i].addr.v4.sin_addr, text, sizeof(text));
        else
            formatted = inet_ntop(AF_INET6, &resolved[i].addr.v6.sin6_addr, text, sizeof(text));
        if (!formatted)
            continue;
        XrString *s = xr_string_intern(X, text, strlen(text), 0);
        if (s)
            xr_array_push(arr, xr_string_value(s));
    }
    return xr_value_from_array(arr);
}

typedef struct {
    char host[256];
} NetResolveState;

static XrCFuncResult net_resolve_all_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                              void *ctx, XrValue *result) {
    (void) resume_value;
    NetResolveState *state = (NetResolveState *) ctx;
    if (status == XR_RESUME_CANCELLED) {
        xr_free(state);
        XrArray *arr = xr_array_new(xr_current_coro(X));
        if (!arr) {
            *result = XR_NULL_VAL;
            return XR_CFUNC_ERROR;
        }
        *result = xr_value_from_array(arr);
        return XR_CFUNC_DONE;
    }
    // The async pool has warmed the runtime DNS cache; this lookup is now a
    // cache hit (a rare LRU eviction falls back to a bounded sync resolve).
    *result = net_resolved_addrs_value(X, state->host);
    xr_free(state);
    return XR_IS_NULL(*result) ? XR_CFUNC_ERROR : XR_CFUNC_DONE;
}

/*
 * net.__resolveAll(host) -> Array<string>
 * Yieldable: cache miss resolves on the async pool so the calling coroutine
 * suspends instead of blocking its worker thread in getaddrinfo.
 */
static XrCFuncResult net_resolve_all_yieldable(XrVMRuntime *X, XrValue *args, int nargs,
                                               XrValue *result) {
    if (nargs < 1 || !XR_IS_STRING(args[0])) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }
    const char *host = XR_STRING_CHARS(XR_TO_STRING(args[0]));

    XrCoroutine *coro = xr_current_coro(X);
    XrWorker *worker = xr_current_worker();
    if (!coro || !worker || net_is_literal_ip(host)) {
        *result = net_resolved_addrs_value(X, host);
        return XR_IS_NULL(*result) ? XR_CFUNC_ERROR : XR_CFUNC_DONE;
    }

    // Probe with family zeroed: a filled family means the cache (or the
    // no-pool sync fallback) answered inline and no suspension happened.
    XrSockAddr probe;
    memset(&probe, 0, sizeof(probe));
    if (!xr_dns_resolve_async(X, coro, worker->p.id, host, &probe, XR_AF_UNSPEC) ||
        probe.family != 0) {
        *result = net_resolved_addrs_value(X, host);
        return XR_IS_NULL(*result) ? XR_CFUNC_ERROR : XR_CFUNC_DONE;
    }

    // Submitted: the pool already parked this coroutine; bind our resume.
    NetResolveState *state = (NetResolveState *) xr_calloc(1, sizeof(NetResolveState));
    if (!state) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }
    strncpy(state->host, host, sizeof(state->host) - 1);
    if (!xr_yield_set_continuation(X, net_resolve_all_continue, state)) {
        xr_free(state);
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }
    return XR_CFUNC_BLOCKED;
}

// ========== Yieldable net.__connectFd ==========

/*
 * Connect has no handle to carry an error until it succeeds, so the last
 * failure code lives in a per-thread slot that net.__lastConnectCode reads.
 * Each worker thread runs one coroutine at a time, and net.xr reads the code
 * immediately after a null connect result, so a thread-local is sufficient
 * and needs no synchronization.
 */
static XR_THREAD_LOCAL int g_last_connect_code = XR_NETERR_NONE;

typedef struct {
    int fd;
    XrPollDesc *pd;                // set on the io_uring completion path
    struct sockaddr_storage addr;  // resolved target; kept valid for the connect op
    socklen_t addrlen;
} NetConnectState;

/* Record the failure code, release the socket and state, and yield a null
 * result. The trailing dummy return lets call sites use the comma form
 * `return (net_connect_fail(...), XR_CFUNC_DONE)`. */
static void net_connect_fail(XrVMRuntime *X, NetConnectState *state, uint8_t code,
                             XrValue *result) {
    g_last_connect_code = code;
    if (state) {
        net_close_fd(X, state->fd);
        xr_free(state);
    }
    *result = XR_NULL_VAL;
}

static XrCFuncResult net_connect_fd_step(XrVMRuntime *X, NetConnectState *state, XrValue *result);

static XrCFuncResult net_connect_fd_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                             void *ctx, XrValue *result) {
    (void) resume_value;
    NetConnectState *state = (NetConnectState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        net_connect_fail(X, state, net_code_from_resume(status), result);
        return XR_CFUNC_DONE;
    }
    return net_connect_fd_step(X, state, result);
}

#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
// Completion continuation for connect: the connect op resolved to 0 (connected)
// or -errno. No SO_ERROR round-trip needed — the CQE carries the verdict.
static XrCFuncResult net_connect_fd_complete(XrVMRuntime *X, int status, XrValue resume_value,
                                             void *ctx, XrValue *result) {
    (void) status;
    (void) resume_value;
    NetConnectState *state = (NetConnectState *) ctx;
    XrUringXferKind kind;
    long res = xr_netpoll_uring_xfer_result(state->pd, XR_POLL_WRITE, &kind);
    if (kind == XR_URING_XFER_DATA && res == 0) {
        int fd = state->fd;
        xr_free(state);
        g_last_connect_code = XR_NETERR_NONE;
        *result = make_conn_handle(X, fd);
        return XR_IS_NULL(*result) ? XR_CFUNC_ERROR : XR_CFUNC_DONE;
    }
    uint8_t code = (kind == XR_URING_XFER_TIMEOUT)  ? XR_NETERR_TIMEOUT
                   : (kind == XR_URING_XFER_CLOSED) ? XR_NETERR_CLOSED
                                                    : net_error_from_errno((int) (-res));
    net_connect_fail(X, state, code, result);
    return XR_CFUNC_DONE;
}
#endif

static XrCFuncResult net_connect_fd_step(XrVMRuntime *X, NetConnectState *state, XrValue *result) {
    int error = xr_socket_get_error((xr_socket_t) state->fd);
    if (error != 0) {
        net_connect_fail(X, state, net_error_from_errno(error), result);
        return XR_CFUNC_DONE;
    }
    int fd = state->fd;
    xr_free(state);
    g_last_connect_code = XR_NETERR_NONE;
    *result = make_conn_handle(X, fd);
    return XR_IS_NULL(*result) ? XR_CFUNC_ERROR : XR_CFUNC_DONE;
}

/*
 * net.__connectFd(addrLiteral, port, timeoutMs) -> NetConn?
 * Yieldable: non-blocking connect to ONE literal address. Name resolution and
 * multi-address fallback are net.xr policy; an unresolvable input here is an
 * invalid-argument code. Null result carries its code on __lastConnectCode.
 * The result is deliberately NOT a `NetConn | int` union: a union of a builtin
 * native class with a scalar forces module-wide runtime discrimination that
 * miscompiles suspended handle results in coroutine frames.
 */
static XrCFuncResult net_connect_fd_yieldable(XrVMRuntime *X, XrValue *args, int nargs,
                                              XrValue *result) {
    if (nargs < 3 || !XR_IS_STRING(args[0]) || !XR_IS_INT(args[1]) || !XR_IS_INT(args[2]))
        return (net_connect_fail(X, NULL, XR_NETERR_INVALID, result), XR_CFUNC_DONE);
    const char *addr_text = XR_STRING_CHARS(XR_TO_STRING(args[0]));
    int port_num = (int) XR_TO_INT(args[1]);
    int64_t timeout_ms = (int64_t) XR_TO_INT(args[2]);
    if (port_num < 0 || port_num > 65535)
        return (net_connect_fail(X, NULL, XR_NETERR_INVALID, result), XR_CFUNC_DONE);

    XrSockAddr resolved;
    if (!xr_dns_resolve(X, addr_text, &resolved, XR_AF_UNSPEC) || !net_is_literal_ip(addr_text))
        return (net_connect_fail(X, NULL, XR_NETERR_INVALID, result), XR_CFUNC_DONE);

    int fd = socket(resolved.family, SOCK_STREAM, 0);
    if (fd < 0)
        return (net_connect_fail(X, NULL, net_error_from_errno(xr_get_socket_error()), result),
                XR_CFUNC_DONE);
    xr_io_set_nonblocking(fd);
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *) &opt, sizeof(opt));

    NetConnectState *state = (NetConnectState *) xr_calloc(1, sizeof(NetConnectState));
    if (!state) {
        net_close_fd(X, fd);
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }
    state->fd = fd;
    if (resolved.family == AF_INET) {
        resolved.addr.v4.sin_port = htons((uint16_t) port_num);
        memcpy(&state->addr, &resolved.addr.v4, sizeof(struct sockaddr_in));
        state->addrlen = sizeof(struct sockaddr_in);
    } else {
        resolved.addr.v6.sin6_port = htons((uint16_t) port_num);
        memcpy(&state->addr, &resolved.addr.v6, sizeof(struct sockaddr_in6));
        state->addrlen = sizeof(struct sockaddr_in6);
    }

#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
    // Completion mode: submit a connect op; its CQE is the connect verdict.
    XrRuntime *rt = (XrRuntime *) X->vm.scheduler;
    if (rt && xr_netpoll_uring_active(&rt->netpoll)) {
        XrPollDesc *pd = xr_netpoll_open(&rt->netpoll, fd);
        if (pd) {
            state->pd = pd;
            XrCFuncResult cr;
            XrUringReq req = {.kind = XR_URING_OP_CONNECT,
                              .addr = &state->addr,
                              .addrlen = state->addrlen,
                              .timeout_ms = timeout_ms};
            if (xr_yield_for_uring_io(X, pd, XR_POLL_WRITE, &req, net_connect_fd_complete, state,
                                      result, &cr))
                return cr;
            state->pd = NULL;  // completion not taken — fall through to readiness connect
        }
    }
#endif

    // Readiness connect: start connect(), then wait for writable + SO_ERROR.
    int ret = connect(fd, (struct sockaddr *) &state->addr, state->addrlen);
    if (ret == 0) {
        xr_free(state);
        g_last_connect_code = XR_NETERR_NONE;
        *result = make_conn_handle(X, fd);
        return XR_IS_NULL(*result) ? XR_CFUNC_ERROR : XR_CFUNC_DONE;
    }
    if (!xr_socket_err_is_inprogress(xr_get_socket_error())) {
        net_connect_fail(X, state, net_error_from_errno(xr_get_socket_error()), result);
        return XR_CFUNC_DONE;
    }
    return xr_yield_for_io(X, fd, XR_WAIT_WRITE, timeout_ms < 0 ? -1 : timeout_ms,
                           net_connect_fd_continue, state, result);
}

// ========== Yieldable net.__accept ==========

typedef struct {
    int listen_fd;
    XrNetListener *listener;
    XrPollDesc *pd;  // set on the io_uring completion path
} NetAcceptState;

static XrCFuncResult net_accept_step(XrVMRuntime *X, NetAcceptState *state, XrValue *result);

static XrCFuncResult net_accept_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                         void *ctx, XrValue *result) {
    (void) resume_value;
    NetAcceptState *state = (NetAcceptState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        net_listener_set_error(state->listener, net_code_from_resume(status), 0);
        xr_free(state);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    return net_accept_step(X, state, result);
}

#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
// Completion continuation for accept: the accept op's new fd is in the pd. The
// kernel returned a SOCK_NONBLOCK fd (accept4 flags); set TCP_NODELAY to match
// the readiness accept path (xr_socket_accept_try).
static XrCFuncResult net_accept_complete(XrVMRuntime *X, int status, XrValue resume_value,
                                         void *ctx, XrValue *result) {
    (void) status;
    (void) resume_value;
    NetAcceptState *state = (NetAcceptState *) ctx;
    XrUringXferKind kind;
    long fd = xr_netpoll_uring_xfer_result(state->pd, XR_POLL_READ, &kind);
    if (kind == XR_URING_XFER_DATA && fd >= 0) {
        int client_fd = (int) fd;
        int opt = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, (const char *) &opt, sizeof(opt));
        net_listener_clear_error(state->listener);
        xr_free(state);
        *result = make_conn_handle(X, client_fd);
        return XR_CFUNC_DONE;
    }
    uint8_t err = (kind == XR_URING_XFER_TIMEOUT)  ? XR_NETERR_TIMEOUT
                  : (kind == XR_URING_XFER_CLOSED) ? XR_NETERR_CLOSED
                                                   : net_error_from_errno((int) (-fd));
    net_listener_set_error(state->listener, err, (kind == XR_URING_XFER_ERROR) ? (int) (-fd) : 0);
    xr_free(state);
    *result = XR_NULL_VAL;
    return XR_CFUNC_DONE;
}
#endif

static XrCFuncResult net_accept_step(XrVMRuntime *X, NetAcceptState *state, XrValue *result) {
    XrIOTryResult r = xr_socket_accept_try(X, state->listen_fd);
    if (r.ready) {
        int client_fd = r.value;
        if (client_fd < 0) {
            net_listener_set_error(state->listener, net_error_from_errno(r.error), r.error);
            xr_free(state);
            *result = XR_NULL_VAL;
            return XR_CFUNC_DONE;
        }
        net_listener_clear_error(state->listener);
        xr_free(state);
        *result = make_conn_handle(X, client_fd);
        return XR_CFUNC_DONE;
    }
    // EAGAIN - wait for an incoming connection.
    int64_t timeout_ms =
        net_timeout_until(state->listener ? state->listener->accept_deadline_ms : 0);
#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
    // Completion mode: submit an accept op; the CQE carries the new fd directly
    // (no second accept() after readiness).
    XrRuntime *rt = (XrRuntime *) X->vm.scheduler;
    if (rt && xr_netpoll_uring_active(&rt->netpoll)) {
        XrPollDesc *pd = xr_netpoll_open(&rt->netpoll, state->listen_fd);
        if (pd) {
            state->pd = pd;
            XrCFuncResult cr;
            XrUringReq req = {.kind = XR_URING_OP_ACCEPT, .timeout_ms = timeout_ms};
            if (xr_yield_for_uring_io(X, pd, XR_POLL_READ, &req, net_accept_complete, state, result,
                                      &cr))
                return cr;
            state->pd = NULL;
        }
    }
#endif
    return xr_yield_for_io(X, state->listen_fd, XR_WAIT_READ, timeout_ms, net_accept_continue,
                           state, result);
}

/*
 * net.__accept(listener) -> NetConn | null
 * Yieldable: try-accept fast path, then park until readable.
 */
static XrCFuncResult net_accept_handle_yieldable(XrVMRuntime *X, XrValue *args, int nargs,
                                                 XrValue *result) {
    if (nargs < 1) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }

    XrNetListener *listener = unwrap_listener(args[0]);
    if (!listener || listener->closed || listener->fd < 0) {
        net_listener_set_error(listener, XR_NETERR_CLOSED, 0);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }

    XrIOTryResult r = xr_socket_accept_try(X, listener->fd);
    if (r.ready) {
        if (r.value < 0) {
            net_listener_set_error(listener, net_error_from_errno(r.error), r.error);
            *result = XR_NULL_VAL;
            return XR_CFUNC_DONE;
        }
        net_listener_clear_error(listener);
        *result = make_conn_handle(X, r.value);
        return XR_CFUNC_DONE;
    }

    NetAcceptState *state = (NetAcceptState *) xr_malloc(sizeof(NetAcceptState));
    if (!state) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }
    state->listen_fd = listener->fd;
    state->listener = listener;
    state->pd = NULL;
    return net_accept_step(X, state, result);
}

// ========== Yieldable net.__readInto (TCP + TLS dispatch) ==========

typedef struct {
    int fd;
    XrNetConn *conn;
    XrArray *buf;  // Array<byte> buffer supplied by caller (not owned)
    size_t target_len;
    size_t received;
    bool exact;
    bool is_tls;
    XrPollDesc *pd;  // set on the io_uring completion path
} NetReadIntoState;

static XrCFuncResult net_read_into_step(XrVMRuntime *X, NetReadIntoState *state, XrValue *result);

static XrCFuncResult net_read_into_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                            void *ctx, XrValue *result) {
    (void) resume_value;
    NetReadIntoState *state = (NetReadIntoState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        net_conn_set_error(state->conn, net_code_from_resume(status), 0);
        /* Partial progress survives a timeout or a cancellation: the caller
         * reads what already arrived from buffer.length instead of losing it. */
        state->buf->length = (int32_t) state->received;
        int total = state->received > 0 ? (int) state->received : -1;
        xr_free(state);
        *result = xr_int(total);
        return XR_CFUNC_DONE;
    }
    return net_read_into_step(X, state, result);
}

static XrCFuncResult net_read_into_value(XrNetConn *conn, XrArray *buf, ssize_t n,
                                         XrValue *result) {
    if (n >= 0) {
        buf->length = (int32_t) n;
        net_conn_clear_error(conn);
    }
    *result = xr_int((xr_Integer) n);
    return XR_CFUNC_DONE;
}

static XrCFuncResult net_read_into_done(NetReadIntoState *state, ssize_t n, XrValue *result) {
    XrCFuncResult done = net_read_into_value(state->conn, state->buf, n, result);
    xr_free(state);
    return done;
}

static XrCFuncResult net_read_into_progress(XrVMRuntime *X, NetReadIntoState *state, ssize_t n,
                                            XrValue *result) {
    if (n > 0)
        state->received += (size_t) n;
    if (!state->exact)
        return net_read_into_done(state, n, result);
    if (n == 0 || state->received == state->target_len)
        return net_read_into_done(state, (ssize_t) state->received, result);
    return net_read_into_step(X, state, result);
}

static XrCFuncResult net_read_into_error(NetReadIntoState *state, XrValue *result) {
    state->buf->length = (int32_t) state->received;
    int total = state->received > 0 ? (int) state->received : -1;
    xr_free(state);
    *result = xr_int(total);
    return XR_CFUNC_DONE;
}

#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
// Completion continuation for readInto: the recv op's byte count is in the pd.
static XrCFuncResult net_read_into_complete(XrVMRuntime *X, int status, XrValue resume_value,
                                            void *ctx, XrValue *result) {
    (void) status;
    (void) resume_value;
    NetReadIntoState *state = (NetReadIntoState *) ctx;
    XrUringXferKind kind;
    long n = xr_netpoll_uring_xfer_result(state->pd, XR_POLL_READ, &kind);
    state->pd = NULL;
    if (kind == XR_URING_XFER_DATA)
        return net_read_into_progress(X, state, n, result);
    uint8_t err = (kind == XR_URING_XFER_TIMEOUT)  ? XR_NETERR_TIMEOUT
                  : (kind == XR_URING_XFER_CLOSED) ? XR_NETERR_CLOSED
                                                   : net_error_from_errno((int) (-n));
    net_conn_set_error(state->conn, err, (kind == XR_URING_XFER_ERROR) ? (int) (-n) : 0);
    return net_read_into_error(state, result);
}
#endif

static XrCFuncResult net_read_into_wait(XrVMRuntime *X, XrNetConn *conn, XrArray *buf,
                                        size_t target_len, size_t received, bool exact, bool is_tls,
                                        int wait_mode, int64_t deadline_ms, XrValue *result) {
    NetReadIntoState *state = (NetReadIntoState *) xr_malloc(sizeof(NetReadIntoState));
    if (!state) {
        *result = xr_int(-1);
        return XR_CFUNC_ERROR;
    }
    state->fd = conn->fd;
    state->conn = conn;
    state->buf = buf;
    state->target_len = target_len;
    state->received = received;
    state->exact = exact;
    state->is_tls = is_tls;
    state->pd = NULL;
    return xr_yield_for_io(X, state->fd, wait_mode, net_timeout_until(deadline_ms),
                           net_read_into_continue, state, result);
}

static XrCFuncResult net_read_into_step(XrVMRuntime *X, NetReadIntoState *state, XrValue *result) {
    uint8_t *data = xr_array_raw_u8(state->buf) + state->received;
    size_t remaining = state->target_len - state->received;
#ifdef XR_ENABLE_TLS
    if (state->is_tls) {
        XrTlsConn *tls = state->conn ? (XrTlsConn *) state->conn->tls_state : NULL;
        if (!tls) {
            net_conn_set_error(state->conn, XR_NETERR_TLS, 0);
            return net_read_into_error(state, result);
        }
        for (;;) {
            int n = xr_tls_conn_read_try(tls, (char *) data, (int) remaining);
            if (n >= 0) {
                if (n > 0 && state->exact) {
                    state->received += (size_t) n;
                    if (state->received == state->target_len)
                        return net_read_into_done(state, (ssize_t) state->received, result);
                    data += n;
                    remaining -= (size_t) n;
                    continue;
                }
                return net_read_into_progress(X, state, n, result);
            }
            if (n == -3) {
                net_conn_set_error(state->conn, XR_NETERR_TLS, 0);
                return net_read_into_error(state, result);
            }
            int wait_mode = (n == -1) ? XR_WAIT_READ : XR_WAIT_WRITE;
            int64_t deadline = (wait_mode == XR_WAIT_READ && state->conn)
                                   ? state->conn->read_deadline_ms
                                   : (state->conn ? state->conn->write_deadline_ms : 0);
            return xr_yield_for_io(X, state->fd, wait_mode, net_timeout_until(deadline),
                                   net_read_into_continue, state, result);
        }
    }
#endif

    for (;;) {
        ssize_t n = xr_socket_recv((xr_socket_t) state->fd, data, remaining);
        if (n >= 0) {
            if (n > 0 && state->exact) {
                state->received += (size_t) n;
                if (state->received == state->target_len)
                    return net_read_into_done(state, (ssize_t) state->received, result);
                data += n;
                remaining -= (size_t) n;
                continue;
            }
            return net_read_into_progress(X, state, n, result);
        }
        int socket_error = xr_get_socket_error();
        if (socket_error == XR_EINTR)
            continue;
        if (xr_socket_err_is_again(socket_error)) {
            int64_t timeout_ms = net_timeout_until(state->conn ? state->conn->read_deadline_ms : 0);
#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
            XrRuntime *rt = (XrRuntime *) X->vm.scheduler;
            if (rt && xr_netpoll_uring_active(&rt->netpoll)) {
                XrPollDesc *pd = xr_netpoll_open(&rt->netpoll, state->fd);
                if (pd) {
                    state->pd = pd;
                    XrCFuncResult cr;
                    XrUringReq req = {.kind = XR_URING_OP_RECV,
                                      .buf = data,
                                      .len = (unsigned) remaining,
                                      .timeout_ms = timeout_ms};
                    if (xr_yield_for_uring_io(X, pd, XR_POLL_READ, &req, net_read_into_complete,
                                              state, result, &cr))
                        return cr;
                    state->pd = NULL;
                }
            }
#endif
            return xr_yield_for_io(X, state->fd, XR_WAIT_READ, timeout_ms, net_read_into_continue,
                                   state, result);
        }
        net_conn_set_error(state->conn, net_error_from_errno(socket_error), socket_error);
        return net_read_into_error(state, result);
    }
}

/*
 * net.__readInto(conn, buffer, maxlen) -> int
 * Read once into a reusable Array<byte> buffer. EOF returns 0; errors return -1.
 */
static XrCFuncResult net_read_into_yieldable(XrVMRuntime *X, XrValue *args, int nargs,
                                             XrValue *result) {
    if (nargs < 3) {
        *result = xr_int(-1);
        return XR_CFUNC_ERROR;
    }

    XrNetConn *conn = unwrap_conn(args[0]);
    if (!conn || conn->closed || conn->fd < 0) {
        net_conn_set_error(conn, XR_NETERR_CLOSED, 0);
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }

    XrArray *buf = net_as_writable_bytes(args[1]);
    if (!buf) {
        *result = xr_int(-1);
        return XR_CFUNC_ERROR;
    }

    int max_len = XR_IS_INT(args[2]) ? (int) XR_TO_INT(args[2]) : buf->capacity;
    if (max_len > buf->capacity)
        max_len = buf->capacity;
    if (max_len <= 0) {
        *result = xr_int(-1);
        return XR_CFUNC_ERROR;
    }

    size_t read_len = (size_t) max_len;
    uint8_t *data = xr_array_raw_u8(buf);

#ifdef XR_ENABLE_TLS
    bool is_tls = conn->kind == XR_NETCONN_TLS;
    if (is_tls) {
        XrTlsConn *tls = (XrTlsConn *) conn->tls_state;
        if (!tls) {
            net_conn_set_error(conn, XR_NETERR_TLS, 0);
            return net_read_into_value(conn, buf, -1, result);
        }
        int n = xr_tls_conn_read_try(tls, (char *) data, (int) read_len);
        if (n >= 0)
            return net_read_into_value(conn, buf, n, result);
        if (n == -3) {
            net_conn_set_error(conn, XR_NETERR_TLS, 0);
            return net_read_into_value(conn, buf, -1, result);
        }
        int wait_mode = (n == -1) ? XR_WAIT_READ : XR_WAIT_WRITE;
        int64_t deadline =
            (wait_mode == XR_WAIT_READ) ? conn->read_deadline_ms : conn->write_deadline_ms;
        return net_read_into_wait(X, conn, buf, read_len, 0, false, true, wait_mode, deadline,
                                  result);
    }
#endif

    ssize_t n = xr_socket_recv((xr_socket_t) conn->fd, data, read_len);
    if (n >= 0)
        return net_read_into_value(conn, buf, n, result);
    int socket_error = xr_get_socket_error();
    if (xr_socket_err_is_again(socket_error)) {
        return net_read_into_wait(X, conn, buf, read_len, 0, false, false, XR_WAIT_READ,
                                  conn->read_deadline_ms, result);
    }
    net_conn_set_error(conn, net_error_from_errno(socket_error), socket_error);
    return net_read_into_value(conn, buf, -1, result);
}

// ========== Yieldable net.__writeBytes (TCP + TLS dispatch) ===
typedef struct {
    int fd;
    XrNetConn *conn;
    const char *data;  // Points directly into Array<byte> storage (not owned)
    size_t len;
    size_t written;
    bool is_tls;
    XrPollDesc *pd;  // set on the io_uring completion path
} NetWriteState;

static XrCFuncResult net_write_step(XrVMRuntime *X, NetWriteState *state, XrValue *result);

static XrCFuncResult net_write_continue(XrVMRuntime *X, int status, XrValue resume_value, void *ctx,
                                        XrValue *result) {
    (void) resume_value;
    NetWriteState *state = (NetWriteState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        net_conn_set_error(state->conn, net_code_from_resume(status), 0);
        int total = state->written > 0 ? (int) state->written : -1;
        xr_free(state);
        *result = XR_FROM_INT(total);
        return XR_CFUNC_DONE;
    }
    return net_write_step(X, state, result);
}

#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
// Completion continuation for write: a send op delivered its byte count. Add it
// to the running total and re-enter the step to send any remainder (which uses a
// non-blocking write() fast path and only submits another send op on EAGAIN).
static XrCFuncResult net_write_complete(XrVMRuntime *X, int status, XrValue resume_value, void *ctx,
                                        XrValue *result) {
    (void) status;
    (void) resume_value;
    NetWriteState *state = (NetWriteState *) ctx;
    XrUringXferKind kind;
    long n = xr_netpoll_uring_xfer_result(state->pd, XR_POLL_WRITE, &kind);
    state->pd = NULL;
    if (kind == XR_URING_XFER_DATA) {
        if (n > 0)
            state->written += (size_t) n;
        if (n > 0 && state->written < state->len)
            return net_write_step(X, state, result);  // send the remainder
        int total = state->written > 0 ? (int) state->written : -1;
        if (state->written == state->len)
            net_conn_clear_error(state->conn);
        xr_free(state);
        *result = XR_FROM_INT(total);
        return XR_CFUNC_DONE;
    }
    uint8_t err = (kind == XR_URING_XFER_TIMEOUT)  ? XR_NETERR_TIMEOUT
                  : (kind == XR_URING_XFER_CLOSED) ? XR_NETERR_CLOSED
                                                   : net_error_from_errno((int) (-n));
    net_conn_set_error(state->conn, err, (kind == XR_URING_XFER_ERROR) ? (int) (-n) : 0);
    int total = state->written > 0 ? (int) state->written : -1;
    xr_free(state);
    *result = XR_FROM_INT(total);
    return XR_CFUNC_DONE;
}
#endif

static XrCFuncResult net_write_wait(XrVMRuntime *X, XrNetConn *conn, const char *data, size_t len,
                                    size_t written, bool is_tls, int wait_mode, int64_t deadline_ms,
                                    XrValue *result) {
    NetWriteState *state = (NetWriteState *) xr_calloc(1, sizeof(NetWriteState));
    if (!state) {
        *result = XR_FROM_INT(-1);
        return XR_CFUNC_ERROR;
    }
    state->fd = conn->fd;
    state->conn = conn;
    state->data = data;
    state->len = len;
    state->written = written;
    state->is_tls = is_tls;
    return xr_yield_for_io(X, state->fd, wait_mode, net_timeout_until(deadline_ms),
                           net_write_continue, state, result);
}

static XrCFuncResult net_write_step(XrVMRuntime *X, NetWriteState *state, XrValue *result) {
#ifdef XR_ENABLE_TLS
    if (state->is_tls) {
        XrTlsConn *tls = state->conn ? (XrTlsConn *) state->conn->tls_state : NULL;
        if (!tls) {
            net_conn_set_error(state->conn, XR_NETERR_TLS, 0);
            xr_free(state);
            *result = XR_FROM_INT(-1);
            return XR_CFUNC_DONE;
        }
        while (state->written < state->len) {
            int n = xr_tls_conn_write_try(tls, state->data + state->written,
                                          (int) (state->len - state->written));
            if (n > 0) {
                state->written += n;
                continue;
            }
            if (n == -3) {
                net_conn_set_error(state->conn, XR_NETERR_TLS, 0);
                break;
            }
            // -1=WANT_WRITE, -2=WANT_READ
            int wait_mode = (n == -1) ? XR_WAIT_WRITE : XR_WAIT_READ;
            int64_t deadline = (wait_mode == XR_WAIT_WRITE && state->conn)
                                   ? state->conn->write_deadline_ms
                                   : (state->conn ? state->conn->read_deadline_ms : 0);
            return xr_yield_for_io(X, state->fd, wait_mode, net_timeout_until(deadline),
                                   net_write_continue, state, result);
        }
        int total = (int) state->written;
        if (total == (int) state->len)
            net_conn_clear_error(state->conn);
        else if (total == 0)
            total = -1;
        xr_free(state);
        *result = XR_FROM_INT(total);
        return XR_CFUNC_DONE;
    }
#endif

    // TCP write
    while (state->written < state->len) {
        ssize_t n = xr_socket_send((xr_socket_t) state->fd, state->data + state->written,
                                   state->len - state->written);
        if (n > 0) {
            state->written += n;
            continue;
        }
        if (n == 0)
            break;
        int socket_error = xr_get_socket_error();
        if (xr_socket_err_is_again(socket_error)) {
            int64_t timeout_ms =
                net_timeout_until(state->conn ? state->conn->write_deadline_ms : 0);
#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
            XrRuntime *rt = (XrRuntime *) X->vm.scheduler;
            if (rt && xr_netpoll_uring_active(&rt->netpoll)) {
                XrPollDesc *pd = xr_netpoll_open(&rt->netpoll, state->fd);
                if (pd) {
                    state->pd = pd;
                    XrCFuncResult cr;
                    XrUringReq req = {.kind = XR_URING_OP_SEND,
                                      .buf = (void *) (state->data + state->written),
                                      .len = (unsigned) (state->len - state->written),
                                      .timeout_ms = timeout_ms};
                    if (xr_yield_for_uring_io(X, pd, XR_POLL_WRITE, &req, net_write_complete, state,
                                              result, &cr))
                        return cr;
                    state->pd = NULL;
                }
            }
#endif
            return xr_yield_for_io(X, state->fd, XR_WAIT_WRITE, timeout_ms, net_write_continue,
                                   state, result);
        }
        net_conn_set_error(state->conn, net_error_from_errno(socket_error), socket_error);
        break;
    }
    int total = (int) state->written;
    if (total == (int) state->len)
        net_conn_clear_error(state->conn);
    else if (total == 0)
        total = -1;
    xr_free(state);
    *result = XR_FROM_INT(total);
    return XR_CFUNC_DONE;
}

/*
 * net.__writeBytes(conn, data) -> int
 * Yieldable batch write: drains the whole buffer, parking on EAGAIN or TLS
 * want-read/want-write, honouring the write deadline on every wait.
 */
static XrCFuncResult net_write_bytes_yieldable(XrVMRuntime *X, XrValue *args, int nargs,
                                               XrValue *result) {
    if (nargs < 2) {
        *result = xr_int(-1);
        return XR_CFUNC_ERROR;
    }

    XrNetConn *conn = unwrap_conn(args[0]);
    if (!conn || conn->closed || conn->fd < 0) {
        net_conn_set_error(conn, XR_NETERR_CLOSED, 0);
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }

    XrArray *data = net_as_bytes(args[1]);
    if (!data || (data->length > 0 && !data->data)) {
        *result = xr_int(-1);
        return XR_CFUNC_ERROR;
    }

    if (data->length == 0) {
        net_conn_clear_error(conn);
        *result = xr_int(0);
        return XR_CFUNC_DONE;
    }

    size_t len = (size_t) data->length;
    size_t written = 0;
    // Zero-copy: coroutine arena GC doesn't run while yielded, so a direct
    // reference into the array storage stays valid across the suspension.
    const char *raw = (const char *) xr_array_raw_u8(data);

#ifdef XR_ENABLE_TLS
    bool is_tls = conn->kind == XR_NETCONN_TLS;
    if (is_tls) {
        XrTlsConn *tls = (XrTlsConn *) conn->tls_state;
        if (!tls) {
            net_conn_set_error(conn, XR_NETERR_TLS, 0);
            *result = xr_int(-1);
            return XR_CFUNC_DONE;
        }
        while (written < len) {
            int n = xr_tls_conn_write_try(tls, raw + written, (int) (len - written));
            if (n > 0) {
                written += (size_t) n;
                continue;
            }
            if (n == -3) {
                net_conn_set_error(conn, XR_NETERR_TLS, 0);
                *result = xr_int(written > 0 ? (xr_Integer) written : -1);
                return XR_CFUNC_DONE;
            }
            int wait_mode = (n == -1) ? XR_WAIT_WRITE : XR_WAIT_READ;
            int64_t deadline =
                (wait_mode == XR_WAIT_WRITE) ? conn->write_deadline_ms : conn->read_deadline_ms;
            return net_write_wait(X, conn, raw, len, written, true, wait_mode, deadline, result);
        }
        net_conn_clear_error(conn);
        *result = xr_int((xr_Integer) written);
        return XR_CFUNC_DONE;
    }
#endif

    while (written < len) {
        ssize_t n = xr_socket_send((xr_socket_t) conn->fd, raw + written, len - written);
        if (n > 0) {
            written += (size_t) n;
            continue;
        }
        if (n == 0)
            break;
        int socket_error = xr_get_socket_error();
        if (xr_socket_err_is_again(socket_error)) {
            return net_write_wait(X, conn, raw, len, written, false, XR_WAIT_WRITE,
                                  conn->write_deadline_ms, result);
        }
        net_conn_set_error(conn, net_error_from_errno(socket_error), socket_error);
        break;
    }

    if (written == len) {
        net_conn_clear_error(conn);
    }
    *result = xr_int(written > 0 ? (xr_Integer) written : -1);
    return XR_CFUNC_DONE;
}

// Ensure coroutine has I/O buffer of at least `needed` bytes.
// Returns buffer pointer, or NULL on allocation failure.
// Buffer is lazily allocated (initial 4KB) and grows up to 256KB.
static inline char *xr_coro_ensure_io_buf(XrCoroutine *coro, size_t needed) {
    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext)
        return NULL;
    if (ext->io_buf && ext->io_buf_cap >= needed)
        return ext->io_buf;
    size_t cap = ext->io_buf_cap ? ext->io_buf_cap : 4096;
    while (cap < needed && cap < 262144)
        cap *= 2;
    if (cap < needed)
        cap = needed;
    char *buf = (char *) xr_realloc(ext->io_buf, cap);
    if (!buf)
        return NULL;
    ext->io_buf = buf;
    ext->io_buf_cap = cap;
    return buf;
}

// ========== Native TCP stream copy ==========

typedef struct {
    int src_fd;
    int dst_fd;
    XrNetConn *src_conn;
    XrNetConn *dst_conn;
    XrNetConn *waiting_conn;
    bool src_tls;
    bool dst_tls;
    char *buf;
    size_t cap;
    size_t len;
    size_t off;
    int64_t total;
    bool owns_buf;
    struct NetBidiShared *notify_shared;
    int notify_dir;
    bool shutdown_dst_write_on_finish;
} NetCopyState;

typedef struct NetBidiShared {
    _Atomic int done;
    _Atomic int refs;
    _Atomic bool ok;
    _Atomic int64_t ab;
    _Atomic int64_t ba;
    _Atomic uint8_t error;
} NetBidiShared;

enum {
    NET_BIDI_ERROR_CANCELLED = 250,
    NET_BIDI_ERROR_OUT_OF_MEMORY = 251,
};

typedef enum {
    NET_ERROR_TIMEOUT,
    NET_ERROR_CLOSED,
    NET_ERROR_RESET,
    NET_ERROR_REFUSED,
    NET_ERROR_DNS,
    NET_ERROR_TLS,
    NET_ERROR_IO,
    NET_ERROR_INVALID,
    NET_ERROR_CANCELLED,
    NET_ERROR_OUT_OF_MEMORY,
} NetErrorVariant;

static uint32_t net_error_variant_index(uint8_t error) {
    switch (error) {
        case XR_NETERR_TIMEOUT:
            return NET_ERROR_TIMEOUT;
        case XR_NETERR_CLOSED:
            return NET_ERROR_CLOSED;
        case XR_NETERR_RESET:
            return NET_ERROR_RESET;
        case XR_NETERR_REFUSED:
            return NET_ERROR_REFUSED;
        case XR_NETERR_DNS:
            return NET_ERROR_DNS;
        case XR_NETERR_TLS:
            return NET_ERROR_TLS;
        case XR_NETERR_INVALID:
            return NET_ERROR_INVALID;
        case NET_BIDI_ERROR_CANCELLED:
            return NET_ERROR_CANCELLED;
        case NET_BIDI_ERROR_OUT_OF_MEMORY:
            return NET_ERROR_OUT_OF_MEMORY;
        case XR_NETERR_IO:
        case XR_NETERR_NONE:
        default:
            return NET_ERROR_IO;
    }
}

static XrEnumType *net_error_type(XrVMRuntime *X) {
    return xr_stdlib_enum_type_get(X, "net", "NetError");
}

static void net_set_pending_error(XrVMRuntime *X, uint8_t error) {
    XrEnumType *type = net_error_type(X);
    if (!type)
        return;
    XrEnumAggregateValue *value =
        xr_enum_zero_payload_value(X, type, net_error_variant_index(error));
    if (value)
        xr_vm_set_pending_error(X, XR_FROM_PTR(value));
}

static void net_bidi_shared_set_error(NetBidiShared *shared, uint8_t error) {
    if (!shared || error == XR_NETERR_NONE)
        return;
    uint8_t expected = XR_NETERR_NONE;
    (void) atomic_compare_exchange_strong_explicit(&shared->error, &expected, error,
                                                   memory_order_acq_rel, memory_order_acquire);
    atomic_store_explicit(&shared->ok, false, memory_order_release);
}

static void net_bidi_shared_release(NetBidiShared *shared) {
    if (!shared)
        return;
    int previous = atomic_fetch_sub_explicit(&shared->refs, 1, memory_order_acq_rel);
    if (previous == 1)
        xr_free(shared);
}

static void net_bidi_shared_complete(NetBidiShared *shared, int dir, int64_t bytes, uint8_t error) {
    if (!shared)
        return;
    if (dir == 0)
        atomic_store_explicit(&shared->ab, bytes, memory_order_release);
    else
        atomic_store_explicit(&shared->ba, bytes, memory_order_release);
    if (error != XR_NETERR_NONE)
        net_bidi_shared_set_error(shared, error);
    atomic_fetch_add_explicit(&shared->done, 1, memory_order_release);
    net_bidi_shared_release(shared);
}

static void net_copy_notify(NetCopyState *state, int64_t bytes, bool ok) {
    if (!state || !state->notify_shared)
        return;
    uint8_t error = XR_NETERR_NONE;
    if (!ok) {
        error = state->waiting_conn ? state->waiting_conn->last_error : XR_NETERR_IO;
        if (error == XR_NETERR_NONE)
            error = XR_NETERR_IO;
    }
    net_bidi_shared_complete(state->notify_shared, state->notify_dir, bytes, error);
}

static void net_copy_state_free(NetCopyState *state) {
    if (!state)
        return;
    if (state->owns_buf)
        xr_free(state->buf);
    xr_free(state);
}

static XrCFuncResult net_copy_step(XrVMRuntime *X, NetCopyState *state, XrValue *result);

static XrCFuncResult net_copy_continue(XrVMRuntime *X, int status, XrValue resume_value, void *ctx,
                                       XrValue *result) {
    (void) resume_value;
    NetCopyState *state = (NetCopyState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        net_conn_set_error(state->waiting_conn,
                           status == XR_RESUME_TIMEOUT ? XR_NETERR_TIMEOUT : XR_NETERR_CLOSED, 0);
        if (state->notify_shared) {
            net_bidi_shared_set_error(state->notify_shared, status == XR_RESUME_TIMEOUT
                                                                ? XR_NETERR_TIMEOUT
                                                                : NET_BIDI_ERROR_CANCELLED);
        }
        net_copy_notify(state, state->total, false);
        net_copy_state_free(state);
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }
    return net_copy_step(X, state, result);
}

static XrCFuncResult net_copy_finish(NetCopyState *state, XrValue *result) {
    int64_t total = state->total;
    if (state->shutdown_dst_write_on_finish && state->dst_fd >= 0)
        shutdown(state->dst_fd, XR_SHUT_WR);
    net_copy_notify(state, total, true);
    net_conn_clear_error(state->src_conn);
    net_conn_clear_error(state->dst_conn);
    net_copy_state_free(state);
    *result = xr_int((xr_Integer) total);
    return XR_CFUNC_DONE;
}

static XrCFuncResult net_copy_error(NetCopyState *state, XrValue *result) {
    if (state->waiting_conn && state->waiting_conn->last_error == XR_NETERR_NONE)
        net_conn_set_error(state->waiting_conn, XR_NETERR_IO, 0);
    net_copy_notify(state, state->total, false);
    net_copy_state_free(state);
    *result = xr_int(-1);
    return XR_CFUNC_DONE;
}

static XrCFuncResult net_copy_step(XrVMRuntime *X, NetCopyState *state, XrValue *result) {
    for (;;) {
        if (state->off < state->len) {
#ifdef XR_ENABLE_TLS
            if (state->dst_tls) {
                XrTlsConn *tls = state->dst_conn ? (XrTlsConn *) state->dst_conn->tls_state : NULL;
                if (!tls) {
                    net_conn_set_error(state->dst_conn, XR_NETERR_TLS, 0);
                    return net_copy_error(state, result);
                }
                int n = xr_tls_conn_write_try(tls, state->buf + state->off,
                                              (int) (state->len - state->off));
                if (n > 0) {
                    state->off += (size_t) n;
                    continue;
                }
                if (n == -3) {
                    net_conn_set_error(state->dst_conn, XR_NETERR_TLS, 0);
                    return net_copy_error(state, result);
                }
                int wait_mode = (n == -1) ? XR_WAIT_WRITE : XR_WAIT_READ;
                int64_t deadline = wait_mode == XR_WAIT_WRITE ? state->dst_conn->write_deadline_ms
                                                              : state->dst_conn->read_deadline_ms;
                state->waiting_conn = state->dst_conn;
                return xr_yield_for_io(X, state->dst_fd, wait_mode, net_timeout_until(deadline),
                                       net_copy_continue, state, result);
            }
#endif
            ssize_t n = xr_socket_send((xr_socket_t) state->dst_fd, state->buf + state->off,
                                       state->len - state->off);
            if (n > 0) {
                state->off += (size_t) n;
                continue;
            }
            if (n == 0)
                return net_copy_error(state, result);
            int socket_error = xr_get_socket_error();
            if (xr_socket_err_is_again(socket_error)) {
                state->waiting_conn = state->dst_conn;
                return xr_yield_for_io(X, state->dst_fd, XR_WAIT_WRITE,
                                       net_timeout_until(state->dst_conn->write_deadline_ms),
                                       net_copy_continue, state, result);
            }
            net_conn_set_error(state->dst_conn, net_error_from_errno(socket_error), socket_error);
            return net_copy_error(state, result);
        }

        state->len = 0;
        state->off = 0;

#ifdef XR_ENABLE_TLS
        if (state->src_tls) {
            XrTlsConn *tls = state->src_conn ? (XrTlsConn *) state->src_conn->tls_state : NULL;
            if (!tls) {
                net_conn_set_error(state->src_conn, XR_NETERR_TLS, 0);
                return net_copy_error(state, result);
            }
            int n = xr_tls_conn_read_try(tls, state->buf, (int) state->cap);
            if (n > 0) {
                state->len = (size_t) n;
                state->total += n;
                continue;
            }
            if (n == 0)
                return net_copy_finish(state, result);
            if (n == -3) {
                net_conn_set_error(state->src_conn, XR_NETERR_TLS, 0);
                return net_copy_error(state, result);
            }
            int wait_mode = (n == -1) ? XR_WAIT_READ : XR_WAIT_WRITE;
            int64_t deadline = wait_mode == XR_WAIT_READ ? state->src_conn->read_deadline_ms
                                                         : state->src_conn->write_deadline_ms;
            state->waiting_conn = state->src_conn;
            return xr_yield_for_io(X, state->src_fd, wait_mode, net_timeout_until(deadline),
                                   net_copy_continue, state, result);
        }
#endif
        ssize_t n = xr_socket_recv((xr_socket_t) state->src_fd, state->buf, state->cap);
        if (n > 0) {
            state->len = (size_t) n;
            state->total += n;
            continue;
        }
        if (n == 0)
            return net_copy_finish(state, result);
        int socket_error = xr_get_socket_error();
        if (xr_socket_err_is_again(socket_error)) {
            state->waiting_conn = state->src_conn;
            return xr_yield_for_io(X, state->src_fd, XR_WAIT_READ,
                                   net_timeout_until(state->src_conn->read_deadline_ms),
                                   net_copy_continue, state, result);
        }
        net_conn_set_error(state->src_conn, net_error_from_errno(socket_error), socket_error);
        return net_copy_error(state, result);
    }
}

static XrCFuncResult net_copy_start_ex(XrVMRuntime *X, XrValue src, XrValue dst, int buffer_size,
                                       NetBidiShared *notify_shared, int notify_dir,
                                       bool shutdown_dst_write_on_finish, XrValue *result) {
    XrNetConn *src_conn = unwrap_conn(src);
    XrNetConn *dst_conn = unwrap_conn(dst);
    if (!src_conn || src_conn->closed || src_conn->fd < 0 || !dst_conn || dst_conn->closed ||
        dst_conn->fd < 0) {
        net_conn_set_error(src_conn, XR_NETERR_CLOSED, 0);
        net_conn_set_error(dst_conn, XR_NETERR_CLOSED, 0);
        if (notify_shared) {
            net_bidi_shared_complete(notify_shared, notify_dir, -1, XR_NETERR_CLOSED);
        }
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }

    if (buffer_size <= 0) {
        if (notify_shared) {
            net_bidi_shared_complete(notify_shared, notify_dir, -1, XR_NETERR_INVALID);
        }
        *result = xr_int(-1);
        return notify_shared ? XR_CFUNC_DONE : XR_CFUNC_ERROR;
    }
    if (buffer_size < 1024)
        buffer_size = 1024;
    if (buffer_size > 1048576)
        buffer_size = 1048576;

    XrCoroutine *coro = xr_current_coro(X);
    char *buf = coro ? xr_coro_ensure_io_buf(coro, (size_t) buffer_size) : NULL;
    bool owns_buf = false;
    if (!buf) {
        buf = (char *) xr_malloc((size_t) buffer_size);
        owns_buf = true;
    }
    if (!buf) {
        if (notify_shared) {
            net_bidi_shared_complete(notify_shared, notify_dir, -1, NET_BIDI_ERROR_OUT_OF_MEMORY);
        }
        *result = xr_int(-1);
        return notify_shared ? XR_CFUNC_DONE : XR_CFUNC_ERROR;
    }

    NetCopyState *state = (NetCopyState *) xr_calloc(1, sizeof(NetCopyState));
    if (!state) {
        if (owns_buf)
            xr_free(buf);
        if (notify_shared) {
            net_bidi_shared_complete(notify_shared, notify_dir, -1, NET_BIDI_ERROR_OUT_OF_MEMORY);
        }
        *result = xr_int(-1);
        return notify_shared ? XR_CFUNC_DONE : XR_CFUNC_ERROR;
    }
    state->src_fd = src_conn->fd;
    state->dst_fd = dst_conn->fd;
    state->src_conn = src_conn;
    state->dst_conn = dst_conn;
    state->waiting_conn = NULL;
    state->src_tls = src_conn->kind == XR_NETCONN_TLS;
    state->dst_tls = dst_conn->kind == XR_NETCONN_TLS;
    state->buf = buf;
    state->cap = (size_t) buffer_size;
    state->owns_buf = owns_buf;
    state->notify_shared = notify_shared;
    state->notify_dir = notify_dir;
    state->shutdown_dst_write_on_finish = shutdown_dst_write_on_finish;

    return net_copy_step(X, state, result);
}

typedef struct {
    NetBidiShared *shared;
} NetBidiWaitState;

static XrCFuncResult net_bidi_wait_step(XrVMRuntime *X, NetBidiWaitState *state, XrValue *result);

static XrCFuncResult net_bidi_wait_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                            void *ctx, XrValue *result) {
    (void) resume_value;
    NetBidiWaitState *state = (NetBidiWaitState *) ctx;
    if (status == XR_RESUME_CANCELLED) {
        net_bidi_shared_release(state->shared);
        xr_free(state);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    return net_bidi_wait_step(X, state, result);
}

static XrValue net_bidi_result_object(XrVMRuntime *X, NetBidiWaitState *state) {
    XrClass *cls = xr_stdlib_object_shape_class_get(X, "net", "__CopyBidirectionalResult");
    if (!cls)
        return XR_NULL_VAL;
    XrJson *object = xr_json_new_with_class(xr_current_coro(X), cls);
    if (!object)
        return XR_NULL_VAL;
    /* Set by field name, not by a hardcoded slot: a structural object's physical
     * field order is the canonical (sorted-by-name) order the bytecode reader
     * uses, which need not match the declaration order aToB, bToA. Writing raw
     * slot 0/1 would land each count under the wrong name whenever the canonical
     * order differs, so route both through the same name lookup the reader does.
     * ab is the a-to-b byte count, ba is b-to-a. */
    if (!xr_json_set_by_key(
            X, object, "aToB",
            xr_int((xr_Integer) atomic_load_explicit(&state->shared->ab, memory_order_acquire))) ||
        !xr_json_set_by_key(
            X, object, "bToA",
            xr_int((xr_Integer) atomic_load_explicit(&state->shared->ba, memory_order_acquire))))
        return XR_NULL_VAL;
    return xr_json_value(object);
}

static XrCFuncResult net_bidi_wait_step(XrVMRuntime *X, NetBidiWaitState *state, XrValue *result) {
    if (atomic_load_explicit(&state->shared->done, memory_order_acquire) >= 2) {
        if (!atomic_load_explicit(&state->shared->ok, memory_order_acquire)) {
            uint8_t error = atomic_load_explicit(&state->shared->error, memory_order_acquire);
            net_set_pending_error(X, error == XR_NETERR_NONE ? XR_NETERR_IO : error);
            *result = XR_NULL_VAL;
        } else {
            *result = net_bidi_result_object(X, state);
            if (XR_IS_NULL(*result))
                net_set_pending_error(X, NET_BIDI_ERROR_OUT_OF_MEMORY);
        }
        net_bidi_shared_release(state->shared);
        xr_free(state);
        return XR_CFUNC_DONE;
    }

    return xr_yield_for_timeout(X, 1, net_bidi_wait_continue, state, result);
}

static XrCFuncResult net_copy_direction_coro(XrVMRuntime *X, XrValue *args, int nargs,
                                             XrValue *result) {
    if (nargs < 4 || !XR_IS_INT(args[2]) || !XR_IS_INT(args[3])) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    NetBidiShared *shared = (NetBidiShared *) (intptr_t) XR_TO_INT(args[2]);
    int dir = (int) XR_TO_INT(args[3]);
    return net_copy_start_ex(X, args[0], args[1], 65536, shared, dir, true, result);
}

/*
 * net.copyBidirectional(a, b) -> CopyBidirectionalResult
 * Runs two native stream pumps in opposite directions. Each EOF half-closes
 * the peer write side so request/response style TCP protocols can drain.
 */
static XrCFuncResult net_copy_bidirectional_yieldable(XrVMRuntime *X, XrValue *args, int nargs,
                                                      XrValue *result) {
    if (nargs < 2) {
        net_set_pending_error(X, XR_NETERR_INVALID);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    XrNetConn *a = unwrap_conn(args[0]);
    XrNetConn *b = unwrap_conn(args[1]);
    if (!a || a->closed || a->fd < 0 || !b || b->closed || b->fd < 0) {
        net_conn_set_error(a, XR_NETERR_CLOSED, 0);
        net_conn_set_error(b, XR_NETERR_CLOSED, 0);
        net_set_pending_error(X, XR_NETERR_CLOSED);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    if (a == b || a->fd == b->fd) {
        net_set_pending_error(X, XR_NETERR_INVALID);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }

    NetBidiShared *shared = (NetBidiShared *) xr_calloc(1, sizeof(NetBidiShared));
    if (!shared) {
        net_set_pending_error(X, NET_BIDI_ERROR_OUT_OF_MEMORY);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    atomic_init(&shared->done, 0);
    atomic_init(&shared->refs, 3);
    atomic_init(&shared->ok, true);
    atomic_init(&shared->error, XR_NETERR_NONE);
    atomic_init(&shared->ab, 0);
    atomic_init(&shared->ba, 0);

    XrValue shared_ptr = xr_int((xr_Integer) (intptr_t) shared);
    XrValue ab_args[4] = {args[0], args[1], shared_ptr, xr_int(0)};
    XrValue ba_args[4] = {args[1], args[0], shared_ptr, xr_int(1)};
    XrCoroutine *ab =
        xr_coro_create_vm_cfunc(X, net_copy_direction_coro, ab_args, 4, "net.copy.ab");
    XrCoroutine *ba =
        xr_coro_create_vm_cfunc(X, net_copy_direction_coro, ba_args, 4, "net.copy.ba");
    if (!ab || !ba) {
        xr_coro_destroy(ab);
        xr_coro_destroy(ba);
        xr_free(shared);
        net_set_pending_error(X, NET_BIDI_ERROR_OUT_OF_MEMORY);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }

    NetBidiWaitState *state = (NetBidiWaitState *) xr_calloc(1, sizeof(NetBidiWaitState));
    if (!state) {
        xr_coro_destroy(ab);
        xr_coro_destroy(ba);
        xr_free(shared);
        net_set_pending_error(X, NET_BIDI_ERROR_OUT_OF_MEMORY);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    state->shared = shared;
    xr_coro_spawn(X, ab);
    xr_coro_spawn(X, ba);
    return net_bidi_wait_step(X, state, result);
}

// ========== TCP half-close ==========

static XrValue net_shutdown_mode(XrVMRuntime *X, XrValue *args, int nargs, int mode) {
    (void) X;
    if (nargs < 1)
        return xr_bool(false);
    XrNetConn *c = unwrap_conn(args[0]);
    if (!c || c->closed || c->fd < 0) {
        net_conn_set_error(c, XR_NETERR_CLOSED, 0);
        return xr_bool(false);
    }
    if (shutdown(c->fd, mode) == 0) {
        net_conn_clear_error(c);
        return xr_bool(true);
    }
    net_conn_set_error(c, net_error_from_errno(errno), errno);
    return xr_bool(false);
}

static XrValue net_shutdown_read(XrVMRuntime *X, XrValue *args, int nargs) {
    return net_shutdown_mode(X, args, nargs, XR_SHUT_RD);
}

static XrValue net_shutdown_write(XrVMRuntime *X, XrValue *args, int nargs) {
    return net_shutdown_mode(X, args, nargs, XR_SHUT_WR);
}

static XrValue net_shutdown_conn(XrVMRuntime *X, XrValue *args, int nargs) {
    return net_shutdown_mode(X, args, nargs, XR_SHUT_RDWR);
}

// ========== net.__listenFd ==========

/*
 * net.__listenFd(port, backlog, forceV4) -> NetListener | null
 * xr_io_listen owns the dual-stack-preferred bind; forceV4 pins the socket to
 * an IPv4 wildcard for callers that must interoperate with v4-only tooling.
 */
static XrValue net_listen_fd(XrVMRuntime *X, XrValue *args, int nargs) {
    if (nargs < 3 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return XR_NULL_VAL;

    int port_num = (int) XR_TO_INT(args[0]);
    int backlog = (int) XR_TO_INT(args[1]);
    bool force_v4 = XR_IS_BOOL(args[2]) && XR_TO_BOOL(args[2]);
    if (port_num < 0 || port_num > 65535 || backlog <= 0)
        return XR_NULL_VAL;

    int fd = xr_io_listen(force_v4 ? "0.0.0.0" : NULL, port_num, backlog);
    if (fd < 0)
        return XR_NULL_VAL;

    /* Ephemeral port: query the kernel-assigned port via getsockname */
    if (port_num == 0) {
        struct sockaddr_storage ss;
        socklen_t sslen = sizeof(ss);
        if (getsockname(fd, (struct sockaddr *) &ss, &sslen) == 0) {
            if (ss.ss_family == AF_INET6)
                port_num = ntohs(((struct sockaddr_in6 *) &ss)->sin6_port);
            else
                port_num = ntohs(((struct sockaddr_in *) &ss)->sin_port);
        }
    }

    return make_listener_handle(X, fd, port_num);
}

// ========== net.__close / net.__fd ==========

/*
 * net.__close(handle) -> void
 * Close connection, listener, or UDP socket. Safe to call multiple times.
 */
static XrValue net_close_handle(XrVMRuntime *X, XrValue *args, int nargs) {
    (void) X;
    if (nargs < 1)
        return XR_NULL_VAL;

    XrNetConn *c = unwrap_conn(args[0]);
    if (c) {
        xr_net_conn_close(c);
        return XR_NULL_VAL;
    }
    XrNetListener *l = unwrap_listener(args[0]);
    if (l) {
        xr_net_listener_close(l);
    }
    return XR_NULL_VAL;
}

static XrValue net_fd_handle(XrVMRuntime *X, XrValue *args, int nargs) {
    if (nargs < 1)
        return xr_int(-1);
    return xr_int(handle_get_fd(X, args[0]));
}

// ========== Deadline and diagnostic error primitives ==========

static XrValue net_set_read_deadline(XrVMRuntime *X, XrValue *args, int nargs) {
    (void) X;
    if (nargs < 2 || !XR_IS_INT(args[1]))
        return xr_bool(false);
    XrNetConn *c = unwrap_conn(args[0]);
    if (!c || c->closed)
        return xr_bool(false);
    int64_t deadline = (int64_t) XR_TO_INT(args[1]);
    if (deadline < 0)
        return xr_bool(false);
    c->read_deadline_ms = deadline;
    return xr_bool(true);
}

static XrValue net_set_write_deadline(XrVMRuntime *X, XrValue *args, int nargs) {
    (void) X;
    if (nargs < 2 || !XR_IS_INT(args[1]))
        return xr_bool(false);
    XrNetConn *c = unwrap_conn(args[0]);
    if (!c || c->closed)
        return xr_bool(false);
    int64_t deadline = (int64_t) XR_TO_INT(args[1]);
    if (deadline < 0)
        return xr_bool(false);
    c->write_deadline_ms = deadline;
    return xr_bool(true);
}

static XrValue net_set_deadline(XrVMRuntime *X, XrValue *args, int nargs) {
    (void) X;
    if (nargs < 2 || !XR_IS_INT(args[1]))
        return xr_bool(false);
    XrNetConn *c = unwrap_conn(args[0]);
    if (!c || c->closed)
        return xr_bool(false);
    int64_t deadline = (int64_t) XR_TO_INT(args[1]);
    if (deadline < 0)
        return xr_bool(false);
    c->read_deadline_ms = deadline;
    c->write_deadline_ms = deadline;
    return xr_bool(true);
}

static XrValue net_set_accept_deadline(XrVMRuntime *X, XrValue *args, int nargs) {
    (void) X;
    if (nargs < 2 || !XR_IS_INT(args[1]))
        return xr_bool(false);
    XrNetListener *l = unwrap_listener(args[0]);
    if (!l || l->closed)
        return xr_bool(false);
    int64_t deadline = (int64_t) XR_TO_INT(args[1]);
    if (deadline < 0)
        return xr_bool(false);
    l->accept_deadline_ms = deadline;
    return xr_bool(true);
}

static XrValue net_last_code(XrVMRuntime *X, XrValue *args, int nargs) {
    (void) X;
    if (nargs < 1)
        return xr_int(XR_NETERR_INVALID);
    XrNetConn *c = unwrap_conn(args[0]);
    if (c)
        return xr_int(c->last_error);
    XrNetListener *l = unwrap_listener(args[0]);
    if (l)
        return xr_int(l->last_error);
    return xr_int(XR_NETERR_INVALID);
}

static XrValue net_now_ms_fn(XrVMRuntime *X, XrValue *args, int nargs) {
    (void) X;
    (void) args;
    (void) nargs;
    return xr_int(net_now_ms());
}

static XrValue net_last_connect_code(XrVMRuntime *X, XrValue *args, int nargs) {
    (void) X;
    (void) args;
    (void) nargs;
    return xr_int(g_last_connect_code);
}

static XrValue net_last_errno(XrVMRuntime *X, XrValue *args, int nargs) {
    (void) X;
    if (nargs < 1)
        return xr_int(0);
    XrNetConn *c = unwrap_conn(args[0]);
    if (c)
        return xr_int(c->last_errno);
    XrNetListener *l = unwrap_listener(args[0]);
    if (l)
        return xr_int(l->last_errno);
    return xr_int(0);
}

// ========== net.__hasTLS ==========

static XrValue net_has_tls(XrVMRuntime *X, XrValue *args, int nargs) {
    (void) X;
    (void) args;
    (void) nargs;
#ifdef XR_ENABLE_TLS
    return xr_bool(true);
#else
    return xr_bool(false);
#endif
}

// ========== Yieldable net.__tlsHandshake ==========

#ifdef XR_ENABLE_TLS

typedef struct {
    XrValue handle;  // NetConn being promoted; attach happens only on success
    XrTlsConn *tls;  // owned by this state machine until attached or freed
    int fd;
    int64_t deadline_ms;  // absolute; every wait re-derives the remaining slice
} NetTlsHandshakeState;

static XrCFuncResult net_tls_handshake_step(XrVMRuntime *X, NetTlsHandshakeState *state,
                                            XrValue *result);

/* Any handshake failure closes the connection: the script layer receives a
 * bare error code with no live handle to clean up, which is what lets dial
 * keep its TLS arm as a single fresh-returning tail call. */
static XrCFuncResult net_tls_handshake_fail(NetTlsHandshakeState *state, uint8_t code,
                                            XrValue *result) {
    XrNetConn *conn = unwrap_conn(state->handle);
    net_conn_set_error(conn, code, 0);
    xr_tls_conn_close(state->tls);
    xr_tls_conn_free(state->tls);
    if (conn)
        xr_net_conn_close(conn);
    xr_free(state);
    *result = xr_int(code);
    return XR_CFUNC_DONE;
}

static XrCFuncResult net_tls_handshake_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                                void *ctx, XrValue *result) {
    (void) resume_value;
    NetTlsHandshakeState *state = (NetTlsHandshakeState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED)
        return net_tls_handshake_fail(state, net_code_from_resume(status), result);
    return net_tls_handshake_step(X, state, result);
}

static XrCFuncResult net_tls_handshake_step(XrVMRuntime *X, NetTlsHandshakeState *state,
                                            XrValue *result) {
    int hs = xr_tls_conn_handshake_try(state->tls);
    if (hs == 0) {
        XrNetConn *conn = unwrap_conn(state->handle);
        if (!conn || conn->closed)
            return net_tls_handshake_fail(state, XR_NETERR_CLOSED, result);
        xr_net_conn_set_tls(conn, state->tls);
        net_conn_clear_error(conn);
        xr_free(state);
        *result = xr_int(0);
        return XR_CFUNC_DONE;
    }
    if (hs < 0)
        return net_tls_handshake_fail(state, XR_NETERR_TLS, result);
    // 1=WANT_READ, 2=WANT_WRITE; the wait consumes the remaining slice of the
    // one absolute deadline instead of re-arming the full timeout per yield.
    int wait_mode = (hs == 1) ? XR_WAIT_READ : XR_WAIT_WRITE;
    return xr_yield_for_io(X, state->fd, wait_mode, net_timeout_until(state->deadline_ms),
                           net_tls_handshake_continue, state, result);
}

/*
 * net.__tlsHandshake(conn, hostname, deadlineMs) -> int
 * Yieldable client handshake on an established TCP conn under one absolute
 * monotonic deadline (0 means none). Returns 0 on success (the conn is
 * promoted to TLS in place); every failure closes the conn and returns a
 * portable net error code, also stored on the handle. The result is a bare
 * int, not `NetConn | int`: unioning a builtin native class with a scalar
 * forces module-wide discrimination that miscompiles suspended handle
 * results inside coroutine frames.
 */
static XrCFuncResult net_tls_handshake_yieldable(XrVMRuntime *X, XrValue *args, int nargs,
                                                 XrValue *result) {
    if (nargs < 3 || !XR_IS_STRING(args[1]) || !XR_IS_INT(args[2])) {
        *result = xr_int(XR_NETERR_INVALID);
        return XR_CFUNC_DONE;
    }
    XrNetConn *conn = unwrap_conn(args[0]);
    if (!conn || conn->closed || conn->fd < 0 || conn->kind != XR_NETCONN_TCP) {
        net_conn_set_error(conn, XR_NETERR_INVALID, 0);
        if (conn)
            xr_net_conn_close(conn);
        *result = xr_int(XR_NETERR_INVALID);
        return XR_CFUNC_DONE;
    }

    XrTlsContext *ctx = get_tls_client_ctx();
    if (!ctx) {
        net_conn_set_error(conn, XR_NETERR_TLS, 0);
        xr_net_conn_close(conn);
        *result = xr_int(XR_NETERR_TLS);
        return XR_CFUNC_DONE;
    }
    XrTlsConn *tls = xr_tls_conn_new(ctx, conn->fd);
    if (!tls) {
        net_conn_set_error(conn, XR_NETERR_TLS, 0);
        xr_net_conn_close(conn);
        *result = xr_int(XR_NETERR_TLS);
        return XR_CFUNC_DONE;
    }
    xr_tls_conn_set_hostname(tls, XR_STRING_CHARS(XR_TO_STRING(args[1])));

    NetTlsHandshakeState *state =
        (NetTlsHandshakeState *) xr_calloc(1, sizeof(NetTlsHandshakeState));
    if (!state) {
        xr_tls_conn_close(tls);
        xr_tls_conn_free(tls);
        xr_net_conn_close(conn);
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }
    state->handle = args[0];
    state->tls = tls;
    state->fd = conn->fd;
    state->deadline_ms = (int64_t) XR_TO_INT(args[2]);
    if (state->deadline_ms < 0)
        state->deadline_ms = 0;
    return net_tls_handshake_step(X, state, result);
}

#endif  // XR_ENABLE_TLS

// ========== UDP primitives ==========

/*
 * net.__udpBind(port, addr) -> NetConn | null
 * Empty addr binds the family-appropriate wildcard.
 */
static XrValue net_udp_bind_handle(XrVMRuntime *X, XrValue *args, int nargs) {
    if (nargs < 2 || !XR_IS_INT(args[0]) || !XR_IS_STRING(args[1]))
        return XR_NULL_VAL;
    int port_num = (int) XR_TO_INT(args[0]);
    const char *addr = XR_STRING_CHARS(XR_TO_STRING(args[1]));
    if (port_num < 0 || port_num > 65535)
        return XR_NULL_VAL;

    sa_family_t family = AF_INET;
    if (addr[0] && strchr(addr, ':'))
        family = AF_INET6;

    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0)
        return XR_NULL_VAL;

    if (family == AF_INET) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t) port_num);
        if (addr[0])
            inet_pton(AF_INET, addr, &sa.sin_addr);
        else
            sa.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
            net_close_fd(X, fd);
            return XR_NULL_VAL;
        }
    } else {
        struct sockaddr_in6 sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin6_family = AF_INET6;
        sa.sin6_port = htons((uint16_t) port_num);
        if (addr[0])
            inet_pton(AF_INET6, addr, &sa.sin6_addr);
        else
            sa.sin6_addr = in6addr_any;
        if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
            net_close_fd(X, fd);
            return XR_NULL_VAL;
        }
    }

    extern int xr_socket_set_nonblock(int fd);
    xr_socket_set_nonblock(fd);

    return make_udp_handle(X, fd);
}

typedef struct {
    int fd;
    XrNetConn *conn;
    const char *data;  // Points into Array<byte> storage (not owned)
    size_t len;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    int64_t deadline_ms;
} NetUdpSendState;

static XrCFuncResult net_udp_send_step(XrVMRuntime *X, NetUdpSendState *state, XrValue *result);

static XrCFuncResult net_udp_send_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                           void *ctx, XrValue *result) {
    (void) resume_value;
    NetUdpSendState *state = (NetUdpSendState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        net_conn_set_error(state->conn, net_code_from_resume(status), 0);
        xr_free(state);
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }
    return net_udp_send_step(X, state, result);
}

static XrCFuncResult net_udp_send_step(XrVMRuntime *X, NetUdpSendState *state, XrValue *result) {
    ssize_t n = sendto(state->fd, state->data, state->len, 0, (struct sockaddr *) &state->addr,
                       state->addr_len);
    if (n >= 0) {
        net_conn_clear_error(state->conn);
        xr_free(state);
        *result = xr_int((xr_Integer) n);
        return XR_CFUNC_DONE;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return xr_yield_for_io(X, state->fd, XR_WAIT_WRITE, net_timeout_until(state->deadline_ms),
                               net_udp_send_continue, state, result);
    }
    net_conn_set_error(state->conn, net_error_from_errno(errno), errno);
    xr_free(state);
    *result = xr_int(-1);
    return XR_CFUNC_DONE;
}

/*
 * net.__udpSendTo(conn, data, addrLiteral, port, timeoutMs) -> int
 * Yieldable single datagram send. The destination must be a literal address;
 * hostname resolution is net.xr policy.
 */
static XrCFuncResult net_udp_send_to_yieldable(XrVMRuntime *X, XrValue *args, int nargs,
                                               XrValue *result) {
    if (nargs < 5 || !XR_IS_STRING(args[2]) || !XR_IS_INT(args[3]) || !XR_IS_INT(args[4])) {
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }
    XrNetConn *conn = unwrap_conn(args[0]);
    if (!conn || conn->closed || conn->fd < 0) {
        net_conn_set_error(conn, XR_NETERR_CLOSED, 0);
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }
    XrArray *data = net_as_bytes(args[1]);
    if (!data || (data->length > 0 && !data->data)) {
        net_conn_set_error(conn, XR_NETERR_INVALID, 0);
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }
    const char *addr_text = XR_STRING_CHARS(XR_TO_STRING(args[2]));
    int port_num = (int) XR_TO_INT(args[3]);
    int64_t timeout_ms = (int64_t) XR_TO_INT(args[4]);
    if (port_num < 0 || port_num > 65535 || !net_is_literal_ip(addr_text)) {
        net_conn_set_error(conn, XR_NETERR_INVALID, 0);
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }

    XrSockAddr resolved;
    if (!xr_dns_resolve(X, addr_text, &resolved, XR_AF_UNSPEC)) {
        net_conn_set_error(conn, XR_NETERR_INVALID, 0);
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }

    NetUdpSendState *state = (NetUdpSendState *) xr_calloc(1, sizeof(NetUdpSendState));
    if (!state) {
        *result = xr_int(-1);
        return XR_CFUNC_ERROR;
    }
    state->fd = conn->fd;
    state->conn = conn;
    // Zero-copy: the array storage stays pinned while this coroutine is parked.
    state->data = (const char *) xr_array_raw_u8(data);
    state->len = (size_t) data->length;
    state->deadline_ms = net_deadline_from_timeout(timeout_ms);
    memset(&state->addr, 0, sizeof(state->addr));
    if (resolved.family == AF_INET) {
        resolved.addr.v4.sin_port = htons((uint16_t) port_num);
        memcpy(&state->addr, &resolved.addr.v4, sizeof(struct sockaddr_in));
        state->addr_len = sizeof(struct sockaddr_in);
    } else {
        resolved.addr.v6.sin6_port = htons((uint16_t) port_num);
        memcpy(&state->addr, &resolved.addr.v6, sizeof(struct sockaddr_in6));
        state->addr_len = sizeof(struct sockaddr_in6);
    }
    return net_udp_send_step(X, state, result);
}

typedef struct {
    int fd;
    XrNetConn *conn;
    XrArray *buf;  // caller-owned Array<byte>
    int64_t deadline_ms;
} NetUdpRecvState;

static XrCFuncResult net_udp_recv_step(XrVMRuntime *X, NetUdpRecvState *state, XrValue *result);

static XrCFuncResult net_udp_recv_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                           void *ctx, XrValue *result) {
    (void) resume_value;
    NetUdpRecvState *state = (NetUdpRecvState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        net_conn_set_error(state->conn, net_code_from_resume(status), 0);
        xr_free(state);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    return net_udp_recv_step(X, state, result);
}

static XrCFuncResult net_udp_recv_step(XrVMRuntime *X, NetUdpRecvState *state, XrValue *result) {
    uint8_t *data = xr_array_raw_u8(state->buf);
    struct sockaddr_storage saddr;
    socklen_t slen = sizeof(saddr);

    ssize_t n = recvfrom(state->fd, data, (size_t) state->buf->capacity, 0,
                         (struct sockaddr *) &saddr, &slen);
    if (n >= 0) {
        state->buf->length = (int32_t) n;
        XrNetConn *conn = state->conn;
        conn->udp_from_host[0] = '\0';
        conn->udp_from_port = 0;
        if (saddr.ss_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *) &saddr;
            inet_ntop(AF_INET, &sin->sin_addr, conn->udp_from_host, sizeof(conn->udp_from_host));
            conn->udp_from_port = ntohs(sin->sin_port);
        } else if (saddr.ss_family == AF_INET6) {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *) &saddr;
            inet_ntop(AF_INET6, &sin6->sin6_addr, conn->udp_from_host, sizeof(conn->udp_from_host));
            conn->udp_from_port = ntohs(sin6->sin6_port);
        }
        net_conn_clear_error(conn);
        xr_free(state);
        *result = xr_int((xr_Integer) n);
        return XR_CFUNC_DONE;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return xr_yield_for_io(X, state->fd, XR_WAIT_READ, net_timeout_until(state->deadline_ms),
                               net_udp_recv_continue, state, result);
    }

    net_conn_set_error(state->conn, net_error_from_errno(errno), errno);
    xr_free(state);
    *result = xr_int(-1);
    return XR_CFUNC_DONE;
}

/*
 * net.__udpRecvInto(conn, buffer, timeoutMs) -> int
 * Yieldable single datagram receive into a caller buffer. Returns the byte
 * count and records the sender on the handle; -1 means timeout, cancellation,
 * or a socket error with the code stored.
 */
static XrCFuncResult net_udp_recv_into_yieldable(XrVMRuntime *X, XrValue *args, int nargs,
                                                 XrValue *result) {
    if (nargs < 3 || !XR_IS_INT(args[2])) {
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }
    XrNetConn *conn = unwrap_conn(args[0]);
    if (!conn || conn->closed || conn->fd < 0) {
        net_conn_set_error(conn, XR_NETERR_CLOSED, 0);
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }
    XrArray *buf = net_as_writable_bytes(args[1]);
    if (!buf) {
        net_conn_set_error(conn, XR_NETERR_INVALID, 0);
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }

    NetUdpRecvState *state = (NetUdpRecvState *) xr_calloc(1, sizeof(NetUdpRecvState));
    if (!state) {
        *result = xr_int(-1);
        return XR_CFUNC_ERROR;
    }
    state->fd = conn->fd;
    state->conn = conn;
    state->buf = buf;
    state->deadline_ms = net_deadline_from_timeout((int64_t) XR_TO_INT(args[2]));
    return net_udp_recv_step(X, state, result);
}

static XrValue net_udp_from_host(XrVMRuntime *X, XrValue *args, int nargs) {
    if (nargs < 1)
        return xr_string_value(xr_string_intern(X, "", 0, 0));
    XrNetConn *conn = unwrap_conn(args[0]);
    const char *host = conn ? conn->udp_from_host : "";
    return xr_string_value(xr_string_intern(X, host, strlen(host), 0));
}

static XrValue net_udp_from_port(XrVMRuntime *X, XrValue *args, int nargs) {
    (void) X;
    if (nargs < 1)
        return xr_int(0);
    XrNetConn *conn = unwrap_conn(args[0]);
    return xr_int(conn ? conn->udp_from_port : 0);
}

#define XR_STDLIB_VM_BIND_MODULE_NET 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_NET

/* ========== Native-type instance methods (synchronous) ==========
 *
 * Yieldable operations (read / write / accept) stay as module-level
 * cfuncs because the native-type method table currently only carries
 * the synchronous XrCFunctionPtr signature. Once the dispatcher grows
 * a yieldable variant, the matching wrappers can move here.
 */

static XrValue conn_method_fd(XrVMRuntime *X, XrValue self, XrValue *args, int n) {
    (void) X;
    (void) args;
    (void) n;
    XrNetConn *c = unwrap_conn(self);
    return xr_int(c ? c->fd : -1);
}

static XrValue conn_method_close(XrVMRuntime *X, XrValue self, XrValue *args, int n) {
    (void) X;
    (void) args;
    (void) n;
    XrNetConn *c = unwrap_conn(self);
    if (c)
        xr_net_conn_close(c);
    return XR_NULL_VAL;
}

static XrValue conn_method_is_closed(XrVMRuntime *X, XrValue self, XrValue *args, int n) {
    (void) X;
    (void) args;
    (void) n;
    XrNetConn *c = unwrap_conn(self);
    return xr_bool(!c || c->closed);
}

static XrValue conn_method_is_tls(XrVMRuntime *X, XrValue self, XrValue *args, int n) {
    (void) X;
    (void) args;
    (void) n;
    XrNetConn *c = unwrap_conn(self);
    return xr_bool(c && c->kind == XR_NETCONN_TLS);
}

static XrValue listener_method_fd(XrVMRuntime *X, XrValue self, XrValue *args, int n) {
    (void) X;
    (void) args;
    (void) n;
    XrNetListener *l = unwrap_listener(self);
    return xr_int(l ? l->fd : -1);
}

static XrValue listener_method_port(XrVMRuntime *X, XrValue self, XrValue *args, int n) {
    (void) X;
    (void) args;
    (void) n;
    XrNetListener *l = unwrap_listener(self);
    return xr_int(l ? l->port : -1);
}

static XrValue listener_method_close(XrVMRuntime *X, XrValue self, XrValue *args, int n) {
    (void) X;
    (void) args;
    (void) n;
    XrNetListener *l = unwrap_listener(self);
    if (l)
        xr_net_listener_close(l);
    return XR_NULL_VAL;
}

static XrValue listener_method_is_closed(XrVMRuntime *X, XrValue self, XrValue *args, int n) {
    (void) X;
    (void) args;
    (void) n;
    XrNetListener *l = unwrap_listener(self);
    return xr_bool(!l || l->closed);
}

#define XR_STDLIB_VM_BIND_CLASS_NET_CONN 1
#define XR_STDLIB_VM_BIND_CLASS_NET_LISTENER 1
#include "../../src/stdlib/xstdlib_class_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_CLASS_NET_LISTENER
#undef XR_STDLIB_VM_BIND_CLASS_NET_CONN

/* NetConn and NetListener class registrations are invoked
 * unconditionally during isolate init by
 * xr_prelude_register_all_native_types, so the XrClasses are available
 * even when user code never `import net`. */
void xr_netconn_register_class(XrVMRuntime *isolate) {
    xr_stdlib_vm_register_net_conn_class_generated(isolate);
}

void xr_netlistener_register_class(XrVMRuntime *isolate) {
    xr_stdlib_vm_register_net_listener_class_generated(isolate);
}

XrModule *xr_load_module_net(XrVMRuntime *isolate) {
    net_platform_init();
    XrModule *mod = xr_module_create_native(isolate, "net");

    // NetConn / NetListener XrClasses are registered up front by the
    // prelude module; nothing to do here.

    xr_stdlib_vm_bind_net_generated(isolate, mod);

    return mod;
}
