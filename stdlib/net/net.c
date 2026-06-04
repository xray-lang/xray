/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * net.c - Network standard library implementation
 *
 * KEY CONCEPT:
 *   Unified network interface built on http_io/http_tls/http_dns
 */

#include "../../src/base/xmalloc.h"
#include "../common.h"
#include "net.h"
#include "io.h"
#include "tls.h"
#include "../../src/io/xdns.h"
#include "../../src/io/xnet_handle.h"
#include "../../src/runtime/class/xclass.h"
#include "../../src/runtime/class/xclass_builder.h"
#include "../../src/runtime/class/xclass_system.h"
#include "../../src/runtime/class/xinstance.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xjson.h"

#include "../../src/module/xmodule.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/coro/xworker.h"
#include "../../src/coro/xnetpoll.h"
#include "../../src/vm/xvm_internal.h"
#include "../../src/runtime/symbol/xsymbol_table.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/os/os_time.h"

// Import types and functions from xsocket.h (avoid header conflicts)
typedef struct {
    bool ready;
    int value;
    int error;
} XrIOTryResult;

extern XrIOTryResult xr_socket_accept_try(struct XrayIsolate *X, int listen_fd);
extern XrIOTryResult xr_socket_read_try(struct XrayIsolate *X, int fd, char *buf, int maxlen);
extern XrIOTryResult xr_socket_write_try(struct XrayIsolate *X, int fd, const char *data,
                                         size_t len);
extern void xr_socket_close(struct XrayIsolate *X, int fd);
extern XrCoroutine *xr_coro_create_cfunc(XrayIsolate *X,
                                         XrCFuncResult (*cfunc)(XrayIsolate *, XrValue *, int,
                                                                XrValue *),
                                         XrValue *args, int argc, const char *name);

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

/*
 * Close a socket fd with proper netpoll cleanup.
 * Handles: netpoll deregistration, shutdown, close.
 * Safe to call with fd < 0 or NULL isolate.
 */
static void net_close_fd(XrayIsolate *X, int fd) {
    if (fd < 0)
        return;
    XrRuntime *runtime = X ? (XrRuntime *) X->vm.runtime : NULL;
    if (runtime) {
        XrPollDesc *pd = xr_fdmap_get(&runtime->netpoll, fd);
        if (pd && !atomic_load(&pd->closing))
            xr_netpoll_close(&runtime->netpoll, pd);
    }
    shutdown(fd, XR_SHUT_WR);
    xr_closesocket(fd);
}

/*
 * DNS resolve + create non-blocking TCP socket + start connect.
 * Returns fd on success (connect may still be in progress), -1 on failure.
 * On EINPROGRESS the caller should yield for write then check SO_ERROR.
 * On immediate connect (ret 0) the caller can proceed directly.
 * *out_ret receives the connect() return value (0 or -1 with errno).
 */
