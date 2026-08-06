/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_net.h - Hosted AOT helpers for net.* TCP handle primitives.
 */

#ifndef XRT_NET_H
#define XRT_NET_H

#include "xrt_arc.h"
#include "xrt_value.h"
#include "../shared/xr_array_abi.h"
#include "../shared/xr_elem_type.h"
#include "../os/os_net.h"

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define XRT_NET_DEFAULT_TIMEOUT_MS 30000
#define XRT_NET_DEFAULT_READ_BYTES 4096
#define XRT_NET_MAX_READ_BYTES 1048576

typedef enum xrt_net_handle_kind {
    XRT_NET_HANDLE_CONN = 1,
    XRT_NET_HANDLE_LISTENER = 2,
} xrt_net_handle_kind_t;

typedef enum xrt_net_conn_kind {
    XRT_NETCONN_TCP = 0,
    XRT_NETCONN_UDP = 1,
    XRT_NETCONN_TLS = 2,
} xrt_net_conn_kind_t;

typedef enum xrt_net_error_kind {
    XRT_NETERR_NONE = 0,
    XRT_NETERR_TIMEOUT = 1,
    XRT_NETERR_CLOSED = 2,
    XRT_NETERR_RESET = 3,
    XRT_NETERR_REFUSED = 4,
    XRT_NETERR_DNS = 5,
    XRT_NETERR_TLS = 6,
    XRT_NETERR_IO = 7,
    XRT_NETERR_INVALID = 8,
} xrt_net_error_kind_t;

typedef struct xrt_net_handle_base {
    uint8_t handle_kind;
    uint8_t last_error;
    bool closed;
    int last_errno;
    xr_socket_t fd;
} xrt_net_handle_base_t;

typedef struct xrt_net_conn_object {
    xrt_net_handle_base_t base;
    uint8_t conn_kind;
    int64_t read_deadline_ms;
    int64_t write_deadline_ms;
} xrt_net_conn_object_t;

typedef struct xrt_net_listener_object {
    xrt_net_handle_base_t base;
    int port;
    int64_t accept_deadline_ms;
} xrt_net_listener_object_t;

/* xrt_net.h is included before xrt_coll.h, so use the shared array ABI
 * directly instead of depending on xrt_array_t. */
typedef struct xrt_net_array_view {
    XrObjHeader hdr;
    XR_ARRAY_ABI_FIELDS;
} xrt_net_array_view_t;

static inline void xrt_net_init_once(void) {
#if defined(XR_OS_WINDOWS)
    static int initialized = 0;
    if (!initialized) {
        if (xr_winsock_init() == 0)
            initialized = 1;
    }
#endif
}

static inline int64_t xrt_net_int_arg(XrValue v) {
    return v.tag == XR_TAG_I64 ? v.i : 0;
}

static inline int64_t xrt_net_now_ms(void) {
#if defined(XR_OS_WINDOWS)
    return (int64_t) GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (int64_t) ts.tv_sec * 1000 + (int64_t) (ts.tv_nsec / 1000000);
    return 0;
#endif
}

static inline int64_t xrt_net_relative_deadline_ms(int64_t timeout_ms) {
    if (timeout_ms <= 0)
        timeout_ms = XRT_NET_DEFAULT_TIMEOUT_MS;
    return xrt_net_now_ms() + timeout_ms;
}

static inline int xrt_net_deadline_timeout_ms(int64_t deadline_ms, struct timeval *tv) {
    if (deadline_ms <= 0)
        return -1;
    int64_t now = xrt_net_now_ms();
    int64_t remaining = deadline_ms - now;
    if (remaining <= 0)
        remaining = 0;
    tv->tv_sec = (long) (remaining / 1000);
    tv->tv_usec = (long) ((remaining % 1000) * 1000);
    return (int) (remaining > INT32_MAX ? INT32_MAX : remaining);
}

static inline uint8_t xrt_net_error_from_errno(int err) {
    if (err == XR_ETIMEDOUT)
        return XRT_NETERR_TIMEOUT;
    if (err == XR_ECONNREFUSED)
        return XRT_NETERR_REFUSED;
#if defined(ECONNRESET)
    if (err == ECONNRESET)
        return XRT_NETERR_RESET;
#endif
#if defined(EPIPE)
    if (err == EPIPE)
        return XRT_NETERR_RESET;
#endif
#if defined(EBADF)
    if (err == EBADF)
        return XRT_NETERR_CLOSED;
#endif
#if defined(ENOTCONN)
    if (err == ENOTCONN)
        return XRT_NETERR_CLOSED;
#endif
    return XRT_NETERR_IO;
}

static inline void xrt_net_clear_error_base(xrt_net_handle_base_t *base) {
    if (!base)
        return;
    base->last_error = XRT_NETERR_NONE;
    base->last_errno = 0;
}

