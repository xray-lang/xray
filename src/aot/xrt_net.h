/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_net.h - Hosted AOT helpers for net.* TCP/UDP handle primitives.
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
#include <stdatomic.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Upper bound on addresses returned by net.__resolveAll; mirrors the VM
 * resolver cap so both backends hand net.xr the same candidate set. */
#define XRT_NET_RESOLVE_MAX_ADDRS 8

#if defined(XRT_ENABLE_NETPOLL)
/* Hosted executor hook: remove an fd from the runtime netpoll before the
 * platform handle is closed. The declaration and call are capability-gated so
 * synchronous network values do not pull the coroutine runtime into otherwise
 * runtime-free AOT artifacts. */
XR_FUNC void xr_aot_netpoll_close_fd(int fd);
#endif

typedef enum xrt_net_handle_kind {
    XRT_NET_HANDLE_CONN = 1,
    XRT_NET_HANDLE_LISTENER = 2,
} xrt_net_handle_kind_t;

typedef enum xrt_net_conn_kind {
    XRT_NETCONN_TCP = 0,
    XRT_NETCONN_UDP = 1,
    XRT_NETCONN_TLS = 2,
} xrt_net_conn_kind_t;

/*
 * Portable network error codes. The numbering is a stable script-facing
 * contract shared with the VM handle layer: net.__lastCode returns these
 * values verbatim and the classification table in the net module source maps
 * them to NetError variants, so renumbering is a breaking semantic change.
 */
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
    XRT_NETERR_CANCELLED = 9,
} xrt_net_error_kind_t;

/* A yieldable net primitive called from an AOT coroutine runs as a sequence of
 * non-blocking attempts. DONE carries the public result; WAIT_READ/WAIT_WRITE
 * carry the descriptor and remaining timeout the scheduler needs to park the
 * coroutine on netpoll before the attempt is replayed. `progress` lets a
 * partially completed write resume where it stopped. */
typedef enum xrt_net_try_state {
    XRT_NET_TRY_DONE = 0,
    XRT_NET_TRY_WAIT_READ = 1,
    XRT_NET_TRY_WAIT_WRITE = 2,
} xrt_net_try_state_t;

typedef struct xrt_net_try_result {
    XrValue value;
    int state;
    int fd;
    int64_t timeout_ms;
    int64_t progress;
} xrt_net_try_result_t;

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
    char udp_from_host[46]; /* INET6_ADDRSTRLEN text, "" when none */
    int udp_from_port;
} xrt_net_conn_object_t;

typedef struct xrt_net_listener_object {
    xrt_net_handle_base_t base;
    int port;
    int64_t accept_deadline_ms;
} xrt_net_listener_object_t;

/* xrt_net.h is included before xrt_coll.h, so use the shared array ABI
 * directly instead of depending on xrt_array_t. The trailing VM-ABI slots
 * keep this struct layout-identical to the full array object, which lets it
 * double as the allocation template for arrays built in this header. */
