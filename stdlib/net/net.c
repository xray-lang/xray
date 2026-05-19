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

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
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
 * class carries the expected flag.
 */
static inline bool is_conn_handle(XrValue v) {
    if (!XR_IS_PTR(v) || XR_HEAP_TYPE(v) != XR_TINSTANCE)
        return false;
    XrInstance *inst = (XrInstance *) XR_VALUE_GCPTR(v);
    return inst->klass && (inst->klass->flags & XR_CLASS_NETCONN);
}

static inline bool is_listener_handle(XrValue v) {
    if (!XR_IS_PTR(v) || XR_HEAP_TYPE(v) != XR_TINSTANCE)
        return false;
    XrInstance *inst = (XrInstance *) XR_VALUE_GCPTR(v);
    return inst->klass && (inst->klass->flags & XR_CLASS_NETLISTENER);
}

static inline XrNetConn *unwrap_conn(XrValue v) {
    return is_conn_handle(v) ? (XrNetConn *) XR_VALUE_GCPTR(v) : NULL;
}

static inline XrNetListener *unwrap_listener(XrValue v) {
    return is_listener_handle(v) ? (XrNetListener *) XR_VALUE_GCPTR(v) : NULL;
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

static bool handle_is_tls(XrayIsolate *X, XrValue handle) {
    (void) X;
    XrNetConn *c = unwrap_conn(handle);
    return c && c->kind == XR_NETCONN_TLS;
}

// ========== Handle-based API Functions ==========

// ========== Yieldable net.dial (handle-based) ==========

typedef struct {
    int fd;
    int phase;  // 0=connect_start done, waiting for writable; 1=connect_finish
} NetDialState;

static XrCFuncResult net_dial_step(XrayIsolate *X, NetDialState *state, XrValue *result);

static XrCFuncResult net_dial_continue(XrayIsolate *X, int status, void *ctx, XrValue *result) {
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
} NetAcceptState;

static XrCFuncResult net_accept_step(XrayIsolate *X, NetAcceptState *state, XrValue *result);

static XrCFuncResult net_accept_continue(XrayIsolate *X, int status, void *ctx, XrValue *result) {
    NetAcceptState *state = (NetAcceptState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
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
            // Error
            xr_free(state);
            *result = XR_NULL_VAL;
            return XR_CFUNC_DONE;
        }
        xr_free(state);
        *result = make_conn_handle(X, client_fd, false);
        return XR_CFUNC_DONE;
    }
    // EAGAIN - yield for read
    return xr_yield_for_io(X, state->listen_fd, XR_WAIT_READ, -1, net_accept_continue, state,
                           result);
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

    int listen_fd = handle_get_fd(X, args[0]);
    if (listen_fd < 0) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }

    NetAcceptState *state = (NetAcceptState *) xr_malloc(sizeof(NetAcceptState));
    if (!state) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }
    state->listen_fd = listen_fd;
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

typedef struct {
    int fd;
    char *buf;  // Points to coroutine's io_buf (not owned)
    size_t max_len;
    bool is_tls;
} NetReadHandleState;

static XrCFuncResult net_read_handle_step(XrayIsolate *X, NetReadHandleState *state,
                                          XrValue *result);

static XrCFuncResult net_read_handle_continue(XrayIsolate *X, int status, void *ctx,
                                              XrValue *result) {
    NetReadHandleState *state = (NetReadHandleState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        xr_free(state);
        *result = XR_NULL_VAL;
        return XR_CFUNC_ERROR;
    }
    return net_read_handle_step(X, state, result);
}