static int net_tcp_connect(XrayIsolate *X, const char *host, int port, int *out_ret) {
    XrSockAddr addr;
    if (!xr_dns_resolve(X, host, &addr, XR_AF_UNSPEC))
        return -1;

    int fd = socket(addr.family, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    xr_io_set_nonblocking(fd);
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr *sa;
    socklen_t sa_len;
    if (addr.family == AF_INET) {
        addr.addr.v4.sin_port = htons(port);
        sa = (struct sockaddr *) &addr.addr.v4;
        sa_len = sizeof(struct sockaddr_in);
    } else {
        addr.addr.v6.sin6_port = htons(port);
        sa = (struct sockaddr *) &addr.addr.v6;
        sa_len = sizeof(struct sockaddr_in6);
    }

    int ret = connect(fd, sa, sa_len);
    if (out_ret)
        *out_ret = ret;

    if (ret == 0 || errno == EINPROGRESS)
        return fd;

    close(fd);
    return -1;
}

// ========== DNS resolve helper (used by net.lookup binding) ==========

static int net_dns_lookup_to_addrs(XrayIsolate *X, const char *hostname, XrNetAddr *addrs,
                                   int max_addrs) {
    if (!hostname || !addrs || max_addrs <= 0)
        return 0;

    XrSockAddr resolved[8];
    int count =
        xr_dns_resolve_all(X, hostname, resolved, max_addrs > 8 ? 8 : max_addrs, XR_AF_UNSPEC);

    for (int i = 0; i < count; i++) {
        if (resolved[i].family == AF_INET) {
            addrs[i].family = XR_NET_IPV4;
            inet_ntop(AF_INET, &resolved[i].addr.v4.sin_addr, addrs[i].host, sizeof(addrs[i].host));
        } else {
            addrs[i].family = XR_NET_IPV6;
            inet_ntop(AF_INET6, &resolved[i].addr.v6.sin6_addr, addrs[i].host,
                      sizeof(addrs[i].host));
        }
        addrs[i].port = 0;
    }
    return count;
}

// ========== Utility Functions ==========

int xr_net_parse_addr(const char *addr_str, char *host, size_t host_len, int *port) {
    if (!addr_str)
        return -1;

    const char *colon = strrchr(addr_str, ':');
    if (!colon) {
        if (host && host_len > 0) {
            strncpy(host, addr_str, host_len - 1);
            host[host_len - 1] = '\0';
        }
        if (port)
            *port = 0;
        return 0;
    }

    size_t host_part_len = colon - addr_str;
    if (host && host_len > 0) {
        if (host_part_len >= host_len)
            host_part_len = host_len - 1;
        memcpy(host, addr_str, host_part_len);
        host[host_part_len] = '\0';
    }
    if (port) {
        *port = atoi(colon + 1);
    }
    return 0;
}

int xr_net_format_addr(const XrNetAddr *addr, char *buf, size_t buf_len) {
    if (!addr || !buf || buf_len == 0)
        return -1;
    return snprintf(buf, buf_len, "%s:%d", addr->host, addr->port);
}

// ========== Script Bindings ==========

// ========== TLS fd-indexed storage ==========

#ifdef XR_ENABLE_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>

#define NET_FD_INIT_CAP 256

static XrTlsConn **g_tls_conns = NULL;
static int g_tls_conns_cap = 0;
static xr_mutex_t g_tls_conns_mutex = XR_MUTEX_INITIALIZER;
static XrTlsContext *g_tls_client_ctx = NULL;
static xr_once_t g_tls_client_once = XR_ONCE_INITIALIZER;

// Grow g_tls_conns array to hold fd. Caller must hold g_tls_conns_mutex.
static bool tls_fd_ensure_locked(int fd) {
    if (fd < 0)
        return false;
    if (fd < g_tls_conns_cap)
        return true;
    int new_cap = (g_tls_conns_cap == 0) ? NET_FD_INIT_CAP : g_tls_conns_cap;
    while (new_cap <= fd)
        new_cap *= 2;
    XrTlsConn **new_arr = (XrTlsConn **) xr_realloc(g_tls_conns, sizeof(XrTlsConn *) * new_cap);
    if (!new_arr)
        return false;
    memset(new_arr + g_tls_conns_cap, 0, sizeof(XrTlsConn *) * (new_cap - g_tls_conns_cap));
    g_tls_conns = new_arr;
    g_tls_conns_cap = new_cap;
    return true;
}

static void tls_client_ctx_init(void) {
    g_tls_client_ctx = xr_tls_context_new_client();
}

static XrTlsContext *get_tls_client_ctx(void) {
    xr_once_call(&g_tls_client_once, tls_client_ctx_init);
    return g_tls_client_ctx;
}

static XrTlsConn *get_tls_conn(int fd) {
    xr_mutex_lock(&g_tls_conns_mutex);
    XrTlsConn *tls = (fd >= 0 && fd < g_tls_conns_cap) ? g_tls_conns[fd] : NULL;
    xr_mutex_unlock(&g_tls_conns_mutex);
    return tls;
}

static bool set_tls_conn(int fd, XrTlsConn *tls) {
    xr_mutex_lock(&g_tls_conns_mutex);
    bool ok = tls_fd_ensure_locked(fd);
    if (ok)
        g_tls_conns[fd] = tls;
    xr_mutex_unlock(&g_tls_conns_mutex);
    return ok;
}
#endif

// ========== Forward declarations for UDP buffers ==========

static XR_THREAD_LOCAL char g_udp_recv_buf[65536];
static XR_THREAD_LOCAL XrNetAddr g_udp_recv_addr;

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

static uint8_t net_error_from_errno(int err) {
    switch (err) {
        case ETIMEDOUT:
            return XR_NETERR_TIMEOUT;
        case ECONNRESET:
            return XR_NETERR_RESET;
        case ECONNREFUSED:
            return XR_NETERR_REFUSED;
        case EBADF:
        case ENOTCONN:
            return XR_NETERR_CLOSED;
#ifdef EPIPE
        case EPIPE:
            return XR_NETERR_RESET;
#endif
        default:
            return XR_NETERR_IO;
    }
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

static const char *net_error_name(uint8_t kind) {
    switch (kind) {
        case XR_NETERR_TIMEOUT:
            return "timeout";
        case XR_NETERR_CLOSED:
            return "closed";
        case XR_NETERR_RESET:
            return "reset";
        case XR_NETERR_REFUSED:
            return "refused";
        case XR_NETERR_DNS:
            return "dns";
        case XR_NETERR_TLS:
            return "tls";
        case XR_NETERR_IO:
            return "io";
        case XR_NETERR_INVALID:
            return "invalid";
        default:
            return NULL;
    }
}

static XrValue make_conn_handle(XrayIsolate *X, int fd, bool is_tls) {
    XrNetConnKind kind = is_tls ? XR_NETCONN_TLS : XR_NETCONN_TCP;
    XrNetConn *c = xr_net_conn_new(X, fd, kind);
    if (!c)
        return XR_NULL_VAL;
#ifdef XR_ENABLE_TLS
    if (is_tls) {
        /*
         * Transfer ownership of the in-flight XrTlsConn from the
         * fd-indexed g_tls_conns table into the typed handle. The
         * handle is the sole owner from now on; xr_net_conn_close
         * (or the GC destroy hook) will free the XrTlsConn so the
         * legacy table no longer needs an entry for this fd.
         */
        XrTlsConn *tls = get_tls_conn(fd);
        if (tls) {
            xr_net_conn_set_tls(c, tls);
            set_tls_conn(fd, NULL);
        }
    }
#endif
    return XR_FROM_PTR(c);
}

static XrValue make_listener_handle(XrayIsolate *X, int fd, int port_num) {
    XrNetListener *l = xr_net_listener_new(X, fd, port_num);
    if (!l)
        return XR_NULL_VAL;
    return XR_FROM_PTR(l);
}

static XrValue make_udp_handle(XrayIsolate *X, int fd) {
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
static int handle_get_fd(XrayIsolate *X, XrValue handle) {
    (void) X;
    XrNetConn *c = unwrap_conn(handle);
    if (c)
        return c->closed ? -1 : c->fd;
    XrNetListener *l = unwrap_listener(handle);
    if (l)
        return l->closed ? -1 : l->fd;
    return -1;
}

// ========== Handle-based API Functions ==========

// ========== Yieldable net.dial (handle-based) ==========

typedef struct {
    int fd;
    int phase;  // 0=connect_start done, waiting for writable; 1=connect_finish
} NetDialState;

static XrCFuncResult net_dial_step(XrayIsolate *X, NetDialState *state, XrValue *result);

static XrCFuncResult net_dial_continue(XrayIsolate *X, int status, XrValue resume_value, void *ctx,
                                       XrValue *result) {
    NetDialState *state = (NetDialState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        net_close_fd(X, state->fd);
        xr_free(state);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    return net_dial_step(X, state, result);
}

static XrCFuncResult net_dial_step(XrayIsolate *X, NetDialState *state, XrValue *result) {
    // Check the connect result.
    int error = 0;
    socklen_t elen = sizeof(error);
    if (getsockopt(state->fd, SOL_SOCKET, SO_ERROR, &error, &elen) < 0 || error != 0) {
        net_close_fd(X, state->fd);
        xr_free(state);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    // Success - create Json handle
    int fd = state->fd;
    xr_free(state);
    *result = make_conn_handle(X, fd, false);
    return XR_CFUNC_DONE;
}

/*
 * net.dial(host, port, timeout?) -> Json handle | null
 * Yieldable: non-blocking connect + waitIO
 */
static XrCFuncResult net_dial_yieldable(XrayIsolate *X, XrValue *args, int nargs, XrValue *result) {
    if (nargs < 2 || !XR_IS_STRING(args[0]) || !XR_IS_INT(args[1])) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }

    XrString *host = XR_TO_STRING(args[0]);
    int port_num = (int) XR_TO_INT(args[1]);

    int conn_ret;
    int fd = net_tcp_connect(X, XR_STRING_CHARS(host), port_num, &conn_ret);
    if (fd < 0) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }

    if (conn_ret == 0) {
        *result = make_conn_handle(X, fd, false);
        return XR_CFUNC_DONE;
    }

    // EINPROGRESS: yield for write, then check connect result
    NetDialState *state = (NetDialState *) xr_malloc(sizeof(NetDialState));
    if (!state) {
        close(fd);
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }
    state->fd = fd;
    state->phase = 1;

    int timeout_ms = (nargs > 2 && XR_IS_INT(args[2])) ? (int) XR_TO_INT(args[2]) : 30000;
    return xr_yield_for_io(X, fd, XR_WAIT_WRITE, timeout_ms, net_dial_continue, state, result);
}

// ========== Yieldable net.accept (handle-based) ==========

typedef struct {
    int listen_fd;
    XrNetListener *listener;
} NetAcceptState;

static XrCFuncResult net_accept_step(XrayIsolate *X, NetAcceptState *state, XrValue *result);

static XrCFuncResult net_accept_continue(XrayIsolate *X, int status, XrValue resume_value,
                                         void *ctx, XrValue *result) {
    NetAcceptState *state = (NetAcceptState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        net_listener_set_error(
            state->listener, status == XR_RESUME_TIMEOUT ? XR_NETERR_TIMEOUT : XR_NETERR_CLOSED, 0);
        xr_free(state);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    return net_accept_step(X, state, result);
}

static XrCFuncResult net_accept_step(XrayIsolate *X, NetAcceptState *state, XrValue *result) {
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
        *result = make_conn_handle(X, client_fd, false);
        return XR_CFUNC_DONE;
    }
    // EAGAIN - yield for read
    return xr_yield_for_io(
        X, state->listen_fd, XR_WAIT_READ,
        net_timeout_until(state->listener ? state->listener->accept_deadline_ms : 0),
        net_accept_continue, state, result);
}

/*
 * net.accept(listener_handle) -> Json handle | null
 * Yieldable: loop acceptFast + yield
 */
static XrCFuncResult net_accept_handle_yieldable(XrayIsolate *X, XrValue *args, int nargs,
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
        *result = make_conn_handle(X, r.value, false);
        return XR_CFUNC_DONE;
    }

    NetAcceptState *state = (NetAcceptState *) xr_malloc(sizeof(NetAcceptState));
    if (!state) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }
    state->listen_fd = listener->fd;
    state->listener = listener;
    return net_accept_step(X, state, result);
}

// ========== Handle-based net.read (yieldable, TCP + TLS dispatch) ==========

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

typedef struct {
    int fd;
    XrNetConn *conn;
    char *buf;  // Points to coroutine's io_buf (not owned)
    size_t max_len;
    bool is_tls;
} NetReadHandleState;

static XrCFuncResult net_read_handle_step(XrayIsolate *X, NetReadHandleState *state,
                                          XrValue *result);

static XrCFuncResult net_read_handle_continue(XrayIsolate *X, int status, XrValue resume_value,
                                              void *ctx, XrValue *result) {
    NetReadHandleState *state = (NetReadHandleState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        net_conn_set_error(state->conn,
                           status == XR_RESUME_TIMEOUT ? XR_NETERR_TIMEOUT : XR_NETERR_CLOSED, 0);
        xr_free(state);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    return net_read_handle_step(X, state, result);
}

static XrCFuncResult net_read_handle_step(XrayIsolate *X, NetReadHandleState *state,
                                          XrValue *result) {
#ifdef XR_ENABLE_TLS
    if (state->is_tls) {
        XrTlsConn *tls = state->conn ? (XrTlsConn *) state->conn->tls_state : NULL;
        if (!tls) {
            net_conn_set_error(state->conn, XR_NETERR_TLS, 0);
            xr_free(state);
            *result = XR_NULL_VAL;
            return XR_CFUNC_DONE;
        }
        int n = xr_tls_conn_read_try(tls, state->buf, (int) state->max_len);
        if (n > 0) {
            net_conn_clear_error(state->conn);
            *result = xr_string_value(xr_string_new(X, state->buf, n));
            xr_free(state);
            return XR_CFUNC_DONE;
        }
        if (n == 0) {
            net_conn_clear_error(state->conn);
            xr_free(state);
            *result = XR_NULL_VAL;
            return XR_CFUNC_DONE;
        }
        if (n == -3) {
            net_conn_set_error(state->conn, XR_NETERR_TLS, 0);
            xr_free(state);
            *result = XR_NULL_VAL;
            return XR_CFUNC_DONE;
        }
        // -1=WANT_READ, -2=WANT_WRITE
        int wait_mode = (n == -1) ? XR_WAIT_READ : XR_WAIT_WRITE;
        int64_t deadline = (wait_mode == XR_WAIT_READ && state->conn)
                               ? state->conn->read_deadline_ms
                               : (state->conn ? state->conn->write_deadline_ms : 0);
        return xr_yield_for_io(X, state->fd, wait_mode, net_timeout_until(deadline),
                               net_read_handle_continue, state, result);
    }
#endif

    // TCP read
    ssize_t n = read(state->fd, state->buf, state->max_len);
    if (n > 0) {
        net_conn_clear_error(state->conn);
        *result = xr_string_value(xr_string_new(X, state->buf, n));
        xr_free(state);
        return XR_CFUNC_DONE;
    }
    if (n == 0) {
        net_conn_clear_error(state->conn);
        xr_free(state);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return xr_yield_for_io(X, state->fd, XR_WAIT_READ,
                               net_timeout_until(state->conn ? state->conn->read_deadline_ms : 0),
                               net_read_handle_continue, state, result);
    }
    net_conn_set_error(state->conn, net_error_from_errno(errno), errno);
    xr_free(state);
    *result = XR_NULL_VAL;
    return XR_CFUNC_DONE;
}

/*
 * net.read(conn_handle, maxlen?) -> string | null
 * Yieldable: handle-based, dispatches TCP/TLS
 */
static XrCFuncResult net_read_handle_yieldable(XrayIsolate *X, XrValue *args, int nargs,
                                               XrValue *result) {
    if (nargs < 1) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }

    XrNetConn *conn = unwrap_conn(args[0]);
    if (!conn || conn->closed || conn->fd < 0) {
        net_conn_set_error(conn, XR_NETERR_CLOSED, 0);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }

    int fd = conn->fd;
    bool is_tls = conn->kind == XR_NETCONN_TLS;
    int max_len = (nargs >= 2 && XR_IS_INT(args[1])) ? (int) XR_TO_INT(args[1]) : 4096;
    if (max_len > 1048576)
        max_len = 1048576;
    if (max_len <= 0) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }

    // Use per-coroutine reusable I/O buffer (avoid malloc/free per read)
    XrCoroutine *coro = xr_current_coro(X);
    char *buf = coro ? xr_coro_ensure_io_buf(coro, max_len) : NULL;
    if (!buf) {
        // Fallback to malloc if no coroutine context
        buf = (char *) xr_malloc(max_len);
    }
    if (!buf) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }

    NetReadHandleState *state = (NetReadHandleState *) xr_malloc(sizeof(NetReadHandleState));
    if (!state) {
        if (!coro)
            xr_free(buf);
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }
    state->fd = fd;
    state->conn = conn;
    state->max_len = max_len;
    state->is_tls = is_tls;
    state->buf = buf;
    return net_read_handle_step(X, state, result);
}