static inline void xrt_net_set_error_base(xrt_net_handle_base_t *base, uint8_t kind, int err) {
    if (!base)
        return;
    base->last_error = kind;
    base->last_errno = err;
}

static inline const char *xrt_net_error_variant_name(uint8_t kind) {
    switch (kind) {
        case XRT_NETERR_TIMEOUT:
            return "Timeout";
        case XRT_NETERR_CLOSED:
            return "Closed";
        case XRT_NETERR_RESET:
            return "Reset";
        case XRT_NETERR_REFUSED:
            return "Refused";
        case XRT_NETERR_DNS:
            return "Dns";
        case XRT_NETERR_TLS:
            return "Tls";
        case XRT_NETERR_IO:
            return "Io";
        case XRT_NETERR_INVALID:
            return "Invalid";
        default:
            return NULL;
    }
}

static inline xrt_net_conn_object_t *xrt_net_conn_ptr(XrValue value) {
    return value.tag == XR_TAG_NET_CONN && value.ptr ? (xrt_net_conn_object_t *) value.ptr : NULL;
}

static inline xrt_net_listener_object_t *xrt_net_listener_ptr(XrValue value) {
    return value.tag == XR_TAG_NET_LISTENER && value.ptr ? (xrt_net_listener_object_t *) value.ptr
                                                         : NULL;
}

static inline xrt_net_handle_base_t *xrt_net_handle_base_ptr(XrValue value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(value);
    if (conn)
        return &conn->base;
    xrt_net_listener_object_t *listener = xrt_net_listener_ptr(value);
    return listener ? &listener->base : NULL;
}

static inline XrValue xrt_net_conn_box(xrt_net_conn_object_t *conn) {
    return conn ? xr_mkptr(conn, XR_TAG_NET_CONN) : XR_NULL_VAL;
}

static inline XrValue xrt_net_listener_box(xrt_net_listener_object_t *listener) {
    return listener ? xr_mkptr(listener, XR_TAG_NET_LISTENER) : XR_NULL_VAL;
}

static inline xrt_net_conn_object_t *xrt_net_conn_new(xr_socket_t fd, uint8_t conn_kind) {
    xrt_net_conn_object_t *conn = (xrt_net_conn_object_t *) xrt_arc_alloc(sizeof(*conn));
    memset(conn, 0, sizeof(*conn));
    conn->base.handle_kind = XRT_NET_HANDLE_CONN;
    conn->base.fd = fd;
    conn->conn_kind = conn_kind;
    xrt_arc_mark_builtin(conn, XRT_ARC_KIND_NET_CONN);
    return conn;
}

static inline xrt_net_listener_object_t *xrt_net_listener_new(xr_socket_t fd, int port) {
    xrt_net_listener_object_t *listener =
        (xrt_net_listener_object_t *) xrt_arc_alloc(sizeof(*listener));
    memset(listener, 0, sizeof(*listener));
    listener->base.handle_kind = XRT_NET_HANDLE_LISTENER;
    listener->base.fd = fd;
    listener->port = port;
    xrt_arc_mark_builtin(listener, XRT_ARC_KIND_NET_LISTENER);
    return listener;
}

static inline void xrt_net_close_fd(xr_socket_t fd) {
    if (fd == XR_INVALID_SOCKET)
        return;
    shutdown(fd, XR_SHUT_RDWR);
    xr_closesocket(fd);
}

static inline void xrt_net_close_base(xrt_net_handle_base_t *base) {
    if (!base || base->closed)
        return;
    xr_socket_t fd = base->fd;
    base->fd = XR_INVALID_SOCKET;
    base->closed = true;
    xrt_net_close_fd(fd);
    xrt_net_set_error_base(base, XRT_NETERR_CLOSED, 0);
}

static inline void xrt_net_destroy_builtin(void *obj) {
    if (!obj)
        return;
    xrt_net_close_base((xrt_net_handle_base_t *) obj);
}

static inline int xrt_net_wait_fd(xr_socket_t fd, bool want_read, int64_t deadline_ms) {
    for (;;) {
        fd_set rfds;
        fd_set wfds;
        fd_set *read_set = NULL;
        fd_set *write_set = NULL;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        if (want_read) {
            FD_SET(fd, &rfds);
            read_set = &rfds;
        } else {
            FD_SET(fd, &wfds);
            write_set = &wfds;
        }

        struct timeval tv;
        struct timeval *tvp = NULL;
        if (xrt_net_deadline_timeout_ms(deadline_ms, &tv) >= 0)
            tvp = &tv;

        int ret = select((int) fd + 1, read_set, write_set, NULL, tvp);
        if (ret > 0)
            return 1;
        if (ret == 0) {
            xr_set_socket_error(XR_ETIMEDOUT);
            return 0;
        }
        int err = xr_get_socket_error();
        if (err == XR_EINTR)
            continue;
        return -1;
    }
}

