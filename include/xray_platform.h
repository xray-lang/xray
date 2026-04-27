/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_platform.h - Cross-platform compatibility definitions (macOS/Linux/Windows)
 */

#ifndef XRAY_PLATFORM_H
#define XRAY_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

// macOS: include mach types first to avoid pthread.h type conflicts
#ifdef __APPLE__
#include <mach/mach_types.h>
#endif

#ifndef _WIN32
#include <sys/types.h>
#endif  // ========== Platform Detection ==========

#if defined(_WIN32) || defined(_WIN64)
#define XR_PLATFORM_WINDOWS 1
#elif defined(__APPLE__) && defined(__MACH__)
#define XR_PLATFORM_MACOS 1
#elif defined(__linux__)
#define XR_PLATFORM_LINUX 1
#else
#error "Unsupported platform"
#endif  // ========== Windows Platform ==========

#ifdef XR_PLATFORM_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")

// Socket types and constants
typedef SOCKET xr_socket_t;
#define XR_INVALID_SOCKET INVALID_SOCKET
#define XR_SOCKET_ERROR SOCKET_ERROR

// Socket function mappings
#define xr_closesocket closesocket
#define xr_ioctlsocket ioctlsocket

// Error codes
#define XR_EAGAIN WSAEWOULDBLOCK
#define XR_EWOULDBLOCK WSAEWOULDBLOCK
#define XR_EINPROGRESS WSAEINPROGRESS
#define XR_ECONNREFUSED WSAECONNREFUSED
#define XR_ETIMEDOUT WSAETIMEDOUT

static inline int xr_get_socket_error(void) {
    return WSAGetLastError();
}

static inline void xr_set_socket_error(int err) {
    WSASetLastError(err);
}

// File descriptor type
typedef int xr_fd_t;
#define XR_INVALID_FD (-1)

// Initialize network library (Windows requires WSAStartup)
static inline int xr_net_init(void) {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa);
}

static inline void xr_net_cleanup(void) {
    WSACleanup();
}

// writev emulation
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
    if (WSASend(fd, bufs, iovcnt, &sent, 0, NULL, NULL) == SOCKET_ERROR) {
        return -1;
    }
    return (ssize_t) sent;
}

// ssize_t definition
#ifndef ssize_t
#ifdef _WIN64
typedef __int64 ssize_t;
#else
typedef int ssize_t;
#endif
#endif  // ========== Unix Platform (macOS/Linux) ==========

#else

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>

#ifdef XR_PLATFORM_MACOS
#include <sys/event.h>
#include <stdlib.h>  // arc4random_buf
#else
#include <sys/epoll.h>
#include <sys/random.h>  // getrandom
#endif                   // Socket types and constants
typedef int xr_socket_t;
#define XR_INVALID_SOCKET (-1)
#define XR_SOCKET_ERROR (-1)

// Socket function mappings
#define xr_closesocket close

// Error codes
#define XR_EAGAIN EAGAIN
#define XR_EWOULDBLOCK EWOULDBLOCK
#define XR_EINPROGRESS EINPROGRESS
#define XR_ECONNREFUSED ECONNREFUSED
#define XR_ETIMEDOUT ETIMEDOUT

static inline int xr_get_socket_error(void) {
    return errno;
}

static inline void xr_set_socket_error(int err) {
    errno = err;
}

// File descriptor type
typedef int xr_fd_t;
#define XR_INVALID_FD (-1)

// Initialize network library (no-op on Unix)
static inline int xr_net_init(void) {
    return 0;
}

static inline void xr_net_cleanup(void) {
    // no-op
}

#endif  // ========== Common Socket Utilities ==========

/*
 * Set socket to non-blocking mode
 */
static inline int xr_socket_set_nonblocking(xr_socket_t fd) {
#ifdef XR_PLATFORM_WINDOWS
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

/*
 * Set TCP_NODELAY
 */
static inline int xr_socket_set_nodelay(xr_socket_t fd, bool enable) {
    int flag = enable ? 1 : 0;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *) &flag, sizeof(flag));
}

/*
 * Set SO_REUSEADDR
 */
static inline int xr_socket_set_reuseaddr(xr_socket_t fd, bool enable) {
    int flag = enable ? 1 : 0;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &flag, sizeof(flag));
}

/*
 * Set SO_KEEPALIVE
 */
static inline int xr_socket_set_keepalive(xr_socket_t fd, bool enable) {
    int flag = enable ? 1 : 0;
    return setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char *) &flag, sizeof(flag));
}

/*
 * Get socket error
 */
static inline int xr_socket_get_error(xr_socket_t fd) {
    int error = 0;
    socklen_t len = sizeof(error);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *) &error, &len);
    return error;
}

/*
 * Set socket to blocking mode
 */
static inline int xr_socket_set_blocking(xr_socket_t fd) {
#ifdef XR_PLATFORM_WINDOWS
    u_long mode = 0;
    return ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
#endif
}

/*
 * Set SO_REUSEPORT (Unix only)
 */
static inline int xr_socket_set_reuseport(xr_socket_t fd, bool enable) {
#if defined(SO_REUSEPORT) && !defined(XR_PLATFORM_WINDOWS)
    int flag = enable ? 1 : 0;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof(flag));
#else
    (void) fd;
    (void) enable;
    return 0;
#endif
}

/*
 * recv with timeout (using select)
 */
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

/*
 * send with timeout (using select)
 */
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

/*
 * Send all data (loop until complete or error)
 */
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

/* ========== Cryptographic Random ========== */

/*
 * Generate cryptographically secure random bytes
 */
static inline void xr_random_bytes(unsigned char *buf, size_t len) {
#ifdef XR_PLATFORM_WINDOWS
    // Windows: use BCryptGenRandom
    BCryptGenRandom(NULL, buf, (ULONG) len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#elif defined(XR_PLATFORM_MACOS)
    // macOS: use arc4random_buf
    arc4random_buf(buf, len);
#else
    // Linux: use getrandom
    size_t offset = 0;
    while (offset < len) {
        ssize_t ret = getrandom(buf + offset, len - offset, 0);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            // Fallback to /dev/urandom
            FILE *f = fopen("/dev/urandom", "rb");
            if (f) {
                fread(buf + offset, 1, len - offset, f);
                fclose(f);
            }
            break;
        }
        offset += ret;
    }
#endif
}

// Time helpers live in src/base/xtime.h (xr_time_monotonic_ns,
// xr_time_realtime_ns, xr_time_sleep_ns and the _ms / _us
// convenience wrappers). Internal callers should include that
// header directly; xray_platform.h is now socket-only.

#endif  // XRAY_PLATFORM_H