// ========== Handle-based net.readInto (yieldable, TCP + TLS dispatch) ==========

typedef struct {
    int fd;
    XrNetConn *conn;
    XrArray *buf;  // Bytes buffer supplied by caller (not owned)
    size_t max_len;
    bool is_tls;
} NetReadIntoState;

static XrCFuncResult net_read_into_step(XrayIsolate *X, NetReadIntoState *state, XrValue *result);

static XrCFuncResult net_read_into_continue(XrayIsolate *X, int status, XrValue resume_value,
                                            void *ctx, XrValue *result) {
    (void) resume_value;
    NetReadIntoState *state = (NetReadIntoState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        net_conn_set_error(state->conn,
                           status == XR_RESUME_TIMEOUT ? XR_NETERR_TIMEOUT : XR_NETERR_CLOSED, 0);
        xr_free(state);
        *result = xr_int(-1);
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

static XrCFuncResult net_read_into_wait(XrayIsolate *X, XrNetConn *conn, XrArray *buf,
                                        size_t max_len, bool is_tls, int wait_mode,
                                        int64_t deadline_ms, XrValue *result) {
    NetReadIntoState *state = (NetReadIntoState *) xr_malloc(sizeof(NetReadIntoState));
    if (!state) {
        *result = xr_int(-1);
        return XR_CFUNC_ERROR;
    }
    state->fd = conn->fd;
    state->conn = conn;
    state->buf = buf;
    state->max_len = max_len;
    state->is_tls = is_tls;
    return xr_yield_for_io(X, state->fd, wait_mode, net_timeout_until(deadline_ms),
                           net_read_into_continue, state, result);
}

static XrCFuncResult net_read_into_step(XrayIsolate *X, NetReadIntoState *state, XrValue *result) {
    uint8_t *data = xr_array_raw_u8(state->buf);
#ifdef XR_ENABLE_TLS
    if (state->is_tls) {
        XrTlsConn *tls = state->conn ? (XrTlsConn *) state->conn->tls_state : NULL;
        if (!tls) {
            net_conn_set_error(state->conn, XR_NETERR_TLS, 0);
            return net_read_into_done(state, -1, result);
        }
        int n = xr_tls_conn_read_try(tls, (char *) data, (int) state->max_len);
        if (n >= 0)
            return net_read_into_done(state, n, result);
        if (n == -3) {
            net_conn_set_error(state->conn, XR_NETERR_TLS, 0);
            return net_read_into_done(state, -1, result);
        }
        int wait_mode = (n == -1) ? XR_WAIT_READ : XR_WAIT_WRITE;
        int64_t deadline = (wait_mode == XR_WAIT_READ && state->conn)
                               ? state->conn->read_deadline_ms
                               : (state->conn ? state->conn->write_deadline_ms : 0);
        return xr_yield_for_io(X, state->fd, wait_mode, net_timeout_until(deadline),
                               net_read_into_continue, state, result);
    }
#endif

    ssize_t n = read(state->fd, data, state->max_len);
    if (n >= 0)
        return net_read_into_done(state, n, result);
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return xr_yield_for_io(X, state->fd, XR_WAIT_READ,
                               net_timeout_until(state->conn ? state->conn->read_deadline_ms : 0),
                               net_read_into_continue, state, result);
    }
    net_conn_set_error(state->conn, net_error_from_errno(errno), errno);
    return net_read_into_done(state, -1, result);
}

/*
 * net.readInto(conn_handle, buffer, maxlen?) -> int
 * Read into a reusable Bytes buffer. EOF returns 0; errors return -1.
 */
static XrCFuncResult net_read_into_yieldable(XrayIsolate *X, XrValue *args, int nargs,
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

    XrArray *buf = net_as_writable_bytes(args[1]);
    if (!buf) {
        *result = xr_int(-1);
        return XR_CFUNC_ERROR;
    }

    int max_len = (nargs >= 3 && XR_IS_INT(args[2])) ? (int) XR_TO_INT(args[2]) : buf->capacity;
    if (max_len > buf->capacity)
        max_len = buf->capacity;
    if (max_len <= 0) {
        *result = xr_int(-1);
        return XR_CFUNC_ERROR;
    }

    size_t read_len = (size_t) max_len;
    uint8_t *data = xr_array_raw_u8(buf);
    bool is_tls = conn->kind == XR_NETCONN_TLS;

#ifdef XR_ENABLE_TLS
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
        return net_read_into_wait(X, conn, buf, read_len, true, wait_mode, deadline, result);
    }
#endif

    ssize_t n = read(conn->fd, data, read_len);
    if (n >= 0)
        return net_read_into_value(conn, buf, n, result);
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return net_read_into_wait(X, conn, buf, read_len, false, XR_WAIT_READ,
                                  conn->read_deadline_ms, result);
    }
    net_conn_set_error(conn, net_error_from_errno(errno), errno);
    return net_read_into_value(conn, buf, -1, result);
}

// ========== Handle-based net.write (yieldable, TCP + TLS dispatch) ==========

typedef struct {
    int fd;
    XrNetConn *conn;
    const char *data;  // Points directly into XrString/Bytes storage (not owned)
    size_t len;
    size_t written;
    bool is_tls;
} NetWriteHandleState;

static XrCFuncResult net_write_handle_step(XrayIsolate *X, NetWriteHandleState *state,
                                           XrValue *result);

static XrCFuncResult net_write_handle_continue(XrayIsolate *X, int status, XrValue resume_value,
                                               void *ctx, XrValue *result) {
    NetWriteHandleState *state = (NetWriteHandleState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        net_conn_set_error(state->conn,
                           status == XR_RESUME_TIMEOUT ? XR_NETERR_TIMEOUT : XR_NETERR_CLOSED, 0);
        int total = state->written > 0 ? (int) state->written : -1;
        xr_free(state);
        *result = XR_FROM_INT(total);
        return XR_CFUNC_DONE;
    }
    return net_write_handle_step(X, state, result);
}

static XrCFuncResult net_write_handle_wait(XrayIsolate *X, XrNetConn *conn, const char *data,
                                           size_t len, size_t written, bool is_tls, int wait_mode,
                                           int64_t deadline_ms, XrValue *result) {
    NetWriteHandleState *state = (NetWriteHandleState *) xr_calloc(1, sizeof(NetWriteHandleState));
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
                           net_write_handle_continue, state, result);
}

static XrCFuncResult net_write_handle_step(XrayIsolate *X, NetWriteHandleState *state,
                                           XrValue *result) {
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
                                   net_write_handle_continue, state, result);
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
        ssize_t n = write(state->fd, state->data + state->written, state->len - state->written);
        if (n > 0) {
            state->written += n;
            continue;
        }
        if (n == 0)
            break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return xr_yield_for_io(
                X, state->fd, XR_WAIT_WRITE,
                net_timeout_until(state->conn ? state->conn->write_deadline_ms : 0),
                net_write_handle_continue, state, result);
        }
        net_conn_set_error(state->conn, net_error_from_errno(errno), errno);
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
 * net.write(conn_handle, data) -> int
 * Yieldable: handle-based, dispatches TCP/TLS
 */