static inline char *xrt_net_cstr_dup_arg(const char *data, int64_t len) {
    if (!data || len < 0)
        return NULL;
    uint64_t n64 = (uint64_t) len;
    if (n64 > (uint64_t) SIZE_MAX - 1u)
        return NULL;
    size_t n = (size_t) n64;
    char *out = (char *) XRT_MALLOC(n + 1u);
    if (!out)
        return NULL;
    memcpy(out, data, n);
    out[n] = '\0';
    return out;
}

static inline XrValue xrt_net_lookup(const char *host_data, int64_t host_len) {
    xrt_net_init_once();
    char *host = xrt_net_cstr_dup_arg(host_data, host_len);
    if (!host || host[0] == '\0') {
        XRT_FREE(host);
        return XR_NULL_VAL;
    }
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *addresses = NULL;
    int status = getaddrinfo(host, NULL, &hints, &addresses);
    XRT_FREE(host);
    if (status != 0 || !addresses)
        return XR_NULL_VAL;

    char text[INET6_ADDRSTRLEN];
    const char *resolved = NULL;
    for (const struct addrinfo *item = addresses; item; item = item->ai_next) {
        const void *address = NULL;
        if (item->ai_family == AF_INET)
            address = &((const struct sockaddr_in *) item->ai_addr)->sin_addr;
        else if (item->ai_family == AF_INET6)
            address = &((const struct sockaddr_in6 *) item->ai_addr)->sin6_addr;
        if (address && inet_ntop(item->ai_family, address, text, sizeof(text))) {
            resolved = text;
            break;
        }
    }
    XrValue result = XR_NULL_VAL;
    if (resolved) {
        size_t length = strlen(resolved);
        result = xrt_str_alloc(length);
        if (length != 0)
            memcpy(xr_str_buf(result), resolved, length);
    }
    freeaddrinfo(addresses);
    return result;
}

static inline XrValue xrt_net_dial(const char *host_data, int64_t host_len, XrValue port_value,
                                   XrValue timeout_value) {
    xrt_net_init_once();
    char *host = xrt_net_cstr_dup_arg(host_data, host_len);
    int64_t port_i = xrt_net_int_arg(port_value);
    if (!host || host[0] == '\0' || port_i <= 0 || port_i > 65535) {
        XRT_FREE(host);
        return XR_NULL_VAL;
    }

    int64_t deadline = xrt_net_relative_deadline_ms(xrt_net_int_arg(timeout_value));
    char service[16];
    snprintf(service, sizeof(service), "%lld", (long long) port_i);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, service, &hints, &res);
    XRT_FREE(host);
    if (gai != 0 || !res)
        return XR_NULL_VAL;

    int last_err = 0;
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        xr_socket_t fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == XR_INVALID_SOCKET)
            continue;
        xr_socket_set_nonblocking(fd);
        xr_socket_set_nodelay(fd, true);

        int ret = connect(fd, rp->ai_addr, (socklen_t) rp->ai_addrlen);
        if (ret == 0) {
            freeaddrinfo(res);
            return xrt_net_conn_box(xrt_net_conn_new(fd, XRT_NETCONN_TCP));
        }

        int err = xr_get_socket_error();
        if (err == XR_EINPROGRESS || err == XR_EWOULDBLOCK || err == XR_EAGAIN) {
            int ready = xrt_net_wait_fd(fd, false, deadline);
            if (ready > 0) {
                err = xr_socket_get_error(fd);
                if (err == 0) {
                    freeaddrinfo(res);
                    return xrt_net_conn_box(xrt_net_conn_new(fd, XRT_NETCONN_TCP));
                }
            } else {
                err = xr_get_socket_error();
            }
        }
        last_err = err;
        xrt_net_close_fd(fd);
    }

    (void) last_err;
    freeaddrinfo(res);
    return XR_NULL_VAL;
}

static inline XrValue xrt_net_dial_tls(const char *host_data, int64_t host_len, XrValue port_value,
                                       XrValue timeout_value) {
    (void) host_data;
    (void) host_len;
    (void) port_value;
    (void) timeout_value;
    return XR_NULL_VAL;
}

static inline XrValue xrt_net_dial_default(const char *host_data, int64_t host_len,
                                           XrValue port_value) {
    return xrt_net_dial(host_data, host_len, port_value, XR_FROM_INT(XRT_NET_DEFAULT_TIMEOUT_MS));
}

static inline XrValue xrt_net_dial_tls_default(const char *host_data, int64_t host_len,
                                               XrValue port_value) {
    return xrt_net_dial_tls(host_data, host_len, port_value,
                            XR_FROM_INT(XRT_NET_DEFAULT_TIMEOUT_MS));
}

