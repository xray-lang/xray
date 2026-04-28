/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xpoll.h - Cross-platform I/O multiplexing abstraction (zero-overhead)
 *
 * DESIGN:
 *   This is the LOW-LEVEL platform abstraction extracted from xnetpoll.
 *   It provides pure I/O multiplexing without coroutine integration.
 *
 *   - Header-only with static inline functions (zero overhead)
 *   - All optimizations from xnetpoll preserved
 *   - Can be used by LSP (simple event loop) or xnetpoll (coroutine scheduler)
 *
 * ARCHITECTURE:
 *   ┌─────────────────────────────────────────────────────────┐
 *   │                     Users                               │
 *   ├─────────────────────────┬───────────────────────────────┤
 *   │       xnetpoll          │           LSP server          │
 *   │  (coroutine scheduler)  │        (event loop)           │
 *   │  + thread safety        │  + simple polling             │
 *   │  + descriptor pool      │                               │
 *   │  + fd_map               │                               │
 *   ├─────────────────────────┴───────────────────────────────┤
 *   │                    xpoll.h (THIS FILE)                  │
 *   │            Pure platform I/O multiplexing               │
 *   └─────────────────────────────────────────────────────────┘
 *
 * SUPPORTED BACKENDS:
 *   - epoll (Linux) - edge-triggered mode
 *   - kqueue (macOS/BSD) - edge-triggered mode
 *   - IOCP (Windows) - completion port model
 *   - select (fallback) - level-triggered
 */

#ifndef XPOLL_H
#define XPOLL_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include "../base/xplatform.h"

// ============================================================================
// Platform Detection
// ============================================================================

#if defined(XR_OS_LINUX)
#define XR_POLL_EPOLL 1
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#elif defined(XR_OS_MACOS) || defined(XR_OS_BSD) || defined(XR_OS_BSD) || defined(XR_OS_BSD)
#define XR_POLL_KQUEUE 1
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#elif defined(XR_OS_WINDOWS)
#define XR_POLL_IOCP 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#define XR_POLL_SELECT 1
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#endif

// ============================================================================
// Event Flags (compatible with xnetpoll)
// ============================================================================

#define XR_POLL_IN 0x01   // Readable
#define XR_POLL_OUT 0x02  // Writable
#define XR_POLL_ERR 0x04  // Error
#define XR_POLL_HUP 0x08  // Hangup

// ============================================================================
// Configuration
// ============================================================================

#ifndef XR_POLL_MAX_EVENTS
#define XR_POLL_MAX_EVENTS 128  // Max events per poll (same as xnetpoll)
#endif

#ifndef XR_POLL_MAX_FDS
#define XR_POLL_MAX_FDS 64  // Max tracked fds for simple use cases
#endif

// ============================================================================
// Types
// ============================================================================

// Event structure returned by poll
typedef struct XrPollEvent {
    int fd;           // File descriptor
    int events;       // Ready events (XR_POLL_IN/OUT/ERR/HUP)
    void *user_data;  // User data associated with fd
} XrPollEvent;

// Tracked fd entry (for maintaining fd -> user_data mapping)
typedef struct XrPollEntry {
    int fd;
    int events;
    void *user_data;
    bool active;
} XrPollEntry;

// Main poll structure
typedef struct XrPoll {
    bool initialized;

    // Platform-specific handle
#if defined(XR_POLL_EPOLL)
    int epfd;
#elif defined(XR_POLL_KQUEUE)
    int kq;
#elif defined(XR_POLL_IOCP)
    HANDLE iocp;
    bool winsock_inited;
#endif

    // Wakeup mechanism (from xnetpoll: pipe for interrupt)
    int wakeup_pipe[2];
    bool wakeup_pending;  // Avoid duplicate wakeup (xnetpoll optimization)

    // fd tracking (for del and user_data lookup)
    XrPollEntry entries[XR_POLL_MAX_FDS];
    int entry_count;
} XrPoll;

// ============================================================================
// Platform Utilities (extracted from xnetpoll.c)
// ============================================================================