static XrCFuncResult net_write_handle_yieldable(XrayIsolate *X, XrValue *args, int nargs,
                                                XrValue *result) {
    if (nargs < 2 || !XR_IS_STRING(args[1])) {
        *result = XR_FROM_INT(-1);
        return XR_CFUNC_ERROR;
    }

    XrNetConn *conn = unwrap_conn(args[0]);
    if (!conn || conn->closed || conn->fd < 0) {
        net_conn_set_error(conn, XR_NETERR_CLOSED, 0);
        *result = XR_FROM_INT(-1);
        return XR_CFUNC_DONE;
    }

    bool is_tls = conn->kind == XR_NETCONN_TLS;
    XrString *data = XR_TO_STRING(args[1]);

    if (data->length == 0) {
        net_conn_clear_error(conn);
        *result = XR_FROM_INT(0);
        return XR_CFUNC_DONE;
    }

    NetWriteHandleState *state = (NetWriteHandleState *) xr_calloc(1, sizeof(NetWriteHandleState));
    if (!state) {
        *result = XR_FROM_INT(-1);
        return XR_CFUNC_ERROR;
    }
    state->fd = conn->fd;
    state->conn = conn;
    state->is_tls = is_tls;
    state->len = data->length;
    state->written = 0;
    // Zero-copy: XrString is immutable and coroutine arena GC
    // doesn't run while yielded, so direct reference is safe
    state->data = XR_STRING_CHARS(data);

    return net_write_handle_step(X, state, result);
}

/*
 * net.writeBytes(conn_handle, data) -> int
 * Yieldable: handle-based, dispatches TCP/TLS, sends Bytes or Bytes slice.
 */
static XrCFuncResult net_write_bytes_yieldable(XrayIsolate *X, XrValue *args, int nargs,
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
    const char *raw = (const char *) xr_array_raw_u8(data);
    bool is_tls = conn->kind == XR_NETCONN_TLS;

#ifdef XR_ENABLE_TLS
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
            return net_write_handle_wait(X, conn, raw, len, written, true, wait_mode, deadline,
                                         result);
        }
        net_conn_clear_error(conn);
        *result = xr_int((xr_Integer) written);
        return XR_CFUNC_DONE;
    }
#endif

    while (written < len) {
        ssize_t n = write(conn->fd, raw + written, len - written);
        if (n > 0) {
            written += (size_t) n;
            continue;
        }
        if (n == 0)
            break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return net_write_handle_wait(X, conn, raw, len, written, false, XR_WAIT_WRITE,
                                         conn->write_deadline_ms, result);
        }
        net_conn_set_error(conn, net_error_from_errno(errno), errno);
        break;
    }

    if (written == len) {
        net_conn_clear_error(conn);
    }
    *result = xr_int(written > 0 ? (xr_Integer) written : -1);
    return XR_CFUNC_DONE;
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
    _Atomic bool ok;
    _Atomic int64_t ab;
    _Atomic int64_t ba;
} NetBidiShared;

static void net_copy_notify(NetCopyState *state, int64_t bytes, bool ok) {
    if (!state || !state->notify_shared)
        return;
    if (!ok)
        atomic_store_explicit(&state->notify_shared->ok, false, memory_order_release);
    if (state->notify_dir == 0)
        atomic_store_explicit(&state->notify_shared->ab, bytes, memory_order_release);
    else
        atomic_store_explicit(&state->notify_shared->ba, bytes, memory_order_release);
    atomic_fetch_add_explicit(&state->notify_shared->done, 1, memory_order_acq_rel);
}

static void net_copy_state_free(NetCopyState *state) {
    if (!state)
        return;
    if (state->owns_buf)
        xr_free(state->buf);
    xr_free(state);
}

static XrCFuncResult net_copy_step(XrayIsolate *X, NetCopyState *state, XrValue *result);

static XrCFuncResult net_copy_continue(XrayIsolate *X, int status, XrValue resume_value, void *ctx,
                                       XrValue *result) {
    (void) resume_value;
    NetCopyState *state = (NetCopyState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        net_conn_set_error(state->waiting_conn,
                           status == XR_RESUME_TIMEOUT ? XR_NETERR_TIMEOUT : XR_NETERR_CLOSED, 0);
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

static XrCFuncResult net_copy_step(XrayIsolate *X, NetCopyState *state, XrValue *result) {
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
            ssize_t n = write(state->dst_fd, state->buf + state->off, state->len - state->off);
            if (n > 0) {
                state->off += (size_t) n;
                continue;
            }
            if (n == 0)
                return net_copy_error(state, result);
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                state->waiting_conn = state->dst_conn;
                return xr_yield_for_io(X, state->dst_fd, XR_WAIT_WRITE,
                                       net_timeout_until(state->dst_conn->write_deadline_ms),
                                       net_copy_continue, state, result);
            }
            net_conn_set_error(state->dst_conn, net_error_from_errno(errno), errno);
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
        ssize_t n = read(state->src_fd, state->buf, state->cap);
        if (n > 0) {
            state->len = (size_t) n;
            state->total += n;
            continue;
        }
        if (n == 0)
            return net_copy_finish(state, result);
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            state->waiting_conn = state->src_conn;
            return xr_yield_for_io(X, state->src_fd, XR_WAIT_READ,
                                   net_timeout_until(state->src_conn->read_deadline_ms),
                                   net_copy_continue, state, result);
        }
        net_conn_set_error(state->src_conn, net_error_from_errno(errno), errno);
        return net_copy_error(state, result);
    }
}