static XrCFuncResult net_read_handle_step(XrayIsolate *X, NetReadHandleState *state,
                                          XrValue *result) {
#ifdef XR_ENABLE_TLS
    if (state->is_tls) {
        XrTlsConn *tls = get_tls_conn(state->fd);
        if (!tls) {
            xr_free(state);
            *result = XR_NULL_VAL;
            return XR_CFUNC_DONE;
        }
        int n = xr_tls_conn_read_try(tls, state->buf, (int) state->max_len);
        if (n > 0) {
            *result = xr_string_value(xr_string_new(X, state->buf, n));
            xr_free(state);
            return XR_CFUNC_DONE;
        }
        if (n == 0) {
            xr_free(state);
            *result = XR_NULL_VAL;
            return XR_CFUNC_DONE;
        }
        if (n == -3) {
            xr_free(state);
            *result = XR_NULL_VAL;
            return XR_CFUNC_DONE;
        }
        // -1=WANT_READ, -2=WANT_WRITE
        int wait_mode = (n == -1) ? XR_WAIT_READ : XR_WAIT_WRITE;
        return xr_yield_for_io(X, state->fd, wait_mode, -1, net_read_handle_continue, state,
                               result);
    }
#endif

    // TCP read
    ssize_t n = read(state->fd, state->buf, state->max_len);
    if (n > 0) {
        *result = xr_string_value(xr_string_new(X, state->buf, n));
        xr_free(state);
        return XR_CFUNC_DONE;
    }
    if (n == 0) {
        xr_free(state);
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return xr_yield_for_io(X, state->fd, XR_WAIT_READ, -1, net_read_handle_continue, state,
                               result);
    }
    xr_free(state);
    *result = XR_NULL_VAL;
    return XR_CFUNC_ERROR;
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

    int fd = handle_get_fd(X, args[0]);
    if (fd < 0) {
        *result = XR_NULL_VAL;
        return XR_CFUNC_DONE;
    }

    bool is_tls = handle_is_tls(X, args[0]);
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
    state->max_len = max_len;
    state->is_tls = is_tls;
    state->buf = buf;
    return net_read_handle_step(X, state, result);
}

// ========== Handle-based net.write (yieldable, TCP + TLS dispatch) ==========

typedef struct {
    int fd;
    const char *data;  // Points directly into XrString (zero-copy, not owned)
    size_t len;
    size_t written;
    bool is_tls;
} NetWriteHandleState;

static XrCFuncResult net_write_handle_step(XrayIsolate *X, NetWriteHandleState *state,
                                           XrValue *result);

static XrCFuncResult net_write_handle_continue(XrayIsolate *X, int status, void *ctx,
                                               XrValue *result) {
    NetWriteHandleState *state = (NetWriteHandleState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        xr_free(state);
        *result = XR_FROM_INT(-1);
        return XR_CFUNC_ERROR;
    }
    return net_write_handle_step(X, state, result);
}

static XrCFuncResult net_write_handle_step(XrayIsolate *X, NetWriteHandleState *state,
                                           XrValue *result) {
#ifdef XR_ENABLE_TLS
    if (state->is_tls) {
        XrTlsConn *tls = get_tls_conn(state->fd);
        if (!tls) {
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
                // Error
                break;
            }
            // -1=WANT_WRITE, -2=WANT_READ
            int wait_mode = (n == -1) ? XR_WAIT_WRITE : XR_WAIT_READ;
            return xr_yield_for_io(X, state->fd, wait_mode, -1, net_write_handle_continue, state,
                                   result);
        }
        int total = (int) state->written;
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
            return xr_yield_for_io(X, state->fd, XR_WAIT_WRITE, -1, net_write_handle_continue,
                                   state, result);
        }
        // Other error
        break;
    }
    int total = (int) state->written;
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

    int fd = handle_get_fd(X, args[0]);
    if (fd < 0) {
        *result = XR_FROM_INT(-1);
        return XR_CFUNC_DONE;
    }

    bool is_tls = handle_is_tls(X, args[0]);
    XrString *data = XR_TO_STRING(args[1]);

    if (data->length == 0) {
        *result = XR_FROM_INT(0);
        return XR_CFUNC_DONE;
    }

    NetWriteHandleState *state = (NetWriteHandleState *) xr_calloc(1, sizeof(NetWriteHandleState));
    if (!state) {
        *result = XR_FROM_INT(-1);
        return XR_CFUNC_ERROR;
    }
    state->fd = fd;
    state->is_tls = is_tls;
    state->len = data->length;
    state->written = 0;
    // Zero-copy: XrString is immutable and coroutine arena GC
    // doesn't run while yielded, so direct reference is safe
    state->data = XR_STRING_CHARS(data);

    return net_write_handle_step(X, state, result);
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

static XrCFuncResult net_dial_tls_continue(XrayIsolate *X, int status, void *ctx, XrValue *result) {
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
        return xr_yield_for_io(X, state->fd, wait_mode, 30000, net_dial_tls_continue, state,
                               result);
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
    strncpy(state->hostname, XR_STRING_CHARS(host), sizeof(state->hostname) - 1);

    if (conn_ret == 0) {
        state->phase = 1;
        return net_dial_tls_step(X, state, result);
    }

    // EINPROGRESS: yield for write
    state->phase = 1;
    int timeout_ms = (nargs > 2 && XR_IS_INT(args[2])) ? (int) XR_TO_INT(args[2]) : 30000;
    return xr_yield_for_io(X, fd, XR_WAIT_WRITE, timeout_ms, net_dial_tls_continue, state, result);
}