typedef struct xrt_net_array_view {
    XrObjHeader hdr;
    XR_ARRAY_ABI_FIELDS;
    uint8_t data_on_region_heap;
    uint8_t _vm_abi_pad[2];
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

static inline bool xrt_net_bool_arg(XrValue v) {
    return v.tag == XR_TAG_BOOL && v.i != 0;
}

/* Borrowed NUL-terminated text of a string argument; NULL when the value is
 * not a string. String payloads stay NUL-terminated, so this is O(1). */
static inline const char *xrt_net_str_arg(XrValue v) {
    if (!XR_IS_STR(v) || !v.ptr)
        return NULL;
    return xr_str_data(v);
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

/* A deadline of zero or less means "wait without limit"; otherwise a step that
 * would block reports the timeout once no time is left. */
static inline bool xrt_net_deadline_expired(int64_t deadline_ms) {
    return deadline_ms > 0 && deadline_ms - xrt_net_now_ms() <= 0;
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

static inline xrt_net_try_result_t xrt_net_try_done(XrValue value, int64_t progress) {
    xrt_net_try_result_t result = {value, XRT_NET_TRY_DONE, -1, -1, progress};
    return result;
}

static inline int64_t xrt_net_timeout_until(int64_t deadline_ms) {
    if (deadline_ms <= 0)
        return -1;
    int64_t remaining = deadline_ms - xrt_net_now_ms();
    return remaining > 0 ? remaining : 0;
}

static inline xrt_net_try_result_t xrt_net_try_wait(int state, xr_socket_t fd, int64_t deadline_ms,
                                                    int64_t progress) {
    xrt_net_try_result_t result = {XR_NULL_VAL, state, (int) fd, xrt_net_timeout_until(deadline_ms),
                                   progress};
    return result;
}

static inline void xrt_net_mark_timeout(XrValue handle_value) {
    xrt_net_set_error_base(xrt_net_handle_base_ptr(handle_value), XRT_NETERR_TIMEOUT, XR_ETIMEDOUT);
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
#if defined(XRT_ENABLE_NETPOLL)
    xr_aot_netpoll_close_fd((int) fd);
#endif
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

/* Single-datagram syscall wrappers, split per platform the same way as
 * xr_socket_recv / xr_socket_send in os_net.h. */
static inline ssize_t xrt_net_sendto(xr_socket_t fd, const void *buf, size_t len,
                                     const struct sockaddr *addr, socklen_t addrlen) {
#if defined(XR_OS_WINDOWS)
    int n = sendto(fd, (const char *) buf, (int) len, 0, addr, (int) addrlen);
    return (n == SOCKET_ERROR) ? -1 : (ssize_t) n;
#else
    return sendto(fd, buf, len, 0, addr, addrlen);
#endif
}

static inline ssize_t xrt_net_recvfrom(xr_socket_t fd, void *buf, size_t len, struct sockaddr *addr,
                                       socklen_t *addrlen) {
#if defined(XR_OS_WINDOWS)
    int alen = (int) *addrlen;
    int n = recvfrom(fd, (char *) buf, (int) len, 0, addr, &alen);
    if (n == SOCKET_ERROR)
        return -1;
    *addrlen = (socklen_t) alen;
    return (ssize_t) n;
#else
    return recvfrom(fd, buf, len, 0, addr, addrlen);
#endif
}

/* Build a sockaddr for one literal (numeric) address. Returns the address
 * family on success and 0 when the text is not a literal; name resolution is
 * net module policy and never happens here. */
static inline int xrt_net_literal_sockaddr(const char *text, int port, struct sockaddr_storage *out,
                                           socklen_t *out_len) {
    struct in_addr v4;
    struct in6_addr v6;
    memset(out, 0, sizeof(*out));
    if (inet_pton(AF_INET, text, &v4) == 1) {
        struct sockaddr_in *sa = (struct sockaddr_in *) out;
        sa->sin_family = AF_INET;
        sa->sin_port = htons((uint16_t) port);
        sa->sin_addr = v4;
        *out_len = (socklen_t) sizeof(*sa);
        return AF_INET;
    }
    if (inet_pton(AF_INET6, text, &v6) == 1) {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *) out;
        sa6->sin6_family = AF_INET6;
        sa6->sin6_port = htons((uint16_t) port);
        sa6->sin6_addr = v6;
        *out_len = (socklen_t) sizeof(*sa6);
        return AF_INET6;
    }
    return 0;
}

/* =========================================================================
 * Array<string> construction
 *
 * This header sits below xrt_coll.h, so the coll array constructors are not
 * declared yet. Allocate through the same execution-arena embedded layout
 * xrt_array_alloc_inline uses (header block plus aligned inline element
 * storage, finalized by xrt_execution_finalize_array) and initialize the
 * shared ABI fields for the tagged ANY lane by hand.
 * ========================================================================= */
static inline xrt_net_array_view_t *xrt_net_array_alloc_any(int64_t cap) {
    if (cap < 4)
        cap = 4;
    uint8_t elem_size = XR_ELEM_SIZES[XR_ELEM_ANY];
    size_t data_bytes = (size_t) cap * (size_t) elem_size;
    size_t pad = XRT_DATA_ALIGN - 1;
    if (data_bytes > SIZE_MAX - sizeof(xrt_net_array_view_t) - pad) {
        fprintf(stderr, "xrt_net_array_alloc_any: allocation size overflow\n");
        abort();
    }
    size_t total = sizeof(xrt_net_array_view_t) + data_bytes + pad;
    xrt_net_array_view_t *a =
        (xrt_net_array_view_t *) xrt_execution_alloc_embedded(total, xrt_execution_finalize_array);
    if (!a) {
        fprintf(stderr, "xrt_net_array_alloc_any: out of memory\n");
        abort();
    }
    xrt_heap_header_init(&a->hdr, XR_TARRAY);
    a->length = 0;
    a->capacity = cap;
    a->source = NULL;
    a->storage = NULL;
    a->elem_type = XR_ELEM_ANY;
    a->elem_size = elem_size;
    a->elem_tid = 0;
    a->contains_refs = 0;
    a->content_version = XR_ARRAY_CONTENT_VERSION_INIT;
    a->deferred_submit_version = 0;
    a->data_storage = XR_ARRAY_DATA_INLINE;
    a->data_on_region_heap = 0;
    a->_vm_abi_pad[0] = 0;
    a->_vm_abi_pad[1] = 0;
    a->data =
        (void *) (((uintptr_t) ((char *) a + sizeof(xrt_net_array_view_t)) + (XRT_DATA_ALIGN - 1)) &
                  ~(uintptr_t) (XRT_DATA_ALIGN - 1));
    memset(a->data, 0, data_bytes);
    return a;
}

/* One resolved candidate address kept in binary form until formatting. */
typedef struct xrt_net_resolved_addr {
    int family;
    union {
        struct in_addr v4;
        struct in6_addr v6;
    } addr;
} xrt_net_resolved_addr_t;

/*
 * net.__resolveAll(host) -> Array<string>
 * Resolve every address of one host and return the textual forms with IPv6
 * and IPv4 results interleaved (v6 first), capped at XRT_NET_RESOLVE_MAX_ADDRS.
 * The interleave order matches the VM resolver so net.xr sees one candidate
 * order on both backends. Failures produce an empty array, never null.
 */
static inline XrValue xrt_net_resolve_all(const char *host_data, int64_t host_len) {
    xrt_net_init_once();
    xrt_net_array_view_t *out = xrt_net_array_alloc_any(XRT_NET_RESOLVE_MAX_ADDRS);
    XrValue out_value = xr_mkptr(out, XR_TAG_ARRAY);

    char *host = xrt_net_cstr_dup_arg(host_data, host_len);
    if (!host || host[0] == '\0') {
        XRT_FREE(host);
        return out_value;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *addresses = NULL;
    int status = getaddrinfo(host, NULL, &hints, &addresses);
    XRT_FREE(host);
    if (status != 0 || !addresses)
        return out_value;

    xrt_net_resolved_addr_t v6[XRT_NET_RESOLVE_MAX_ADDRS];
    xrt_net_resolved_addr_t v4[XRT_NET_RESOLVE_MAX_ADDRS];
    int n6 = 0;
    int n4 = 0;
    for (const struct addrinfo *item = addresses; item; item = item->ai_next) {
        if (item->ai_family == AF_INET6 && n6 < XRT_NET_RESOLVE_MAX_ADDRS) {
            v6[n6].family = AF_INET6;
            v6[n6].addr.v6 = ((const struct sockaddr_in6 *) item->ai_addr)->sin6_addr;
            n6++;
        } else if (item->ai_family == AF_INET && n4 < XRT_NET_RESOLVE_MAX_ADDRS) {
            v4[n4].family = AF_INET;
            v4[n4].addr.v4 = ((const struct sockaddr_in *) item->ai_addr)->sin_addr;
            n4++;
        }
    }
    freeaddrinfo(addresses);

    xrt_net_resolved_addr_t picked[XRT_NET_RESOLVE_MAX_ADDRS];
    int count = 0;
    int i6 = 0;
    int i4 = 0;
    while (count < XRT_NET_RESOLVE_MAX_ADDRS && (i6 < n6 || i4 < n4)) {
        if (i6 < n6) {
            picked[count++] = v6[i6++];
            if (count >= XRT_NET_RESOLVE_MAX_ADDRS)
                break;
        }
        if (i4 < n4)
            picked[count++] = v4[i4++];
    }

    XrValue *items = (XrValue *) out->data;
    for (int i = 0; i < count; i++) {
        char text[INET6_ADDRSTRLEN];
        const void *address = picked[i].family == AF_INET ? (const void *) &picked[i].addr.v4
                                                          : (const void *) &picked[i].addr.v6;
        if (!inet_ntop(picked[i].family, address, text, sizeof(text)))
            continue;
        size_t length = strlen(text);
        XrValue element = xrt_str_alloc(length);
        if (length != 0)
            memcpy(xr_str_buf(element), text, length);
        items[out->length++] = element;
        out->contains_refs = 1;
    }
    return out_value;
}

/*
 * Connect has no handle to carry a code until it succeeds, so the last
 * failure code lives in a per-thread slot read by net.__lastConnectCode.
 * Mirrors the VM g_last_connect_code contract.
 */
static XR_THREAD_LOCAL int g_xrt_last_connect_code = XRT_NETERR_NONE;

static inline XrValue xrt_net_connect_fail(xr_socket_t fd, int code) {
    g_xrt_last_connect_code = code;
    if (fd != XR_INVALID_SOCKET)
        xrt_net_close_fd(fd);
    return XR_NULL_VAL;
}

/*
 * net.__connectFd(addrLiteral, port, deadlineMs) -> NetConn?
 * Non-blocking connect to ONE literal address. Name resolution and
 * multi-address fallback are net module policy; a non-literal input is an
 * invalid-argument code, not a DNS failure. A zero deadline waits without
 * limit; the net module source owns timeout policy. Null result carries its
 * code on __lastConnectCode. The return is deliberately not `NetConn | int`:
 * a union of a builtin native class with a scalar forces module-wide runtime
 * discrimination that miscompiles suspended handle results in coroutine frames.
 */
static inline XrValue xrt_net_connect_fd(XrValue addr_value, XrValue port_value,
                                         XrValue deadline_value) {
    xrt_net_init_once();
    const char *addr_text = xrt_net_str_arg(addr_value);
    int64_t port_i = xrt_net_int_arg(port_value);
    if (!addr_text || port_i < 0 || port_i > 65535)
        return xrt_net_connect_fail(XR_INVALID_SOCKET, XRT_NETERR_INVALID);

    struct sockaddr_storage addr;
    socklen_t addrlen = 0;
    int family = xrt_net_literal_sockaddr(addr_text, (int) port_i, &addr, &addrlen);
    if (family == 0)
        return xrt_net_connect_fail(XR_INVALID_SOCKET, XRT_NETERR_INVALID);

    xr_socket_t fd = socket(family, SOCK_STREAM, 0);
    if (fd == XR_INVALID_SOCKET)
        return xrt_net_connect_fail(XR_INVALID_SOCKET,
                                    xrt_net_error_from_errno(xr_get_socket_error()));
    xr_socket_set_nonblocking(fd);
    xr_socket_set_nodelay(fd, true);

    int ret = connect(fd, (struct sockaddr *) &addr, addrlen);
    if (ret == 0) {
        g_xrt_last_connect_code = XRT_NETERR_NONE;
        return xrt_net_conn_box(xrt_net_conn_new(fd, XRT_NETCONN_TCP));
    }

    int err = xr_get_socket_error();
    if (err == XR_EINPROGRESS || err == XR_EWOULDBLOCK || err == XR_EAGAIN) {
        int64_t deadline = xrt_net_int_arg(deadline_value);
        int ready = xrt_net_wait_fd(fd, false, deadline);
        if (ready == 0)
            return xrt_net_connect_fail(fd, XRT_NETERR_TIMEOUT);
        if (ready > 0) {
            err = xr_socket_get_error(fd);
            if (err == 0) {
                g_xrt_last_connect_code = XRT_NETERR_NONE;
                return xrt_net_conn_box(xrt_net_conn_new(fd, XRT_NETCONN_TCP));
            }
        } else {
            err = xr_get_socket_error();
        }
    }
    return xrt_net_connect_fail(fd, xrt_net_error_from_errno(err));
}

/* net.__lastConnectCode() -> int */
static inline XrValue xrt_net_last_connect_code(void) {
    return XR_FROM_INT(g_xrt_last_connect_code);
}

/*
 * net.__listenFd(port, backlog, forceV4) -> NetListener | null
 * Dual-stack-preferred bind: try a v6 any-address socket with V6ONLY off,
 * fall back to plain IPv4; forceV4 skips the v6 attempt entirely. Ephemeral
 * port requests read the kernel-assigned port back via getsockname.
 */
static inline XrValue xrt_net_listen_fd(XrValue port_value, XrValue backlog_value,
                                        XrValue force_v4_value) {
    xrt_net_init_once();
    int64_t port_i = xrt_net_int_arg(port_value);
    int64_t backlog_i = xrt_net_int_arg(backlog_value);
    bool force_v4 = xrt_net_bool_arg(force_v4_value);
    if (port_i < 0 || port_i > 65535 || backlog_i <= 0 || backlog_i > INT_MAX)
        return XR_NULL_VAL;
    int backlog = (int) backlog_i;

    xr_socket_t fd = XR_INVALID_SOCKET;
    if (!force_v4) {
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

static inline XrValue xrt_net_write_bytes(XrValue conn_value, XrValue data_value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    if (!XR_IS_ARRAY(data_value) || !data_value.ptr) {
        if (conn)
            xrt_net_set_error_base(&conn->base, XRT_NETERR_INVALID, 0);
        return XR_FROM_INT(-1);
    }

    xrt_net_array_view_t *view = (xrt_net_array_view_t *) data_value.ptr;
    if (view->elem_type != XR_ELEM_U8 || view->elem_size != 1 || view->length < 0 ||
        (view->length > 0 && !view->data)) {
        if (conn)
            xrt_net_set_error_base(&conn->base, XRT_NETERR_INVALID, 0);
        return XR_FROM_INT(-1);
    }
    if (!conn || conn->base.closed || conn->base.fd == XR_INVALID_SOCKET) {
        if (conn)
            xrt_net_set_error_base(&conn->base, XRT_NETERR_CLOSED, 0);
        return XR_FROM_INT(-1);
    }
    if (conn->conn_kind == XRT_NETCONN_TLS) {
        xrt_net_set_error_base(&conn->base, XRT_NETERR_TLS, 0);
        return XR_FROM_INT(-1);
    }

    const char *data = (const char *) view->data;
    int64_t len = view->length;
    if (len == 0) {
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

static inline XrValue xrt_net_set_deadline_direction(XrValue conn_value, XrValue deadline_value,
                                                     XrValue direction_value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    int64_t deadline = xrt_net_int_arg(deadline_value);
    int64_t direction = xrt_net_int_arg(direction_value);
    if (!conn || conn->base.closed || deadline < 0 || direction < 0 || direction > 2)
        return XR_FALSE_VAL;
    if (direction != 1)
        conn->read_deadline_ms = deadline;
    if (direction != 0)
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

static inline XrValue xrt_net_last_code(XrValue handle_value) {
    xrt_net_handle_base_t *base = xrt_net_handle_base_ptr(handle_value);
    return XR_FROM_INT(base ? (int64_t) base->last_error : (int64_t) XRT_NETERR_INVALID);
}

static inline XrValue xrt_net_last_errno(XrValue handle_value) {
    xrt_net_handle_base_t *base = xrt_net_handle_base_ptr(handle_value);
    return XR_FROM_INT(base ? base->last_errno : 0);
}

static inline XrValue xrt_net_has_tls(void) {
    return XR_FALSE_VAL;
}

/*
 * net.__tlsHandshake(conn, hostname, timeoutMs) -> int
 * The standalone AOT runtime carries no TLS engine, so a valid open TCP conn
 * still answers with the TLS-unavailable code; anything else is invalid.
 */
static inline XrValue xrt_net_tls_handshake(XrValue conn_value, XrValue host_value,
                                            XrValue deadline_value) {
    (void) host_value;
    (void) deadline_value;
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    if (!conn || conn->base.closed || conn->base.fd == XR_INVALID_SOCKET ||
        conn->conn_kind != XRT_NETCONN_TCP) {
        if (conn) {
            xrt_net_set_error_base(&conn->base, XRT_NETERR_INVALID, 0);
            xrt_net_close_base(&conn->base);
        }
        return XR_FROM_INT(XRT_NETERR_INVALID);
    }
    /* Failure contract: the conn is closed and only the code comes back, so
     * the script layer never holds a half-upgraded handle. */
    xrt_net_set_error_base(&conn->base, XRT_NETERR_TLS, 0);
    xrt_net_close_base(&conn->base);
    return XR_FROM_INT(XRT_NETERR_TLS);
}

/*
 * net.__udpBind(port, addr) -> NetConn | null
 * Empty addr binds the family-appropriate wildcard; a ':' in the addr text
 * selects IPv6, anything else IPv4.
 */
static inline XrValue xrt_net_udp_bind(XrValue port_value, XrValue addr_value) {
    xrt_net_init_once();
    int64_t port_i = xrt_net_int_arg(port_value);
    const char *addr = xrt_net_str_arg(addr_value);
    if (!addr || port_i < 0 || port_i > 65535)
        return XR_NULL_VAL;

    int family = AF_INET;
    if (addr[0] != '\0' && strchr(addr, ':'))
        family = AF_INET6;

    xr_socket_t fd = socket(family, SOCK_DGRAM, 0);
    if (fd == XR_INVALID_SOCKET)
        return XR_NULL_VAL;

    if (family == AF_INET) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t) port_i);
        if (addr[0] != '\0')
            inet_pton(AF_INET, addr, &sa.sin_addr);
        else
            sa.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) != 0) {
            xrt_net_close_fd(fd);
            return XR_NULL_VAL;
        }
    } else {
        struct sockaddr_in6 sa6;
        memset(&sa6, 0, sizeof(sa6));
        sa6.sin6_family = AF_INET6;
        sa6.sin6_port = htons((uint16_t) port_i);
        if (addr[0] != '\0')
            inet_pton(AF_INET6, addr, &sa6.sin6_addr);
        else
            sa6.sin6_addr = in6addr_any;
        if (bind(fd, (struct sockaddr *) &sa6, sizeof(sa6)) != 0) {
            xrt_net_close_fd(fd);
            return XR_NULL_VAL;
        }
    }

    xr_socket_set_nonblocking(fd);
    return xrt_net_conn_box(xrt_net_conn_new(fd, XRT_NETCONN_UDP));
}

/*
 * net.__udpSendTo(conn, data, addrLiteral, port, deadlineMs) -> int
 * Single datagram send to one literal address; bytes sent, or -1 with the
 * code stored on the conn. A zero deadline waits without limit.
 */
static inline XrValue xrt_net_udp_send_to(XrValue conn_value, XrValue data_value,
                                          XrValue addr_value, XrValue port_value,
                                          XrValue deadline_value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    if (!conn || conn->base.closed || conn->base.fd == XR_INVALID_SOCKET) {
        if (conn)
            xrt_net_set_error_base(&conn->base, XRT_NETERR_CLOSED, 0);
        return XR_FROM_INT(-1);
    }
    if (!XR_IS_ARRAY(data_value) || !data_value.ptr) {
        xrt_net_set_error_base(&conn->base, XRT_NETERR_INVALID, 0);
        return XR_FROM_INT(-1);
    }
    xrt_net_array_view_t *data = (xrt_net_array_view_t *) data_value.ptr;
    if (data->elem_type != XR_ELEM_U8 || data->elem_size != 1 || data->length < 0 ||
        (data->length > 0 && !data->data)) {
        xrt_net_set_error_base(&conn->base, XRT_NETERR_INVALID, 0);
        return XR_FROM_INT(-1);
    }

    const char *addr_text = xrt_net_str_arg(addr_value);
    int64_t port_i = xrt_net_int_arg(port_value);
    struct sockaddr_storage addr;
    socklen_t addrlen = 0;
    if (!addr_text || port_i < 0 || port_i > 65535 ||
        xrt_net_literal_sockaddr(addr_text, (int) port_i, &addr, &addrlen) == 0) {
        xrt_net_set_error_base(&conn->base, XRT_NETERR_INVALID, 0);
        return XR_FROM_INT(-1);
    }

    int64_t deadline = xrt_net_int_arg(deadline_value);
    for (;;) {
        ssize_t n = xrt_net_sendto(conn->base.fd, data->data, (size_t) data->length,
                                   (const struct sockaddr *) &addr, addrlen);
        if (n >= 0) {
            xrt_net_clear_error_base(&conn->base);
            return XR_FROM_INT((int64_t) n);
        }
        int err = xr_get_socket_error();
        if (err == XR_EINTR)
            continue;
        if (xr_socket_err_is_again(err)) {
            int ready = xrt_net_wait_fd(conn->base.fd, false, deadline);
            if (ready > 0)
                continue;
            err = xr_get_socket_error();
        }
        xrt_net_set_error_base(&conn->base, xrt_net_error_from_errno(err), err);
        return XR_FROM_INT(-1);
    }
}

/*
 * net.__udpRecvInto(conn, buffer, deadlineMs) -> int
 * Single datagram receive into a caller buffer; returns the byte count and
 * records the sender on the conn, or -1 with the code stored. A zero deadline
 * waits without limit.
 */
static inline XrValue xrt_net_udp_recv_into(XrValue conn_value, XrValue buffer_value,
                                            XrValue deadline_value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    if (!conn || conn->base.closed || conn->base.fd == XR_INVALID_SOCKET) {
        if (conn)
            xrt_net_set_error_base(&conn->base, XRT_NETERR_CLOSED, 0);
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

    int64_t deadline = xrt_net_int_arg(deadline_value);
    for (;;) {
        struct sockaddr_storage sender;
        socklen_t sender_len = sizeof(sender);
        ssize_t n = xrt_net_recvfrom(conn->base.fd, buffer->data, (size_t) buffer->capacity,
                                     (struct sockaddr *) &sender, &sender_len);
        if (n >= 0) {
            buffer->length = (int64_t) n;
            conn->udp_from_host[0] = '\0';
            conn->udp_from_port = 0;
            if (sender.ss_family == AF_INET) {
                struct sockaddr_in *sin = (struct sockaddr_in *) &sender;
                inet_ntop(AF_INET, &sin->sin_addr, conn->udp_from_host,
                          sizeof(conn->udp_from_host));
                conn->udp_from_port = ntohs(sin->sin_port);
            } else if (sender.ss_family == AF_INET6) {
                struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *) &sender;
                inet_ntop(AF_INET6, &sin6->sin6_addr, conn->udp_from_host,
                          sizeof(conn->udp_from_host));
                conn->udp_from_port = ntohs(sin6->sin6_port);
            }
            xrt_net_clear_error_base(&conn->base);
            return XR_FROM_INT((int64_t) n);
        }
        int err = xr_get_socket_error();
        if (err == XR_EINTR)
            continue;
        if (xr_socket_err_is_again(err)) {
            int ready = xrt_net_wait_fd(conn->base.fd, true, deadline);
            if (ready > 0)
                continue;
            err = xr_get_socket_error();
        }
        xrt_net_set_error_base(&conn->base, xrt_net_error_from_errno(err), err);
        return XR_FROM_INT(-1);
    }
}

/*
 * net.__udpFromHost(conn) -> string
 * Sender address of the last successful datagram receive; empty when none.
 */
static inline XrValue xrt_net_udp_from_host(XrValue conn_value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    const char *host = conn ? conn->udp_from_host : "";
    size_t length = strlen(host);
    XrValue result = xrt_str_alloc(length);
    if (length != 0)
        memcpy(xr_str_buf(result), host, length);
    return result;
}

/*
 * net.__udpFromPort(conn) -> int
 * Sender port of the last successful datagram receive; 0 when none.
 */
static inline XrValue xrt_net_udp_from_port(XrValue conn_value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    return XR_FROM_INT(conn ? conn->udp_from_port : 0);
}

static inline XrValue xrt_net_listener_port(XrValue listener_value) {
    xrt_net_listener_object_t *listener = xrt_net_listener_ptr(listener_value);
    return XR_FROM_INT(listener ? listener->port : -1);
}

static inline XrValue xrt_net_is_tls(XrValue conn_value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    return XR_FROM_BOOL(conn && conn->conn_kind == XRT_NETCONN_TLS);
}

static inline XrValue xrt_net_shutdown_direction(XrValue conn_value, XrValue direction_value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    int64_t direction = xrt_net_int_arg(direction_value);
    if (!conn || conn->base.closed || conn->base.fd == XR_INVALID_SOCKET)
        return XR_FALSE_VAL;
    if (direction < 0 || direction > 2)
        return XR_FALSE_VAL;
    int mode = direction == 0 ? XR_SHUT_RD : direction == 1 ? XR_SHUT_WR : XR_SHUT_RDWR;
    return XR_FROM_BOOL(shutdown(conn->base.fd, mode) == 0);
}

/* ==========================================================================
 * Non-blocking attempt layer for the yieldable primitives an AOT coroutine can
 * call. Each attempt either completes or reports which readiness to wait for,
 * together with the descriptor and remaining timeout; the coroutine replays it
 * after netpoll wakes up. Only the primitives the net module still exposes are
 * covered: accept, readInto, and writeBytes.
 * ========================================================================== */

static inline xrt_net_try_result_t xrt_net_accept_try(XrValue listener_value) {
    xrt_net_listener_object_t *listener = xrt_net_listener_ptr(listener_value);
    if (!listener || listener->base.closed || listener->base.fd == XR_INVALID_SOCKET) {
        if (listener)
            xrt_net_set_error_base(&listener->base, XRT_NETERR_CLOSED, 0);
        return xrt_net_try_done(XR_NULL_VAL, 0);
    }
    for (;;) {
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
        xr_socket_t fd = accept(listener->base.fd, (struct sockaddr *) &addr, &addrlen);
        if (fd != XR_INVALID_SOCKET) {
            xr_socket_set_nonblocking(fd);
            xr_socket_set_nodelay(fd, true);
            xrt_net_clear_error_base(&listener->base);
            return xrt_net_try_done(xrt_net_conn_box(xrt_net_conn_new(fd, XRT_NETCONN_TCP)), 0);
        }
        int err = xr_get_socket_error();
        if (xr_socket_err_is_again(err))
            return xrt_net_try_wait(XRT_NET_TRY_WAIT_READ, listener->base.fd,
                                    listener->accept_deadline_ms, 0);
        if (err == XR_EINTR)
            continue;
        xrt_net_set_error_base(&listener->base, xrt_net_error_from_errno(err), err);
        return xrt_net_try_done(XR_NULL_VAL, 0);
    }
}

static inline xrt_net_try_result_t xrt_net_write_try(XrValue conn_value, const char *data,
                                                     int64_t len, int64_t progress) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    if (!conn || conn->base.closed || conn->base.fd == XR_INVALID_SOCKET) {
        if (conn)
            xrt_net_set_error_base(&conn->base, XRT_NETERR_CLOSED, 0);
        return xrt_net_try_done(XR_FROM_INT(progress > 0 ? progress : -1), progress);
    }
    if (conn->conn_kind == XRT_NETCONN_TLS) {
        xrt_net_set_error_base(&conn->base, XRT_NETERR_TLS, 0);
        return xrt_net_try_done(XR_FROM_INT(progress > 0 ? progress : -1), progress);
    }
    if ((!data && len > 0) || progress < 0 || progress > len) {
        xrt_net_set_error_base(&conn->base, XRT_NETERR_INVALID, 0);
        return xrt_net_try_done(XR_FROM_INT(progress > 0 ? progress : -1), progress);
    }
    while (progress < len) {
        ssize_t n = xr_socket_send(conn->base.fd, data + progress, (size_t) (len - progress));
        if (n > 0) {
            progress += n;
            continue;
        }
        if (n == 0)
            break;
        int err = xr_get_socket_error();
        if (err == XR_EINTR)
            continue;
        if (xr_socket_err_is_again(err))
            return xrt_net_try_wait(XRT_NET_TRY_WAIT_WRITE, conn->base.fd, conn->write_deadline_ms,
                                    progress);
        xrt_net_set_error_base(&conn->base, xrt_net_error_from_errno(err), err);
        return xrt_net_try_done(XR_FROM_INT(progress > 0 ? progress : -1), progress);
    }
    if (progress == len)
        xrt_net_clear_error_base(&conn->base);
    return xrt_net_try_done(XR_FROM_INT(progress > 0 || len == 0 ? progress : -1), progress);
}

static inline xrt_net_try_result_t xrt_net_write_bytes_try(XrValue conn_value, XrValue data_value,
                                                           int64_t progress) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    if (!XR_IS_ARRAY(data_value) || !data_value.ptr) {
        if (conn)
            xrt_net_set_error_base(&conn->base, XRT_NETERR_INVALID, 0);
        return xrt_net_try_done(XR_FROM_INT(-1), progress);
    }
    xrt_net_array_view_t *data = (xrt_net_array_view_t *) data_value.ptr;
    if (data->elem_type != XR_ELEM_U8 || data->elem_size != 1 || data->length < 0 ||
        (data->length > 0 && !data->data)) {
        if (conn)
            xrt_net_set_error_base(&conn->base, XRT_NETERR_INVALID, 0);
        return xrt_net_try_done(XR_FROM_INT(-1), progress);
    }
    return xrt_net_write_try(conn_value, (const char *) data->data, data->length, progress);
}

/* readInto fills a caller-supplied Array<u8>; it never allocates, so the
 * attempt reports the byte count and leaves the data in the caller's buffer. */
static inline xrt_net_try_result_t xrt_net_read_into_try(XrValue conn_value, XrValue buffer_value,
                                                         XrValue maxlen_value) {
    xrt_net_conn_object_t *conn = xrt_net_conn_ptr(conn_value);
    if (!conn || conn->base.closed || conn->base.fd == XR_INVALID_SOCKET) {
        if (conn)
            xrt_net_set_error_base(&conn->base, XRT_NETERR_CLOSED, 0);
        return xrt_net_try_done(XR_FROM_INT(-1), 0);
    }
    if (conn->conn_kind == XRT_NETCONN_TLS) {
        xrt_net_set_error_base(&conn->base, XRT_NETERR_TLS, 0);
        return xrt_net_try_done(XR_FROM_INT(-1), 0);
    }
    if (!XR_IS_ARRAY(buffer_value) || !buffer_value.ptr) {
        xrt_net_set_error_base(&conn->base, XRT_NETERR_INVALID, 0);
        return xrt_net_try_done(XR_FROM_INT(-1), 0);
    }

    xrt_net_array_view_t *buffer = (xrt_net_array_view_t *) buffer_value.ptr;
    if (buffer->elem_type != XR_ELEM_U8 || buffer->elem_size != 1 || buffer->capacity <= 0 ||
        !buffer->data) {
        xrt_net_set_error_base(&conn->base, XRT_NETERR_INVALID, 0);
        return xrt_net_try_done(XR_FROM_INT(-1), 0);
    }

    int64_t requested = xrt_net_int_arg(maxlen_value);
    if (requested <= 0 || requested > buffer->capacity)
        requested = buffer->capacity;

    for (;;) {
        ssize_t n = xr_socket_recv(conn->base.fd, (char *) buffer->data, (size_t) requested);
        if (n >= 0) {
            buffer->length = (int64_t) n;
            xrt_net_clear_error_base(&conn->base);
            return xrt_net_try_done(XR_FROM_INT((int64_t) n), 0);
        }
        int err = xr_get_socket_error();
        if (err == XR_EINTR)
            continue;
        if (xr_socket_err_is_again(err))
            return xrt_net_try_wait(XRT_NET_TRY_WAIT_READ, conn->base.fd, conn->read_deadline_ms,
                                    0);
        xrt_net_set_error_base(&conn->base, xrt_net_error_from_errno(err), err);
        return xrt_net_try_done(XR_FROM_INT(-1), 0);
    }
}

#endif  // XRT_NET_H