static XrCFuncResult net_copy_start_ex(XrayIsolate *X, XrValue src, XrValue dst, int buffer_size,
                                       NetBidiShared *notify_shared, int notify_dir,
                                       bool shutdown_dst_write_on_finish, XrValue *result) {
    XrNetConn *src_conn = unwrap_conn(src);
    XrNetConn *dst_conn = unwrap_conn(dst);
    if (!src_conn || src_conn->closed || src_conn->fd < 0 || !dst_conn || dst_conn->closed ||
        dst_conn->fd < 0) {
        net_conn_set_error(src_conn, XR_NETERR_CLOSED, 0);
        net_conn_set_error(dst_conn, XR_NETERR_CLOSED, 0);
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }

    if (buffer_size <= 0) {
        *result = xr_int(-1);
        return XR_CFUNC_ERROR;
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
        *result = xr_int(-1);
        return XR_CFUNC_ERROR;
    }

    NetCopyState *state = (NetCopyState *) xr_calloc(1, sizeof(NetCopyState));
    if (!state) {
        if (owns_buf)
            xr_free(buf);
        *result = xr_int(-1);
        return XR_CFUNC_ERROR;
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

static XrCFuncResult net_copy_start(XrayIsolate *X, XrValue src, XrValue dst, int buffer_size,
                                    XrValue *result) {
    return net_copy_start_ex(X, src, dst, buffer_size, NULL, 0, false, result);
}

/*
 * net.copy(src, dst, bufferSize?) -> int
 * Native TCP/TLS stream pump. Payload stays in a reusable C buffer.
 */
static XrCFuncResult net_copy_yieldable(XrayIsolate *X, XrValue *args, int nargs, XrValue *result) {
    if (nargs < 2) {
        *result = xr_int(-1);
        return XR_CFUNC_ERROR;
    }
    int buffer_size = (nargs >= 3 && XR_IS_INT(args[2])) ? (int) XR_TO_INT(args[2]) : 65536;
    return net_copy_start(X, args[0], args[1], buffer_size, result);
}

// ========== TCP half-close and bidirectional copy ==========

static XrValue net_shutdown_mode(XrayIsolate *X, XrValue *args, int nargs, int mode) {
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

static XrValue net_shutdown_read(XrayIsolate *X, XrValue *args, int nargs) {
    return net_shutdown_mode(X, args, nargs, XR_SHUT_RD);
}

static XrValue net_shutdown_write(XrayIsolate *X, XrValue *args, int nargs) {
    return net_shutdown_mode(X, args, nargs, XR_SHUT_WR);
}

static XrValue net_shutdown_conn(XrayIsolate *X, XrValue *args, int nargs) {
    return net_shutdown_mode(X, args, nargs, XR_SHUT_RDWR);
}

typedef struct {
    NetBidiShared *shared;
} NetBidiWaitState;

static XrCFuncResult net_bidi_wait_step(XrayIsolate *X, NetBidiWaitState *state, XrValue *result);

static XrCFuncResult net_bidi_wait_continue(XrayIsolate *X, int status, XrValue resume_value,
                                            void *ctx, XrValue *result) {
    (void) resume_value;
    NetBidiWaitState *state = (NetBidiWaitState *) ctx;
    if (status == XR_RESUME_CANCELLED) {
        xr_free(state->shared);
        xr_free(state);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    return net_bidi_wait_step(X, state, result);
}

static XrValue net_bidi_result_json(XrayIsolate *X, NetBidiWaitState *state) {
    XrJson *json = xr_json_new(xr_current_coro(X));
    if (!json)
        return XR_NULL_VAL;
    xr_json_set_by_key(
        X, json, "ab",
        xr_int((xr_Integer) atomic_load_explicit(&state->shared->ab, memory_order_acquire)));
    xr_json_set_by_key(
        X, json, "ba",
        xr_int((xr_Integer) atomic_load_explicit(&state->shared->ba, memory_order_acquire)));
    xr_json_set_by_key(X, json, "ok",
                       xr_bool(atomic_load_explicit(&state->shared->ok, memory_order_acquire)));
    return xr_json_value(json);
}

static XrCFuncResult net_bidi_wait_step(XrayIsolate *X, NetBidiWaitState *state, XrValue *result) {
    if (atomic_load_explicit(&state->shared->done, memory_order_acquire) >= 2) {
        *result = net_bidi_result_json(X, state);
        xr_free(state->shared);
        xr_free(state);
        return XR_CFUNC_DONE;
    }

    return xr_yield_for_timeout(X, 1, net_bidi_wait_continue, state, result);
}

static XrCFuncResult net_copy_direction_coro(XrayIsolate *X, XrValue *args, int nargs,
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
 * net.copyBidirectional(a, b) -> Json
 * Runs two native stream pumps in opposite directions. Each EOF half-closes
 * the peer write side so request/response style TCP protocols can drain.
 */
static XrCFuncResult net_copy_bidirectional_yieldable(XrayIsolate *X, XrValue *args, int nargs,
                                                      XrValue *result) {
    if (nargs < 2) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }
    XrNetConn *a = unwrap_conn(args[0]);
    XrNetConn *b = unwrap_conn(args[1]);
    if (!a || a->closed || a->fd < 0 || !b || b->closed || b->fd < 0) {
        net_conn_set_error(a, XR_NETERR_CLOSED, 0);
        net_conn_set_error(b, XR_NETERR_CLOSED, 0);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }

    NetBidiShared *shared = (NetBidiShared *) xr_calloc(1, sizeof(NetBidiShared));
    if (!shared) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }
    atomic_init(&shared->done, 0);
    atomic_init(&shared->ok, true);
    atomic_init(&shared->ab, 0);
    atomic_init(&shared->ba, 0);

    XrValue shared_ptr = xr_int((xr_Integer) (intptr_t) shared);
    XrValue ab_args[4] = {args[0], args[1], shared_ptr, xr_int(0)};
    XrValue ba_args[4] = {args[1], args[0], shared_ptr, xr_int(1)};
    XrCoroutine *ab = xr_coro_create_cfunc(X, net_copy_direction_coro, ab_args, 4, "net.copy.ab");
    XrCoroutine *ba = xr_coro_create_cfunc(X, net_copy_direction_coro, ba_args, 4, "net.copy.ba");
    if (!ab || !ba) {
        xr_free(shared);
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }
    xr_coro_spawn(X, ab);
    xr_coro_spawn(X, ba);

    NetBidiWaitState *state = (NetBidiWaitState *) xr_calloc(1, sizeof(NetBidiWaitState));
    if (!state) {
        xr_free(shared);
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }
    state->shared = shared;
    return net_bidi_wait_step(X, state, result);
}

// ========== Handle-based net.listen ==========

/*
 * net.listen(port, backlog?) -> Json listener handle | null
 */
static XrValue net_listen_handle(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_INT(args[0]))
        return XR_NULL_VAL;

    int port_num = (int) XR_TO_INT(args[0]);
    int backlog = (nargs > 1 && XR_IS_INT(args[1])) ? (int) XR_TO_INT(args[1]) : 1024;

    int fd = xr_io_listen(NULL, port_num, backlog);
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

// ========== Handle-based net.close ==========

/*
 * net.close(handle) -> void
 * Close connection, listener, or UDP socket. Safe to call multiple times.
 */
static XrValue net_close_handle(XrayIsolate *X, XrValue *args, int nargs) {
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

// ========== Handle-based net.fd ==========

/*
 * net.fd(handle) -> int
 */
static XrValue net_fd_handle(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1)
        return xr_int(-1);
    return xr_int(handle_get_fd(X, args[0]));
}

// ========== Deadline and diagnostic error helpers ==========

static XrValue net_set_read_deadline(XrayIsolate *X, XrValue *args, int nargs) {
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

static XrValue net_set_write_deadline(XrayIsolate *X, XrValue *args, int nargs) {
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

static XrValue net_set_deadline(XrayIsolate *X, XrValue *args, int nargs) {
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

static XrValue net_set_accept_deadline(XrayIsolate *X, XrValue *args, int nargs) {
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

static XrValue net_last_error(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1)
        return XR_NULL_VAL;
    uint8_t kind = XR_NETERR_INVALID;
    XrNetConn *c = unwrap_conn(args[0]);
    if (c) {
        kind = c->last_error;
    } else {
        XrNetListener *l = unwrap_listener(args[0]);
        if (l)
            kind = l->last_error;
    }
    const char *name = net_error_name(kind);
    if (!name)
        return XR_NULL_VAL;
    return xr_string_value(xr_string_intern(X, name, strlen(name), 0));
}

static XrValue net_last_errno(XrayIsolate *X, XrValue *args, int nargs) {
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

// ========== net.hasTLS ==========

/*
 * net.hasTLS() -> bool
 */
static XrValue net_has_tls(XrayIsolate *X, XrValue *args, int nargs) {
    (void) X;
    (void) args;
    (void) nargs;
#ifdef XR_ENABLE_TLS
    return xr_bool(true);
#else
    return xr_bool(false);
#endif
}

// ========== Yieldable net.dialTLS (handle-based) ==========

#ifdef XR_ENABLE_TLS

typedef struct {
    int fd;
    int phase;  // 0=waiting tcp connect, 1=tls_wrap done, 2+=handshake loop
    int timeout_ms;
    char hostname[256];
} NetDialTLSState;

static XrCFuncResult net_dial_tls_step(XrayIsolate *X, NetDialTLSState *state, XrValue *result);

static void net_dial_tls_cleanup(XrayIsolate *X, NetDialTLSState *state) {
    if (state->fd >= 0) {
        // Close TLS if wrap was done
        if (state->phase >= 2) {
            XrTlsConn *tls = get_tls_conn(state->fd);
            if (tls) {
                xr_tls_conn_close(tls);
                xr_tls_conn_free(tls);
                set_tls_conn(state->fd, NULL);
            }
        }
        net_close_fd(X, state->fd);
    }
}

static XrCFuncResult net_dial_tls_continue(XrayIsolate *X, int status, XrValue resume_value,
                                           void *ctx, XrValue *result) {
    NetDialTLSState *state = (NetDialTLSState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        net_dial_tls_cleanup(X, state);
        xr_free(state);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    return net_dial_tls_step(X, state, result);
}

static XrCFuncResult net_dial_tls_step(XrayIsolate *X, NetDialTLSState *state, XrValue *result) {
    if (state->phase == 1) {
        // TCP connect finished - check result
        int error = 0;
        socklen_t elen = sizeof(error);
        if (getsockopt(state->fd, SOL_SOCKET, SO_ERROR, &error, &elen) < 0 || error != 0) {
            net_dial_tls_cleanup(X, state);
            xr_free(state);
            *result = XR_NULL_VAL;
            return XR_CFUNC_DONE;
        }
        // TCP connected - setup TLS
        XrTlsContext *ctx = get_tls_client_ctx();
        if (!ctx) {
            net_dial_tls_cleanup(X, state);
            xr_free(state);
            *result = XR_NULL_VAL;
            return XR_CFUNC_DONE;
        }
        XrTlsConn *tls = xr_tls_conn_new(ctx, state->fd);
        if (!tls) {
            net_dial_tls_cleanup(X, state);
            xr_free(state);
            *result = XR_NULL_VAL;
            return XR_CFUNC_DONE;
        }
        xr_tls_conn_set_hostname(tls, state->hostname);
        set_tls_conn(state->fd, tls);
        state->phase = 2;
        // Fall through to handshake
    }

    if (state->phase >= 2) {
        // TLS handshake loop
        XrTlsConn *tls = get_tls_conn(state->fd);
        if (!tls) {
            net_dial_tls_cleanup(X, state);
            xr_free(state);
            *result = XR_NULL_VAL;
            return XR_CFUNC_DONE;
        }
        int hs = xr_tls_conn_handshake_try(tls);
        if (hs == 0) {
            // Handshake complete
            int fd = state->fd;
            xr_free(state);
            *result = make_conn_handle(X, fd, true);
            return XR_CFUNC_DONE;
        }
        if (hs < 0) {
            // Error
            net_dial_tls_cleanup(X, state);
            xr_free(state);
            *result = XR_NULL_VAL;
            return XR_CFUNC_DONE;
        }
        // 1=WANT_READ, 2=WANT_WRITE
        int wait_mode = (hs == 1) ? XR_WAIT_READ : XR_WAIT_WRITE;
        return xr_yield_for_io(X, state->fd, wait_mode, state->timeout_ms, net_dial_tls_continue,
                               state, result);
    }

    // Should not reach here
    xr_free(state);
    *result = XR_NULL_VAL;
    return XR_CFUNC_DONE;
}

/*
 * net.dialTLS(host, port, timeout?) -> Json handle | null
 * Yieldable: TCP connect + TLS handshake
 */
static XrCFuncResult net_dial_tls_yieldable(XrayIsolate *X, XrValue *args, int nargs,
                                            XrValue *result) {
    if (nargs < 2 || !XR_IS_STRING(args[0]) || !XR_IS_INT(args[1])) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }

    XrString *host = XR_TO_STRING(args[0]);
    int port_num = (int) XR_TO_INT(args[1]);

    int conn_ret;
    int fd = net_tcp_connect(X, XR_STRING_CHARS(host), port_num, &conn_ret);
    if (fd < 0) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }

    NetDialTLSState *state = (NetDialTLSState *) xr_calloc(1, sizeof(NetDialTLSState));
    if (!state) {
        close(fd);
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }
    state->fd = fd;
    state->timeout_ms = (nargs > 2 && XR_IS_INT(args[2])) ? (int) XR_TO_INT(args[2]) : 30000;
    if (state->timeout_ms <= 0)
        state->timeout_ms = 30000;
    strncpy(state->hostname, XR_STRING_CHARS(host), sizeof(state->hostname) - 1);

    if (conn_ret == 0) {
        state->phase = 1;
        return net_dial_tls_step(X, state, result);
    }

    // EINPROGRESS: yield for write
    state->phase = 1;
    return xr_yield_for_io(X, fd, XR_WAIT_WRITE, state->timeout_ms, net_dial_tls_continue, state,
                           result);
}

// ========== Yieldable net.upgradeTLS (handle-based) ==========

typedef struct {
    int fd;
    XrValue handle;  // kept for updating tls flag on success
    int timeout_ms;
} NetUpgradeTLSState;

static XrCFuncResult net_upgrade_tls_step(XrayIsolate *X, NetUpgradeTLSState *state,
                                          XrValue *result);

static XrCFuncResult net_upgrade_tls_continue(XrayIsolate *X, int status, XrValue resume_value,
                                              void *ctx, XrValue *result) {
    NetUpgradeTLSState *state = (NetUpgradeTLSState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        // Cleanup TLS
        XrTlsConn *tls = get_tls_conn(state->fd);
        if (tls) {
            xr_tls_conn_close(tls);
            xr_tls_conn_free(tls);
            set_tls_conn(state->fd, NULL);
        }
        XrNetConn *conn = unwrap_conn(state->handle);
        net_conn_set_error(conn, status == XR_RESUME_TIMEOUT ? XR_NETERR_TIMEOUT : XR_NETERR_CLOSED,
                           0);
        xr_free(state);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    return net_upgrade_tls_step(X, state, result);
}

static XrCFuncResult net_upgrade_tls_step(XrayIsolate *X, NetUpgradeTLSState *state,
                                          XrValue *result) {
    XrTlsConn *tls = get_tls_conn(state->fd);
    if (!tls) {
        xr_free(state);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    int hs = xr_tls_conn_handshake_try(tls);
    if (hs == 0) {
        /*
         * Handshake done: promote the existing conn to TLS in-place
         * so the script keeps its handle reference intact. Ownership
         * of the XrTlsConn moves from the legacy g_tls_conns slot
         * into the typed handle.
         */
        XrNetConn *conn = unwrap_conn(state->handle);
        if (conn) {
            xr_net_conn_set_tls(conn, tls);
            set_tls_conn(state->fd, NULL);
        }
        XrValue h = state->handle;
        xr_free(state);
        *result = h;
        return XR_CFUNC_DONE;
    }
    if (hs < 0) {
        xr_tls_conn_close(tls);
        xr_tls_conn_free(tls);
        set_tls_conn(state->fd, NULL);
        xr_free(state);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    int wait_mode = (hs == 1) ? XR_WAIT_READ : XR_WAIT_WRITE;
    return xr_yield_for_io(X, state->fd, wait_mode, state->timeout_ms, net_upgrade_tls_continue,
                           state, result);
}

/*
 * net.upgradeTLS(conn_handle, hostname, timeout?) -> conn_handle | null
 * Yieldable: TLS wrap + handshake on existing TCP connection
 */
static XrCFuncResult net_upgrade_tls_yieldable(XrayIsolate *X, XrValue *args, int nargs,
                                               XrValue *result) {
    if (nargs < 2 || !XR_IS_STRING(args[1])) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }

    int fd = handle_get_fd(X, args[0]);
    if (fd < 0) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }

    XrString *hostname = XR_TO_STRING(args[1]);
    XrTlsContext *ctx = get_tls_client_ctx();
    if (!ctx) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }

    XrTlsConn *tls = xr_tls_conn_new(ctx, fd);
    if (!tls) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    xr_tls_conn_set_hostname(tls, XR_STRING_CHARS(hostname));
    set_tls_conn(fd, tls);

    NetUpgradeTLSState *state = (NetUpgradeTLSState *) xr_malloc(sizeof(NetUpgradeTLSState));
    if (!state) {
        xr_tls_conn_close(tls);
        xr_tls_conn_free(tls);
        set_tls_conn(fd, NULL);
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }
    state->fd = fd;
    state->handle = args[0];
    state->timeout_ms = (nargs > 2 && XR_IS_INT(args[2])) ? (int) XR_TO_INT(args[2]) : 30000;
    if (state->timeout_ms <= 0)
        state->timeout_ms = 30000;

    return net_upgrade_tls_step(X, state, result);
}

#endif  // XR_ENABLE_TLS

// ========== Handle-based UDP functions ==========

/*
 * net.udpBind(port, addr?) -> Json handle | null
 */
static XrValue net_udp_bind_handle(XrayIsolate *X, XrValue *args, int nargs) {
    int port_num = (nargs >= 1 && XR_IS_INT(args[0])) ? (int) XR_TO_INT(args[0]) : 0;
    const char *addr = NULL;
    if (nargs >= 2 && XR_IS_STRING(args[1]))
        addr = XR_STRING_CHARS(XR_TO_STRING(args[1]));

    // Detect IPv6 and create socket directly (no XrUdpConn overhead)
    sa_family_t family = AF_INET;
    if (addr && strchr(addr, ':'))
        family = AF_INET6;

    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0)
        return XR_NULL_VAL;

    if (family == AF_INET) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port_num);
        if (addr && addr[0])
            inet_pton(AF_INET, addr, &sa.sin_addr);
        else
            sa.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
            close(fd);
            return XR_NULL_VAL;
        }
    } else {
        struct sockaddr_in6 sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin6_family = AF_INET6;
        sa.sin6_port = htons(port_num);
        if (addr && addr[0])
            inet_pton(AF_INET6, addr, &sa.sin6_addr);
        else
            sa.sin6_addr = in6addr_any;
        if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
            close(fd);
            return XR_NULL_VAL;
        }
    }

    extern int xr_socket_set_nonblock(int fd);
    xr_socket_set_nonblock(fd);

    return make_udp_handle(X, fd);
}

// ========== Yieldable net.sendTo (handle-based) ==========

typedef struct {
    int fd;
    char *data;
    size_t len;
    struct sockaddr_storage addr;
    socklen_t addr_len;
} NetSendToState;

static XrCFuncResult net_send_to_step(XrayIsolate *X, NetSendToState *state, XrValue *result);

static XrCFuncResult net_send_to_continue(XrayIsolate *X, int status, XrValue resume_value,
                                          void *ctx, XrValue *result) {
    NetSendToState *state = (NetSendToState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        xr_free(state->data);
        xr_free(state);
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }
    return net_send_to_step(X, state, result);
}

static XrCFuncResult net_send_to_step(XrayIsolate *X, NetSendToState *state, XrValue *result) {
    (void) X;
    ssize_t n = sendto(state->fd, state->data, state->len, 0, (struct sockaddr *) &state->addr,
                       state->addr_len);
    if (n >= 0) {
        xr_free(state->data);
        xr_free(state);
        *result = xr_int((int) n);
        return XR_CFUNC_DONE;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return xr_yield_for_io(X, state->fd, XR_WAIT_WRITE, 5000, net_send_to_continue, state,
                               result);
    }
    xr_free(state->data);
    xr_free(state);
    *result = xr_int(-1);
    return XR_CFUNC_DONE;
}

/*
 * net.sendTo(handle, data, host, port) -> int
 * Yieldable: handle-based UDP send with EAGAIN retry
 */
static XrCFuncResult net_send_to_yieldable(XrayIsolate *X, XrValue *args, int nargs,
                                           XrValue *result) {
    if (nargs < 4 || !XR_IS_STRING(args[1]) || !XR_IS_STRING(args[2]) || !XR_IS_INT(args[3])) {
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }

    int fd = handle_get_fd(X, args[0]);
    if (fd < 0) {
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }

    XrString *data = XR_TO_STRING(args[1]);
    XrString *host = XR_TO_STRING(args[2]);
    int port_num = (int) XR_TO_INT(args[3]);

    // DNS resolve for dual-stack support
    XrSockAddr resolved;
    if (!xr_dns_resolve(X, XR_STRING_CHARS(host), &resolved, XR_AF_UNSPEC)) {
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }

    struct sockaddr_storage addr;
    socklen_t addr_len;
    memset(&addr, 0, sizeof(addr));
    if (resolved.family == AF_INET) {
        resolved.addr.v4.sin_port = htons(port_num);
        memcpy(&addr, &resolved.addr.v4, sizeof(struct sockaddr_in));
        addr_len = sizeof(struct sockaddr_in);
    } else {
        resolved.addr.v6.sin6_port = htons(port_num);
        memcpy(&addr, &resolved.addr.v6, sizeof(struct sockaddr_in6));
        addr_len = sizeof(struct sockaddr_in6);
    }

    // Try first
    ssize_t n =
        sendto(fd, XR_STRING_CHARS(data), data->length, 0, (struct sockaddr *) &addr, addr_len);
    if (n >= 0) {
        *result = xr_int((int) n);
        return XR_CFUNC_DONE;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }

    // EAGAIN - yield
    NetSendToState *state = (NetSendToState *) xr_malloc(sizeof(NetSendToState));
    if (!state) {
        *result = xr_int(-1);
        return XR_CFUNC_ERROR;
    }
    state->fd = fd;
    state->addr = addr;
    state->addr_len = addr_len;
    state->len = data->length;
    state->data = (char *) xr_malloc(data->length);
    if (!state->data) {
        xr_free(state);
        *result = xr_int(-1);
        return XR_CFUNC_ERROR;
    }
    memcpy(state->data, XR_STRING_CHARS(data), data->length);

    return xr_yield_for_io(X, fd, XR_WAIT_WRITE, 5000, net_send_to_continue, state, result);
}

// ========== Yieldable net.recvFrom (handle-based) ==========

typedef struct {
    int fd;
    int max_len;
} NetRecvFromState;

static XrCFuncResult net_recv_from_step(XrayIsolate *X, NetRecvFromState *state, XrValue *result);

static XrCFuncResult net_recv_from_continue(XrayIsolate *X, int status, XrValue resume_value,
                                            void *ctx, XrValue *result) {
    NetRecvFromState *state = (NetRecvFromState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        xr_free(state);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    return net_recv_from_step(X, state, result);
}

static XrCFuncResult net_recv_from_step(XrayIsolate *X, NetRecvFromState *state, XrValue *result) {
    int maxlen = state->max_len;
    if (maxlen > (int) sizeof(g_udp_recv_buf))
        maxlen = (int) sizeof(g_udp_recv_buf);

    struct sockaddr_storage saddr;
    socklen_t slen = sizeof(saddr);

    ssize_t n = recvfrom(state->fd, g_udp_recv_buf, maxlen, 0, (struct sockaddr *) &saddr, &slen);
    if (n >= 0) {
        // Store sender address
        memset(&g_udp_recv_addr, 0, sizeof(g_udp_recv_addr));
        if (saddr.ss_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *) &saddr;
            g_udp_recv_addr.family = XR_NET_IPV4;
            inet_ntop(AF_INET, &sin->sin_addr, g_udp_recv_addr.host, sizeof(g_udp_recv_addr.host));
            g_udp_recv_addr.port = ntohs(sin->sin_port);
        } else if (saddr.ss_family == AF_INET6) {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *) &saddr;
            g_udp_recv_addr.family = XR_NET_IPV6;
            inet_ntop(AF_INET6, &sin6->sin6_addr, g_udp_recv_addr.host,
                      sizeof(g_udp_recv_addr.host));
            g_udp_recv_addr.port = ntohs(sin6->sin6_port);
        }

        // Build UdpPacket handle: { data: string, host: string, port: int }
        // Flat layout — direct .field access instead of .addr.host / .addr.port.
        XrJson *json = xr_json_new(xr_current_coro(X));
        if (!json) {
            xr_free(state);
            *result = XR_NULL_VAL;
            return XR_CFUNC_DONE;
        }
        xr_json_set_by_key(X, json, "data",
                           xr_string_value(xr_string_intern(X, g_udp_recv_buf, n, 0)));
        xr_json_set_by_key(X, json, "host",
                           xr_string_value(xr_string_intern(X, g_udp_recv_addr.host,
                                                            strlen(g_udp_recv_addr.host), 0)));
        xr_json_set_by_key(X, json, "port", xr_int(g_udp_recv_addr.port));

        xr_free(state);
        *result = xr_json_value(json);
        return XR_CFUNC_DONE;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return xr_yield_for_io(X, state->fd, XR_WAIT_READ, -1, net_recv_from_continue, state,
                               result);
    }

    // Error
    xr_free(state);
    *result = XR_NULL_VAL;
    return XR_CFUNC_DONE;
}