// Set fd to non-blocking mode
static inline int xr_poll_set_nonblock(int fd) {
#ifdef XR_OS_WINDOWS
    u_long mode = 1;
    return ioctlsocket((SOCKET) fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

// Create wakeup pipe (from xnetpoll.c: create_wakeup_pipe)
// Key: non-blocking to avoid deadlock
static inline int xr_poll_create_wakeup_pipe(int pipe_fds[2]) {
#ifdef XR_OS_WINDOWS
    // Windows: create self-connected TCP socket pair
    // (xnetpoll_iocp uses PostQueuedCompletionStatus, but for select we need pipe)

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // Let OS choose port

    if (bind(listener, (struct sockaddr *) &addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listener);
        return -1;
    }

    int addrlen = sizeof(addr);
    if (getsockname(listener, (struct sockaddr *) &addr, &addrlen) == SOCKET_ERROR) {
        closesocket(listener);
        return -1;
    }

    if (listen(listener, 1) == SOCKET_ERROR) {
        closesocket(listener);
        return -1;
    }

    // Create client socket (write end)
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET) {
        closesocket(listener);
        return -1;
    }

    // Set non-blocking for connect
    u_long mode = 1;
    ioctlsocket(client, FIONBIO, &mode);

    if (connect(client, (struct sockaddr *) &addr, sizeof(addr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
            closesocket(client);
            closesocket(listener);
            return -1;
        }
    }

    // Accept connection (read end)
    SOCKET server = accept(listener, NULL, NULL);
    closesocket(listener);  // No longer needed

    if (server == INVALID_SOCKET) {
        closesocket(client);
        return -1;
    }

    // Set both to non-blocking
    mode = 1;
    ioctlsocket(client, FIONBIO, &mode);
    ioctlsocket(server, FIONBIO, &mode);

    pipe_fds[0] = (int) server;  // Read end
    pipe_fds[1] = (int) client;  // Write end
    return 0;
#else
    // Unix: use pipe() (from xnetpoll.c)
    if (pipe(pipe_fds) < 0) {
        return -1;
    }

    // Set non-blocking (from xnetpoll.c)
    fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
    fcntl(pipe_fds[1], F_SETFL, O_NONBLOCK);

    return 0;
#endif
}

// Close wakeup pipe (from xnetpoll.c: close_wakeup_pipe)
static inline void xr_poll_close_wakeup_pipe(int pipe_fds[2]) {
#ifdef XR_OS_WINDOWS
    if (pipe_fds[0] >= 0)
        closesocket((SOCKET) pipe_fds[0]);
    if (pipe_fds[1] >= 0)
        closesocket((SOCKET) pipe_fds[1]);
#else
    if (pipe_fds[0] >= 0)
        close(pipe_fds[0]);
    if (pipe_fds[1] >= 0)
        close(pipe_fds[1]);
#endif
    pipe_fds[0] = pipe_fds[1] = -1;
}

// Drain wakeup pipe (from xnetpoll_epoll.c/kqueue.c)
static inline void xr_poll_drain_wakeup(int fd) {
#ifdef XR_OS_WINDOWS
    char buf[16];
    while (recv((SOCKET) fd, buf, sizeof(buf), 0) > 0) {
    }
#else
    char buf[16];
    while (read(fd, buf, sizeof(buf)) > 0) {
    }
#endif
}

// Write to wakeup pipe (from xnetpoll_epoll.c: xr_netpoll_break)
static inline void xr_poll_signal_wakeup(int fd) {
#ifdef XR_OS_WINDOWS
    char c = 0;
    send((SOCKET) fd, &c, 1, 0);
#else
    char c = 0;
    ssize_t n;
    do {
        n = write(fd, &c, 1);
    } while (n < 0 && errno == EINTR);
#endif
}

// Platform backend implementations (split into separate headers)
#if defined(XR_POLL_EPOLL)
#include "os_poll_epoll.h"
#elif defined(XR_POLL_KQUEUE)
#include "os_poll_kqueue.h"
#elif defined(XR_POLL_IOCP)
#include "os_poll_iocp.h"
#else
#include "os_poll_select.h"
#endif

static inline int xr_poll_get_wakeup_fd(XrPoll *p) {
    return p->wakeup_pipe[0];
}

#endif  // XPOLL_H