static inline XrValue xrt_net_listen(XrValue port_value, XrValue backlog_value) {
    xrt_net_init_once();
    int64_t port_i = xrt_net_int_arg(port_value);
    int64_t backlog_i = xrt_net_int_arg(backlog_value);
    if (port_i < 0 || port_i > 65535)
        return XR_NULL_VAL;
    int backlog = backlog_i > 0 ? (int) backlog_i : 1024;

    xr_socket_t fd = XR_INVALID_SOCKET;
    if (fd == XR_INVALID_SOCKET) {
        fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (fd != XR_INVALID_SOCKET) {
            xr_socket_set_reuseaddr(fd, true);
            int v6only = 0;
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char *) &v6only, sizeof(v6only));
            struct sockaddr_in6 sa6;
            memset(&sa6, 0, sizeof(sa6));
            sa6.sin6_family = AF_INET6;
            sa6.sin6_port = htons((uint16_t) port_i);
            sa6.sin6_addr = in6addr_any;
            if (bind(fd, (struct sockaddr *) &sa6, sizeof(sa6)) != 0 || listen(fd, backlog) != 0) {
                xrt_net_close_fd(fd);
                fd = XR_INVALID_SOCKET;
            }
        }
    }

    if (fd == XR_INVALID_SOCKET) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == XR_INVALID_SOCKET)
            return XR_NULL_VAL;
        xr_socket_set_reuseaddr(fd, true);
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t) port_i);
        sa.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) != 0 || listen(fd, backlog) != 0) {
            xrt_net_close_fd(fd);
            return XR_NULL_VAL;
        }
    }

    xr_socket_set_nonblocking(fd);
    int port = (int) port_i;
    if (port == 0) {
        struct sockaddr_storage ss;
        socklen_t sslen = sizeof(ss);
        if (getsockname(fd, (struct sockaddr *) &ss, &sslen) == 0) {
            if (ss.ss_family == AF_INET6)
                port = ntohs(((struct sockaddr_in6 *) &ss)->sin6_port);
            else if (ss.ss_family == AF_INET)
                port = ntohs(((struct sockaddr_in *) &ss)->sin_port);
        }
    }

    return xrt_net_listener_box(xrt_net_listener_new(fd, port));
}

static inline XrValue xrt_net_listen_default(XrValue port_value) {
    return xrt_net_listen(port_value, XR_FROM_INT(1024));
}

static inline XrValue xrt_net_accept(XrValue listener_value) {
    xrt_net_listener_object_t *listener = xrt_net_listener_ptr(listener_value);
    if (!listener || listener->base.closed || listener->base.fd == XR_INVALID_SOCKET) {
        if (listener)
            xrt_net_set_error_base(&listener->base, XRT_NETERR_CLOSED, 0);
        return XR_NULL_VAL;
    }

    for (;;) {
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
        xr_socket_t fd = accept(listener->base.fd, (struct sockaddr *) &addr, &addrlen);
        if (fd != XR_INVALID_SOCKET) {
            xr_socket_set_nonblocking(fd);
            xr_socket_set_nodelay(fd, true);
            xrt_net_clear_error_base(&listener->base);
            return xrt_net_conn_box(xrt_net_conn_new(fd, XRT_NETCONN_TCP));
        }

        int err = xr_get_socket_error();
        if (xr_socket_err_is_again(err)) {
            int ready = xrt_net_wait_fd(listener->base.fd, true, listener->accept_deadline_ms);
            if (ready > 0)
                continue;
            err = xr_get_socket_error();
            xrt_net_set_error_base(&listener->base, xrt_net_error_from_errno(err), err);
            return XR_NULL_VAL;
        }
        if (err == XR_EINTR)
            continue;
        xrt_net_set_error_base(&listener->base, xrt_net_error_from_errno(err), err);
        return XR_NULL_VAL;
    }
}

static inline XrValue xrt_net_read(XrValue conn_value, XrValue maxlen_value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    if (!conn || conn->base.closed || conn->base.fd == XR_INVALID_SOCKET) {
        if (conn)
            xrt_net_set_error_base(&conn->base, XRT_NETERR_CLOSED, 0);
        return XR_NULL_VAL;
    }
    if (conn->conn_kind == XRT_NETCONN_TLS) {
        xrt_net_set_error_base(&conn->base, XRT_NETERR_TLS, 0);
        return XR_NULL_VAL;
    }

    int64_t requested = xrt_net_int_arg(maxlen_value);
    if (requested <= 0)
        requested = XRT_NET_DEFAULT_READ_BYTES;
    if (requested > XRT_NET_MAX_READ_BYTES)
        requested = XRT_NET_MAX_READ_BYTES;
    char *buf = (char *) XRT_MALLOC((size_t) requested);
    if (!buf) {
        xrt_net_set_error_base(&conn->base, XRT_NETERR_IO, 0);
        return XR_NULL_VAL;
    }

    for (;;) {
        ssize_t n = xr_socket_recv(conn->base.fd, buf, (size_t) requested);
        if (n > 0) {
            XrValue out = xrt_str_alloc((size_t) n);
            memcpy(xr_str_buf(out), buf, (size_t) n);
            XRT_FREE(buf);
            xrt_net_clear_error_base(&conn->base);
            return out;
        }
        if (n == 0) {
            XRT_FREE(buf);
            xrt_net_clear_error_base(&conn->base);
            return XR_NULL_VAL;
        }
        int err = xr_get_socket_error();
        if (xr_socket_err_is_again(err)) {
            int ready = xrt_net_wait_fd(conn->base.fd, true, conn->read_deadline_ms);
            if (ready > 0)
                continue;
            err = xr_get_socket_error();
        }
        XRT_FREE(buf);
        xrt_net_set_error_base(&conn->base, xrt_net_error_from_errno(err), err);
        return XR_NULL_VAL;
    }
}