/*
 * net.recvFrom(handle, maxlen?) -> { data, addr: { host, port } } | null
 * Yieldable: handle-based UDP receive
 */
static XrCFuncResult net_recv_from_yieldable(XrayIsolate *X, XrValue *args, int nargs,
                                             XrValue *result) {
    if (nargs < 1) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }

    int fd = handle_get_fd(X, args[0]);
    if (fd < 0) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }

    int maxlen = (nargs >= 2 && XR_IS_INT(args[1])) ? (int) XR_TO_INT(args[1]) : 4096;

    NetRecvFromState *state = (NetRecvFromState *) xr_malloc(sizeof(NetRecvFromState));
    if (!state) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }
    state->fd = fd;
    state->max_len = maxlen;

    return net_recv_from_step(X, state, result);
}

/*
 * net.lookup(hostname) -> string
 * DNS resolution
 */
static XrValue net_dns_lookup(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0])) {
        return XR_NULL_VAL;
    }

    XrString *hostname = XR_TO_STRING(args[0]);
    XrNetAddr addrs[8];
    int count = net_dns_lookup_to_addrs(isolate, XR_STRING_CHARS(hostname), addrs, 8);

    if (count <= 0)
        return XR_NULL_VAL;

    return xr_string_value(xr_string_intern(isolate, addrs[0].host, strlen(addrs[0].host), 0));
}

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module net
// @handle UdpPacket { const data: string, const host: string, const port: int }
//
// cfunc signatures use the typed prelude handle classes (NetConn /
// NetListener), with union types where the cfunc accepts either kind
// (close / fd / sendTo / recvFrom). The analyzer parses union types
// in cfunc signature strings, and Json <-> instance coercion is
// allowed at compile time with a runtime check, so user code can
// freely mix the typed and untyped surfaces.