// ========== Yieldable net.upgradeTLS (handle-based) ==========

typedef struct {
    int fd;
    XrValue handle;  // kept for updating tls flag on success
} NetUpgradeTLSState;

static XrCFuncResult net_upgrade_tls_step(XrayIsolate *X, NetUpgradeTLSState *state,
                                          XrValue *result);

static XrCFuncResult net_upgrade_tls_continue(XrayIsolate *X, int status, void *ctx,
                                              XrValue *result) {
    NetUpgradeTLSState *state = (NetUpgradeTLSState *) ctx;
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        // Cleanup TLS
        XrTlsConn *tls = get_tls_conn(state->fd);
        if (tls) {
            xr_tls_conn_close(tls);
            xr_tls_conn_free(tls);
            set_tls_conn(state->fd, NULL);
        }
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
    return xr_yield_for_io(X, state->fd, wait_mode, 30000, net_upgrade_tls_continue, state, result);
}

/*
 * net.upgradeTLS(conn_handle, hostname) -> conn_handle | null
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

static XrCFuncResult net_send_to_continue(XrayIsolate *X, int status, void *ctx, XrValue *result) {
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

static XrCFuncResult net_recv_from_continue(XrayIsolate *X, int status, void *ctx,
                                            XrValue *result) {
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

        // Create result Json: { data: string, addr: { host, port } }
        XrJson *json = xr_json_new(xr_current_coro(X));
        if (!json) {
            xr_free(state);
            *result = XR_NULL_VAL;
            return XR_CFUNC_DONE;
        }
        xr_json_set_by_key(X, json, "data",
                           xr_string_value(xr_string_intern(X, g_udp_recv_buf, n, 0)));

        XrJson *addr_json = xr_json_new(xr_current_coro(X));
        if (addr_json) {
            xr_json_set_by_key(X, addr_json, "host",
                               xr_string_value(xr_string_intern(X, g_udp_recv_addr.host,
                                                                strlen(g_udp_recv_addr.host), 0)));
            xr_json_set_by_key(X, addr_json, "port", xr_int(g_udp_recv_addr.port));
            xr_json_set_by_key(X, json, "addr", xr_json_value(addr_json));
        }

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
XR_DEFINE_BUILTIN(net_write_handle_yieldable, "write", "(conn: NetConn, data: string): int",
                  "Write data to connection")
XR_DEFINE_BUILTIN(net_close_handle, "close", "(handle: NetConn | NetListener)",
                  "Close a connection or listener")
XR_DEFINE_BUILTIN(net_fd_handle, "fd", "(handle: NetConn | NetListener): int", "Get fd from handle")
XR_DEFINE_BUILTIN(net_dns_lookup, "lookup", "(hostname: string): string?", "DNS lookup")
XR_DEFINE_BUILTIN(net_has_tls, "hasTLS", "(): bool", "Check if TLS support is available")
XR_DEFINE_BUILTIN(net_dial_tls_yieldable, "dialTLS",
                  "(host: string, port: int, timeout?: int): NetConn?", "Dial a TLS connection")
XR_DEFINE_BUILTIN(net_upgrade_tls_yieldable, "upgradeTLS",
                  "(conn: NetConn, hostname: string): NetConn?", "Upgrade connection to TLS")
XR_DEFINE_BUILTIN(net_udp_bind_handle, "udpBind", "(port: int, addr?: string): NetConn?",
                  "Bind a UDP socket")
XR_DEFINE_BUILTIN(net_send_to_yieldable, "sendTo",
                  "(handle: NetConn, data: string, host: string, port: int): int",
                  "Send UDP datagram")
XR_DEFINE_BUILTIN(net_recv_from_yieldable, "recvFrom", "(handle: NetConn, maxlen?: int): Json",
                  "Receive UDP datagram")

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
    cls->flags |= XR_CLASS_BUILTIN | XR_CLASS_HAS_NATIVE_BODY | XR_CLASS_NETCONN;
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
    cls->flags |= XR_CLASS_BUILTIN | XR_CLASS_HAS_NATIVE_BODY | XR_CLASS_NETLISTENER;
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
    XRS_EXPORT_YIELDABLE(mod, isolate, "write", net_write_handle_yieldable);
    XRS_EXPORT(mod, isolate, "close", net_close_handle);
    XRS_EXPORT(mod, isolate, "fd", net_fd_handle);
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