static inline XrValue xrt_net_read_default(XrValue conn_value) {
    return xrt_net_read(conn_value, XR_FROM_INT(XRT_NET_DEFAULT_READ_BYTES));
}

static inline XrValue xrt_net_read_into(XrValue conn_value, XrValue buffer_value,
                                        XrValue maxlen_value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    if (!conn || conn->base.closed || conn->base.fd == XR_INVALID_SOCKET) {
        if (conn)
            xrt_net_set_error_base(&conn->base, XRT_NETERR_CLOSED, 0);
        return XR_FROM_INT(-1);
    }
    if (conn->conn_kind == XRT_NETCONN_TLS) {
        xrt_net_set_error_base(&conn->base, XRT_NETERR_TLS, 0);
        return XR_FROM_INT(-1);
    }
    if (!XR_IS_ARRAY(buffer_value) || !buffer_value.ptr) {
        xrt_net_set_error_base(&conn->base, XRT_NETERR_INVALID, 0);
        return XR_FROM_INT(-1);
    }

    xrt_net_array_view_t *buffer = (xrt_net_array_view_t *) buffer_value.ptr;
    if (buffer->elem_type != XR_ELEM_U8 || buffer->elem_size != 1 || buffer->capacity <= 0 ||
        !buffer->data) {
        xrt_net_set_error_base(&conn->base, XRT_NETERR_INVALID, 0);
        return XR_FROM_INT(-1);
    }

    int64_t requested = xrt_net_int_arg(maxlen_value);
    if (requested <= 0 || requested > buffer->capacity)
        requested = buffer->capacity;

    for (;;) {
        ssize_t n = xr_socket_recv(conn->base.fd, (char *) buffer->data, (size_t) requested);
        if (n >= 0) {
            buffer->length = (int64_t) n;
            xrt_net_clear_error_base(&conn->base);
            return XR_FROM_INT((int64_t) n);
        }
        int err = xr_get_socket_error();
        if (err == XR_EINTR)
            continue;
        if (xr_socket_err_is_again(err)) {
            int ready = xrt_net_wait_fd(conn->base.fd, true, conn->read_deadline_ms);
            if (ready > 0)
                continue;
            err = xr_get_socket_error();
        }
        xrt_net_set_error_base(&conn->base, xrt_net_error_from_errno(err), err);
        return XR_FROM_INT(-1);
    }
}

static inline XrValue xrt_net_write(XrValue conn_value, const char *data, int64_t len) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    if (!conn || conn->base.closed || conn->base.fd == XR_INVALID_SOCKET) {
        if (conn)
            xrt_net_set_error_base(&conn->base, XRT_NETERR_CLOSED, 0);
        return XR_FROM_INT(-1);
    }
    if (conn->conn_kind == XRT_NETCONN_TLS) {
        xrt_net_set_error_base(&conn->base, XRT_NETERR_TLS, 0);
        return XR_FROM_INT(-1);
    }
    if (!data && len > 0) {
        xrt_net_set_error_base(&conn->base, XRT_NETERR_INVALID, 0);
        return XR_FROM_INT(-1);
    }
    if (len <= 0) {
        xrt_net_clear_error_base(&conn->base);
        return XR_FROM_INT(0);
    }

    int64_t written = 0;
    while (written < len) {
        ssize_t n = xr_socket_send(conn->base.fd, data + written, (size_t) (len - written));
        if (n > 0) {
            written += n;
            continue;
        }
        if (n == 0)
            break;
        int err = xr_get_socket_error();
        if (xr_socket_err_is_again(err)) {
            int ready = xrt_net_wait_fd(conn->base.fd, false, conn->write_deadline_ms);
            if (ready > 0)
                continue;
            err = xr_get_socket_error();
        }
        xrt_net_set_error_base(&conn->base, xrt_net_error_from_errno(err), err);
        return XR_FROM_INT(written > 0 ? written : -1);
    }

    if (written == len)
        xrt_net_clear_error_base(&conn->base);
    return XR_FROM_INT(written > 0 ? written : -1);
}