XR_DEFINE_BUILTIN(net_dial_yieldable, "dial", "(host: string, port: int, timeout?: int): NetConn?",
                  "Dial a TCP connection")
XR_DEFINE_BUILTIN(net_listen_handle, "listen", "(port: int, backlog?: int): NetListener?",
                  "Start listening on a port")
XR_DEFINE_BUILTIN(net_accept_handle_yieldable, "accept", "(listener: NetListener): NetConn?",
                  "Accept a new connection")
XR_DEFINE_BUILTIN(net_read_handle_yieldable, "read", "(conn: NetConn, maxlen?: int): string?",
                  "Read data from connection")
XR_DEFINE_BUILTIN(net_read_into_yieldable, "readInto",
                  "(conn: NetConn, buffer: Bytes, maxlen?: int): int",
                  "Read data into a reusable Bytes buffer")
XR_DEFINE_BUILTIN(net_write_handle_yieldable, "write", "(conn: NetConn, data: string): int",
                  "Write data to connection")
XR_DEFINE_BUILTIN(net_write_bytes_yieldable, "writeBytes", "(conn: NetConn, data: Bytes): int",
                  "Write Bytes data to connection")
XR_DEFINE_BUILTIN(net_copy_yieldable, "copy", "(src: NetConn, dst: NetConn, bufferSize?: int): int",
                  "Copy a TCP/TLS stream using a reusable native buffer")
XR_DEFINE_BUILTIN(net_copy_bidirectional_yieldable, "copyBidirectional",
                  "(a: NetConn, b: NetConn): Json", "Copy two TCP/TLS streams in both directions")
XR_DEFINE_BUILTIN(net_shutdown_read, "shutdownRead", "(conn: NetConn): bool",
                  "Shut down the read side of a TCP connection")
XR_DEFINE_BUILTIN(net_shutdown_write, "shutdownWrite", "(conn: NetConn): bool",
                  "Shut down the write side of a TCP connection")
XR_DEFINE_BUILTIN(net_shutdown_conn, "shutdown", "(conn: NetConn): bool",
                  "Shut down both sides of a TCP connection")
XR_DEFINE_BUILTIN(net_close_handle, "close", "(handle: NetConn | NetListener): ()",
                  "Close a connection or listener")
XR_DEFINE_BUILTIN(net_fd_handle, "fd", "(handle: NetConn | NetListener): int", "Get fd from handle")
XR_DEFINE_BUILTIN(net_set_read_deadline, "setReadDeadline", "(conn: NetConn, deadline: int): bool",
                  "Set read deadline in monotonic ms")
XR_DEFINE_BUILTIN(net_set_write_deadline, "setWriteDeadline",
                  "(conn: NetConn, deadline: int): bool", "Set write deadline in monotonic ms")
XR_DEFINE_BUILTIN(net_set_deadline, "setDeadline", "(conn: NetConn, deadline: int): bool",
                  "Set read and write deadlines in monotonic ms")
XR_DEFINE_BUILTIN(net_set_accept_deadline, "setAcceptDeadline",
                  "(listener: NetListener, deadline: int): bool",
                  "Set accept deadline in monotonic ms")
XR_DEFINE_BUILTIN(net_last_error, "lastError", "(handle: NetConn | NetListener): string?",
                  "Return the last network error name")
