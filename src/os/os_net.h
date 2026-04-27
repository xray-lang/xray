/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_net.h - Cross-platform socket abstraction (header only).
 *
 * KEY CONCEPT:
 *   Single header that hides Winsock vs BSD-sockets differences
 *   behind a small API: xr_socket_t handles, xr_socket_recv /
 *   xr_socket_send, set_nonblocking / set_nodelay / set_reuseaddr,
 *   error-code translation (XR_EAGAIN etc.), shutdown directions,
 *   iovec + writev shim on Windows, and Winsock startup helpers.
 *
 *   All callers needing socket I/O include this header instead of
 *   <sys/socket.h> + <netinet/in.h> + <winsock2.h> + .... Only
 *   src/os/{unix,win} may pull the raw OS headers; everyone else
 *   talks to this interface.
 *
 * IMPLEMENTATION NOTE:
 *   The non-trivial helpers stay static-inline rather than out-of-
 *   line C functions because they are called from hot socket
 *   paths and the inlined branches collapse to a single recv/send
 *   on each platform. Cost of inclusion is one branch resolved at
 *   compile time per call site.
 */

#ifndef XRAY_OS_NET_H
#define XRAY_OS_NET_H

#include "../base/xplatform.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef XR_OS_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")

// MSVC's <sys/types.h> does not define ssize_t; provide it before
// any inline helper that returns one.
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
#ifdef _WIN64
typedef __int64 ssize_t;
#else
typedef int ssize_t;
#endif
#endif

typedef SOCKET xr_socket_t;
#define XR_INVALID_SOCKET INVALID_SOCKET
#define XR_SOCKET_ERROR SOCKET_ERROR

#define xr_closesocket closesocket
#define xr_ioctlsocket ioctlsocket

#define XR_EAGAIN WSAEWOULDBLOCK
#define XR_EWOULDBLOCK WSAEWOULDBLOCK
#define XR_EINPROGRESS WSAEINPROGRESS
#define XR_ECONNREFUSED WSAECONNREFUSED
#define XR_ETIMEDOUT WSAETIMEDOUT

#define XR_SHUT_RD SD_RECEIVE
#define XR_SHUT_WR SD_SEND
#define XR_SHUT_RDWR SD_BOTH

static inline int xr_get_socket_error(void) {
    return WSAGetLastError();
}

static inline void xr_set_socket_error(int err) {
    WSASetLastError(err);
}

typedef int xr_fd_t;
#define XR_INVALID_FD (-1)

// Winsock startup (POSIX has no analog; the helpers below match
// the Windows API shape so callers do not need their own #ifdef).
static inline int xr_winsock_init(void) {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa);
}

static inline void xr_winsock_cleanup(void) {
    WSACleanup();
}

// writev emulation on top of WSASend.
struct iovec {
    void *iov_base;
    size_t iov_len;
};

static inline ssize_t writev(xr_socket_t fd, const struct iovec *iov, int iovcnt) {
    DWORD sent = 0;
    WSABUF *bufs = (WSABUF *) _alloca(iovcnt * sizeof(WSABUF));
    for (int i = 0; i < iovcnt; i++) {
        bufs[i].buf = (char *) iov[i].iov_base;
        bufs[i].len = (ULONG) iov[i].iov_len;
    }
    if (WSASend(fd, bufs, iovcnt, &sent, 0, NULL, NULL) == SOCKET_ERROR)
        return -1;
    return (ssize_t) sent;
}

#else  // XR_OS_WINDOWS

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>

typedef int xr_socket_t;
#define XR_INVALID_SOCKET (-1)
#define XR_SOCKET_ERROR (-1)

#define xr_closesocket close

#define XR_EAGAIN EAGAIN
#define XR_EWOULDBLOCK EWOULDBLOCK
#define XR_EINPROGRESS EINPROGRESS
#define XR_ECONNREFUSED ECONNREFUSED
#define XR_ETIMEDOUT ETIMEDOUT

#define XR_SHUT_RD SHUT_RD
#define XR_SHUT_WR SHUT_WR
#define XR_SHUT_RDWR SHUT_RDWR

static inline int xr_get_socket_error(void) {
    return errno;
}

static inline void xr_set_socket_error(int err) {
    errno = err;
}