static inline XrValue xrt_net_write_bytes(XrValue conn_value, XrValue data_value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    if (!XR_IS_ARRAY(data_value) || !data_value.ptr) {
        if (conn)
            xrt_net_set_error_base(&conn->base, XRT_NETERR_INVALID, 0);
        return XR_FROM_INT(-1);
    }

    xrt_net_array_view_t *data = (xrt_net_array_view_t *) data_value.ptr;
    if (data->elem_type != XR_ELEM_U8 || data->elem_size != 1 || data->length < 0 ||
        (data->length > 0 && !data->data)) {
        if (conn)
            xrt_net_set_error_base(&conn->base, XRT_NETERR_INVALID, 0);
        return XR_FROM_INT(-1);
    }
    return xrt_net_write(conn_value, (const char *) data->data, data->length);
}

#define XRT_NET_BIDI_BUFFER_BYTES 16384

static inline XrtI64PairResult xrt_net_bidi_result(int64_t first, int64_t second,
                                                   int32_t error_index) {
    XrtI64PairResult result;
    result.first = first;
    result.second = second;
    result.error_index = error_index;
    return result;
}

static inline int32_t xrt_net_error_variant_index(uint8_t kind) {
    if (kind >= XRT_NETERR_TIMEOUT && kind <= XRT_NETERR_INVALID)
        return (int32_t) kind - 1;
    return 6; /* NetError.Io */
}

static inline int64_t xrt_net_min_active_deadline(int64_t current, int64_t candidate) {
    if (candidate <= 0)
        return current;
    return current <= 0 || candidate < current ? candidate : current;
}

/* Hosted AOT data plane for net.copyBidirectional. The helper returns a
 * native pair plus an enum ordinal; codegen materializes the declared sealed
 * structural object and publishes the generated stable NetError value when needed. */