XR_DEFINE_BUILTIN(net_last_errno, "lastErrno", "(handle: NetConn | NetListener): int",
                  "Return the last system errno")
XR_DEFINE_BUILTIN(net_dns_lookup, "lookup", "(hostname: string): string?", "DNS lookup")
XR_DEFINE_BUILTIN(net_has_tls, "hasTLS", "(): bool", "Check if TLS support is available")
XR_DEFINE_BUILTIN(net_dial_tls_yieldable, "dialTLS",
                  "(host: string, port: int, timeout?: int): NetConn?", "Dial a TLS connection")
XR_DEFINE_BUILTIN(net_upgrade_tls_yieldable, "upgradeTLS",
                  "(conn: NetConn, hostname: string, timeout?: int): NetConn?",
                  "Upgrade connection to TLS")
XR_DEFINE_BUILTIN(net_udp_bind_handle, "udpBind", "(port: int, addr?: string): NetConn?",
                  "Bind a UDP socket")
XR_DEFINE_BUILTIN(net_send_to_yieldable, "sendTo",
                  "(handle: NetConn, data: string, host: string, port: int): int",
                  "Send UDP datagram")
XR_DEFINE_BUILTIN(net_recv_from_yieldable, "recvFrom",
                  "(handle: NetConn, maxlen?: int): UdpPacket?",
                  "Receive UDP datagram (returns flat handle: data, host, port)")

/* ========== Native-type instance methods (synchronous) ==========
 *
 * Yieldable operations (read / write / accept) stay as module-level
 * cfuncs because the native-type method table currently only carries
 * the synchronous XrCFunctionPtr signature. Once the dispatcher grows
 * a yieldable variant, the matching wrappers can move here.
 */

static XrValue conn_method_fd(XrayIsolate *X, XrValue self, XrValue *args, int n) {
    (void) X;
    (void) args;
    (void) n;
    XrNetConn *c = unwrap_conn(self);
    return xr_int(c ? c->fd : -1);
}

static XrValue conn_method_close(XrayIsolate *X, XrValue self, XrValue *args, int n) {
    (void) X;
    (void) args;
    (void) n;
    XrNetConn *c = unwrap_conn(self);
    if (c)
        xr_net_conn_close(c);
    return XR_NULL_VAL;
}

static XrValue conn_method_is_closed(XrayIsolate *X, XrValue self, XrValue *args, int n) {
    (void) X;
    (void) args;
    (void) n;
    XrNetConn *c = unwrap_conn(self);
    return xr_bool(!c || c->closed);
}

static XrValue conn_method_is_tls(XrayIsolate *X, XrValue self, XrValue *args, int n) {
    (void) X;
    (void) args;
    (void) n;
    XrNetConn *c = unwrap_conn(self);
    return xr_bool(c && c->kind == XR_NETCONN_TLS);
}

static XrValue listener_method_fd(XrayIsolate *X, XrValue self, XrValue *args, int n) {
    (void) X;
    (void) args;
    (void) n;
    XrNetListener *l = unwrap_listener(self);
    return xr_int(l ? l->fd : -1);
}

static XrValue listener_method_port(XrayIsolate *X, XrValue self, XrValue *args, int n) {
    (void) X;
    (void) args;
    (void) n;
    XrNetListener *l = unwrap_listener(self);
    return xr_int(l ? l->port : -1);
}

static XrValue listener_method_close(XrayIsolate *X, XrValue self, XrValue *args, int n) {
    (void) X;
    (void) args;
    (void) n;
    XrNetListener *l = unwrap_listener(self);
    if (l)
        xr_net_listener_close(l);
    return XR_NULL_VAL;
}

static XrValue listener_method_is_closed(XrayIsolate *X, XrValue self, XrValue *args, int n) {
    (void) X;
    (void) args;
    (void) n;
    XrNetListener *l = unwrap_listener(self);
    return xr_bool(!l || l->closed);
}

/* NetConn and NetListener class registrations are invoked
 * unconditionally during isolate init by
 * xr_prelude_register_all_native_types, so the XrClasses are available
 * even when user code never `import net`. */
void xr_netconn_register_class(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "netconn_register_class: NULL isolate");
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core != NULL, "netconn_register_class: NULL core");

    XrClassBuilder *b = xr_class_builder_new(isolate, "NetConn", NULL);
    XR_CHECK(b != NULL, "netconn_register_class: builder alloc failed");

    xr_class_builder_set_native_body(b, xr_netconn_body_desc());

    xr_class_builder_add_method(b, "fd", conn_method_fd, 0, 0);
    xr_class_builder_add_method(b, "close", conn_method_close, 0, 0);
    xr_class_builder_add_method(b, "isClosed", conn_method_is_closed, 0, 0);
    xr_class_builder_add_method(b, "isTLS", conn_method_is_tls, 0, 0);

    XrClass *cls = xr_class_builder_finalize(b);
    XR_CHECK(cls != NULL, "netconn_register_class: finalize failed");
    cls->flags |= XR_CLASS_BUILTIN | XR_CLASS_HAS_NATIVE_BODY;
    cls->builtin_kind = XR_BK_NETCONN;
    core->netConnClass = cls;
}

void xr_netlistener_register_class(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "netlistener_register_class: NULL isolate");
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core != NULL, "netlistener_register_class: NULL core");

    XrClassBuilder *b = xr_class_builder_new(isolate, "NetListener", NULL);
    XR_CHECK(b != NULL, "netlistener_register_class: builder alloc failed");

    xr_class_builder_set_native_body(b, xr_netlistener_body_desc());

    xr_class_builder_add_method(b, "fd", listener_method_fd, 0, 0);
    xr_class_builder_add_method(b, "port", listener_method_port, 0, 0);
    xr_class_builder_add_method(b, "close", listener_method_close, 0, 0);
    xr_class_builder_add_method(b, "isClosed", listener_method_is_closed, 0, 0);

    XrClass *cls = xr_class_builder_finalize(b);
    XR_CHECK(cls != NULL, "netlistener_register_class: finalize failed");
    cls->flags |= XR_CLASS_BUILTIN | XR_CLASS_HAS_NATIVE_BODY;
    cls->builtin_kind = XR_BK_NETLISTENER;
    core->netListenerClass = cls;
}

XrModule *xr_load_module_net(XrayIsolate *isolate) {
    XrModule *mod = xr_module_create_native(isolate, "net");

    // NetConn / NetListener XrClasses are registered up front by the
    // prelude module; nothing to do here.

    // User-level API (handle-based)
    XRS_EXPORT_YIELDABLE(mod, isolate, "dial", net_dial_yieldable);
    XRS_EXPORT(mod, isolate, "listen", net_listen_handle);
    XRS_EXPORT_YIELDABLE(mod, isolate, "accept", net_accept_handle_yieldable);
    XRS_EXPORT_YIELDABLE(mod, isolate, "read", net_read_handle_yieldable);
    XRS_EXPORT_YIELDABLE(mod, isolate, "readInto", net_read_into_yieldable);
    XRS_EXPORT_YIELDABLE(mod, isolate, "write", net_write_handle_yieldable);
    XRS_EXPORT_YIELDABLE(mod, isolate, "writeBytes", net_write_bytes_yieldable);
    XRS_EXPORT_YIELDABLE(mod, isolate, "copy", net_copy_yieldable);
    XRS_EXPORT_YIELDABLE(mod, isolate, "copyBidirectional", net_copy_bidirectional_yieldable);
    XRS_EXPORT(mod, isolate, "shutdownRead", net_shutdown_read);
    XRS_EXPORT(mod, isolate, "shutdownWrite", net_shutdown_write);
    XRS_EXPORT(mod, isolate, "shutdown", net_shutdown_conn);
    XRS_EXPORT(mod, isolate, "close", net_close_handle);
    XRS_EXPORT(mod, isolate, "fd", net_fd_handle);
    XRS_EXPORT(mod, isolate, "setReadDeadline", net_set_read_deadline);
    XRS_EXPORT(mod, isolate, "setWriteDeadline", net_set_write_deadline);
    XRS_EXPORT(mod, isolate, "setDeadline", net_set_deadline);
    XRS_EXPORT(mod, isolate, "setAcceptDeadline", net_set_accept_deadline);
    XRS_EXPORT(mod, isolate, "lastError", net_last_error);
    XRS_EXPORT(mod, isolate, "lastErrno", net_last_errno);
    XRS_EXPORT(mod, isolate, "lookup", net_dns_lookup);
    XRS_EXPORT(mod, isolate, "hasTLS", net_has_tls);

#ifdef XR_ENABLE_TLS
    XRS_EXPORT_YIELDABLE(mod, isolate, "dialTLS", net_dial_tls_yieldable);
    XRS_EXPORT_YIELDABLE(mod, isolate, "upgradeTLS", net_upgrade_tls_yieldable);
#endif

    // UDP (handle-based)
    XRS_EXPORT(mod, isolate, "udpBind", net_udp_bind_handle);
    XRS_EXPORT_YIELDABLE(mod, isolate, "sendTo", net_send_to_yieldable);
    XRS_EXPORT_YIELDABLE(mod, isolate, "recvFrom", net_recv_from_yieldable);

    return mod;
}