typedef int xr_fd_t;
#define XR_INVALID_FD (-1)

static inline int xr_winsock_init(void) {
    return 0;
}

static inline void xr_winsock_cleanup(void) {
}

#endif  // XR_OS_WINDOWS

/* ========== Common socket helpers ========== */

// Read on a socket. POSIX read() works on any fd (sockets, pipes,
// files); Windows ReadFile cannot operate on a SOCKET, and
// _read() on a SOCKET fails — we must call recv().
static inline ssize_t xr_socket_recv(xr_socket_t fd, void *buf, size_t len) {
#ifdef XR_OS_WINDOWS
    int n = recv(fd, (char *) buf, (int) len, 0);
    return (n == SOCKET_ERROR) ? -1 : (ssize_t) n;
#else
    return read(fd, buf, len);
#endif
}

static inline ssize_t xr_socket_send(xr_socket_t fd, const void *buf, size_t len) {
#ifdef XR_OS_WINDOWS
    int n = send(fd, (const char *) buf, (int) len, 0);
    return (n == SOCKET_ERROR) ? -1 : (ssize_t) n;
#else
    return write(fd, buf, len);
#endif
}

// Check whether the most recent socket error means the operation
// would block (caller should rearm netpoll and retry). EAGAIN and
// EWOULDBLOCK are usually equal but the standard does not require
// it.
static inline bool xr_socket_err_is_again(int err) {
    return err == XR_EAGAIN || err == XR_EWOULDBLOCK;
}

static inline int xr_socket_set_nonblocking(xr_socket_t fd) {
#ifdef XR_OS_WINDOWS
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

static inline int xr_socket_set_blocking(xr_socket_t fd) {
#ifdef XR_OS_WINDOWS
    u_long mode = 0;
    return ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
#endif
}

static inline int xr_socket_set_nodelay(xr_socket_t fd, bool enable) {
    int flag = enable ? 1 : 0;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *) &flag, sizeof(flag));
}

static inline int xr_socket_set_reuseaddr(xr_socket_t fd, bool enable) {
    int flag = enable ? 1 : 0;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &flag, sizeof(flag));
}

static inline int xr_socket_set_keepalive(xr_socket_t fd, bool enable) {
    int flag = enable ? 1 : 0;
    return setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char *) &flag, sizeof(flag));
}

// SO_REUSEPORT is a POSIX extension; silently no-op on Windows.
static inline int xr_socket_set_reuseport(xr_socket_t fd, bool enable) {
#if defined(SO_REUSEPORT) && !defined(XR_OS_WINDOWS)
    int flag = enable ? 1 : 0;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof(flag));
#else
    (void) fd;
    (void) enable;
    return 0;
#endif
}

static inline int xr_socket_get_error(xr_socket_t fd) {
    int error = 0;
    socklen_t len = sizeof(error);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *) &error, &len);
    return error;
}

// Synchronous recv with a select()-based timeout. Returns -1 and
// sets XR_ETIMEDOUT if the deadline passes with no data. Avoid in
// hot async paths; netpoll is the right tool there.
static inline ssize_t xr_recv_timeout(xr_socket_t fd, void *buf, size_t len, int timeout_ms) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select((int) fd + 1, &readfds, NULL, NULL, &tv);
    if (ret < 0)
        return -1;
    if (ret == 0) {
        xr_set_socket_error(XR_ETIMEDOUT);
        return -1;
    }
    return recv(fd, buf, len, 0);
}

static inline ssize_t xr_send_timeout(xr_socket_t fd, const void *buf, size_t len, int timeout_ms) {
    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(fd, &writefds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select((int) fd + 1, NULL, &writefds, NULL, &tv);
    if (ret < 0)
        return -1;
    if (ret == 0) {
        xr_set_socket_error(XR_ETIMEDOUT);
        return -1;
    }
    return send(fd, buf, len, 0);
}

// Loop until the entire buffer is sent or one timed send fails.
static inline int xr_send_all(xr_socket_t fd, const void *buf, size_t len, int timeout_ms) {
    const char *ptr = (const char *) buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = xr_send_timeout(fd, ptr + sent, len - sent, timeout_ms);
        if (n <= 0)
            return -1;
        sent += n;
    }
    return 0;
}

#endif  // XRAY_OS_NET_H