static inline XrtI64PairResult xrt_net_copy_bidirectional(XrValue a_value, XrValue b_value) {
    xrt_net_conn_object_t *a = xrt_net_conn_ptr(a_value);
    xrt_net_conn_object_t *b = xrt_net_conn_ptr(b_value);
    if (!a || !b)
        return xrt_net_bidi_result(0, 0, xrt_net_error_variant_index(XRT_NETERR_INVALID));
    if (a->base.closed || b->base.closed || a->base.fd == XR_INVALID_SOCKET ||
        b->base.fd == XR_INVALID_SOCKET)
        return xrt_net_bidi_result(0, 0, xrt_net_error_variant_index(XRT_NETERR_CLOSED));
    if (a == b || a->base.fd == b->base.fd)
        return xrt_net_bidi_result(0, 0, xrt_net_error_variant_index(XRT_NETERR_INVALID));
    if (a->conn_kind == XRT_NETCONN_TLS || b->conn_kind == XRT_NETCONN_TLS)
        return xrt_net_bidi_result(0, 0, xrt_net_error_variant_index(XRT_NETERR_TLS));

    char a_to_b_buf[XRT_NET_BIDI_BUFFER_BYTES];
    char b_to_a_buf[XRT_NET_BIDI_BUFFER_BYTES];
    size_t a_to_b_off = 0;
    size_t a_to_b_len = 0;
    size_t b_to_a_off = 0;
    size_t b_to_a_len = 0;
    int64_t a_to_b_total = 0;
    int64_t b_to_a_total = 0;
    bool a_eof = false;
    bool b_eof = false;

    for (;;) {
        if (a_eof && b_eof && a_to_b_len == 0 && b_to_a_len == 0) {
            xrt_net_clear_error_base(&a->base);
            xrt_net_clear_error_base(&b->base);
            return xrt_net_bidi_result(a_to_b_total, b_to_a_total, -1);
        }

        fd_set read_fds;
        fd_set write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        bool read_a = !a_eof && a_to_b_len == 0;
        bool read_b = !b_eof && b_to_a_len == 0;
        bool write_a = b_to_a_len > 0;
        bool write_b = a_to_b_len > 0;
        if (read_a)
            FD_SET(a->base.fd, &read_fds);
        if (read_b)
            FD_SET(b->base.fd, &read_fds);
        if (write_a)
            FD_SET(a->base.fd, &write_fds);
        if (write_b)
            FD_SET(b->base.fd, &write_fds);

        int64_t deadline = 0;
        if (read_a)
            deadline = xrt_net_min_active_deadline(deadline, a->read_deadline_ms);
        if (read_b)
            deadline = xrt_net_min_active_deadline(deadline, b->read_deadline_ms);
        if (write_a)
            deadline = xrt_net_min_active_deadline(deadline, a->write_deadline_ms);
        if (write_b)
            deadline = xrt_net_min_active_deadline(deadline, b->write_deadline_ms);

        struct timeval tv;
        struct timeval *tvp = NULL;
        if (xrt_net_deadline_timeout_ms(deadline, &tv) >= 0)
            tvp = &tv;
        xr_socket_t max_fd = a->base.fd > b->base.fd ? a->base.fd : b->base.fd;
        int ready = select((int) max_fd + 1, &read_fds, &write_fds, NULL, tvp);
        if (ready == 0) {
            xr_set_socket_error(XR_ETIMEDOUT);
            xrt_net_set_error_base(&a->base, XRT_NETERR_TIMEOUT, XR_ETIMEDOUT);
            xrt_net_set_error_base(&b->base, XRT_NETERR_TIMEOUT, XR_ETIMEDOUT);
            return xrt_net_bidi_result(a_to_b_total, b_to_a_total,
                                       xrt_net_error_variant_index(XRT_NETERR_TIMEOUT));
        }
        if (ready < 0) {
            int err = xr_get_socket_error();
            if (err == XR_EINTR)
                continue;
            uint8_t kind = xrt_net_error_from_errno(err);
            xrt_net_set_error_base(&a->base, kind, err);
            xrt_net_set_error_base(&b->base, kind, err);
            return xrt_net_bidi_result(a_to_b_total, b_to_a_total,
                                       xrt_net_error_variant_index(kind));
        }

        if (write_a && FD_ISSET(a->base.fd, &write_fds)) {
            ssize_t n = xr_socket_send(a->base.fd, b_to_a_buf + b_to_a_off, b_to_a_len);
            if (n > 0) {
                b_to_a_off += (size_t) n;
                b_to_a_len -= (size_t) n;
                b_to_a_total += (int64_t) n;
                if (b_to_a_len == 0)
                    b_to_a_off = 0;
            } else if (n == 0) {
                xrt_net_set_error_base(&a->base, XRT_NETERR_CLOSED, 0);
                return xrt_net_bidi_result(a_to_b_total, b_to_a_total,
                                           xrt_net_error_variant_index(XRT_NETERR_CLOSED));
            } else {
                int err = xr_get_socket_error();
                if (!xr_socket_err_is_again(err) && err != XR_EINTR) {
                    uint8_t kind = xrt_net_error_from_errno(err);
                    xrt_net_set_error_base(&a->base, kind, err);
                    return xrt_net_bidi_result(a_to_b_total, b_to_a_total,
                                               xrt_net_error_variant_index(kind));
                }
            }
        }

        if (write_b && FD_ISSET(b->base.fd, &write_fds)) {
            ssize_t n = xr_socket_send(b->base.fd, a_to_b_buf + a_to_b_off, a_to_b_len);
            if (n > 0) {
                a_to_b_off += (size_t) n;
                a_to_b_len -= (size_t) n;
                a_to_b_total += (int64_t) n;
                if (a_to_b_len == 0)
                    a_to_b_off = 0;
            } else if (n == 0) {
                xrt_net_set_error_base(&b->base, XRT_NETERR_CLOSED, 0);
                return xrt_net_bidi_result(a_to_b_total, b_to_a_total,
                                           xrt_net_error_variant_index(XRT_NETERR_CLOSED));
            } else {
                int err = xr_get_socket_error();
                if (!xr_socket_err_is_again(err) && err != XR_EINTR) {
                    uint8_t kind = xrt_net_error_from_errno(err);
                    xrt_net_set_error_base(&b->base, kind, err);
                    return xrt_net_bidi_result(a_to_b_total, b_to_a_total,
                                               xrt_net_error_variant_index(kind));
                }
            }
        }

        if (read_a && FD_ISSET(a->base.fd, &read_fds)) {
            ssize_t n = xr_socket_recv(a->base.fd, a_to_b_buf, sizeof(a_to_b_buf));
            if (n > 0) {
                a_to_b_off = 0;
                a_to_b_len = (size_t) n;
            } else if (n == 0) {
                a_eof = true;
                (void) shutdown(b->base.fd, XR_SHUT_WR);
            } else {
                int err = xr_get_socket_error();
                if (!xr_socket_err_is_again(err) && err != XR_EINTR) {
                    uint8_t kind = xrt_net_error_from_errno(err);
                    xrt_net_set_error_base(&a->base, kind, err);
                    return xrt_net_bidi_result(a_to_b_total, b_to_a_total,
                                               xrt_net_error_variant_index(kind));
                }
            }
        }

        if (read_b && FD_ISSET(b->base.fd, &read_fds)) {
            ssize_t n = xr_socket_recv(b->base.fd, b_to_a_buf, sizeof(b_to_a_buf));
            if (n > 0) {
                b_to_a_off = 0;
                b_to_a_len = (size_t) n;
            } else if (n == 0) {
                b_eof = true;
                (void) shutdown(a->base.fd, XR_SHUT_WR);
            } else {
                int err = xr_get_socket_error();
                if (!xr_socket_err_is_again(err) && err != XR_EINTR) {
                    uint8_t kind = xrt_net_error_from_errno(err);
                    xrt_net_set_error_base(&b->base, kind, err);
                    return xrt_net_bidi_result(a_to_b_total, b_to_a_total,
                                               xrt_net_error_variant_index(kind));
                }
            }
        }
    }
}

static inline XrValue xrt_net_close(XrValue handle_value) {
    xrt_net_close_base(xrt_net_handle_base_ptr(handle_value));
    return XR_NULL_VAL;
}

static inline XrValue xrt_net_fd(XrValue handle_value) {
    xrt_net_handle_base_t *base = xrt_net_handle_base_ptr(handle_value);
    if (!base || base->closed || base->fd == XR_INVALID_SOCKET)
        return XR_FROM_INT(-1);
    return XR_FROM_INT((int64_t) base->fd);
}

static inline XrValue xrt_net_set_read_deadline(XrValue conn_value, XrValue deadline_value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    int64_t deadline = xrt_net_int_arg(deadline_value);
    if (!conn || conn->base.closed || deadline < 0)
        return XR_FALSE_VAL;
    conn->read_deadline_ms = deadline;
    return XR_TRUE_VAL;
}

static inline XrValue xrt_net_set_write_deadline(XrValue conn_value, XrValue deadline_value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    int64_t deadline = xrt_net_int_arg(deadline_value);
    if (!conn || conn->base.closed || deadline < 0)
        return XR_FALSE_VAL;
    conn->write_deadline_ms = deadline;
    return XR_TRUE_VAL;
}

static inline XrValue xrt_net_set_deadline(XrValue conn_value, XrValue deadline_value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    int64_t deadline = xrt_net_int_arg(deadline_value);
    if (!conn || conn->base.closed || deadline < 0)
        return XR_FALSE_VAL;
    conn->read_deadline_ms = deadline;
    conn->write_deadline_ms = deadline;
    return XR_TRUE_VAL;
}

static inline XrValue xrt_net_set_accept_deadline(XrValue listener_value, XrValue deadline_value) {
    xrt_net_listener_object_t *listener = xrt_net_listener_ptr(listener_value);
    int64_t deadline = xrt_net_int_arg(deadline_value);
    if (!listener || listener->base.closed || deadline < 0)
        return XR_FALSE_VAL;
    listener->accept_deadline_ms = deadline;
    return XR_TRUE_VAL;
}

static inline XrValue xrt_net_last_error(XrValue handle_value) {
    xrt_net_handle_base_t *base = xrt_net_handle_base_ptr(handle_value);
    uint8_t kind = base ? base->last_error : (uint8_t) XRT_NETERR_INVALID;
    const char *name = xrt_net_error_variant_name(kind);
    if (!name)
        return XR_NULL_VAL;
    static const char *const member_names[] = {"Timeout",   "Closed",     "Reset", "Refused",
                                               "Dns",       "Tls",        "Io",    "Invalid",
                                               "Cancelled", "OutOfMemory"};
    static const XrAotEnumScalarLayout layout = {
        {XR_TENUM_SCALAR_LAYOUT, XR_OBJ_IMMORTAL, XR_RC_STICKY, 0, 0},
        "NetError",
        member_names,
        10,
        UINT32_C(2619647518),
    };
    (void) name;
    return xrt_enum_scalar_box(&layout, (int64_t) xrt_net_error_variant_index(kind));
}

static inline XrValue xrt_net_last_errno(XrValue handle_value) {
    xrt_net_handle_base_t *base = xrt_net_handle_base_ptr(handle_value);
    return XR_FROM_INT(base ? base->last_errno : 0);
}

static inline XrValue xrt_net_has_tls(void) {
    return XR_FALSE_VAL;
}

static inline XrValue xrt_net_listener_port(XrValue listener_value) {
    xrt_net_listener_object_t *listener = xrt_net_listener_ptr(listener_value);
    return XR_FROM_INT(listener ? listener->port : -1);
}

static inline XrValue xrt_net_is_closed(XrValue handle_value) {
    xrt_net_handle_base_t *base = xrt_net_handle_base_ptr(handle_value);
    return XR_FROM_BOOL(!base || base->closed);
}

static inline XrValue xrt_net_is_tls(XrValue conn_value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    return XR_FROM_BOOL(conn && conn->conn_kind == XRT_NETCONN_TLS);
}

static inline XrValue xrt_net_shutdown_read(XrValue conn_value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    if (!conn || conn->base.closed || conn->base.fd == XR_INVALID_SOCKET)
        return XR_FALSE_VAL;
    return XR_FROM_BOOL(shutdown(conn->base.fd, XR_SHUT_RD) == 0);
}

static inline XrValue xrt_net_shutdown_write(XrValue conn_value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    if (!conn || conn->base.closed || conn->base.fd == XR_INVALID_SOCKET)
        return XR_FALSE_VAL;
    return XR_FROM_BOOL(shutdown(conn->base.fd, XR_SHUT_WR) == 0);
}

static inline XrValue xrt_net_shutdown(XrValue conn_value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    if (!conn || conn->base.closed || conn->base.fd == XR_INVALID_SOCKET)
        return XR_FALSE_VAL;
    return XR_FROM_BOOL(shutdown(conn->base.fd, XR_SHUT_RDWR) == 0);
}

#endif  // XRT_NET_H
