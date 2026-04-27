/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnetpoll_iocp.c - IOCP backend (Windows)
 *
 * Windows uses I/O Completion Ports for async I/O.
 * Note: IOCP model differs from epoll/kqueue, this is a simplified impl.
 *
 * Note: This file is #included by xnetpoll.c, not compiled separately.
 */

// Only compile when included by xnetpoll.c
#if defined(XR_OS_WINDOWS) && defined(XR_NETPOLL_INCLUDED)

#include "../base/xchecks.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

// Initialize IOCP
int xr_netpoll_init(XrNetpoll *np) {
    if (atomic_load(&np->inited)) {
        return 0;
    }

    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return -1;
    }

    // Create IOCP
    np->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (np->iocp == NULL) {
        WSACleanup();
        return -1;
    }

    // Initialize cache pool
    poll_cache_init(&np->cache);

    np->wakeup_pipe[0] = -1;
    np->wakeup_pipe[1] = -1;

    atomic_store(&np->waiters, 0);
    atomic_store(&np->break_pending, false);
    atomic_store(&np->inited, true);

    return 0;
}

// Cleanup IOCP
void xr_netpoll_cleanup(XrNetpoll *np) {
    if (!atomic_load(&np->inited)) {
        return;
    }

    if (np->iocp != NULL) {
        CloseHandle(np->iocp);
        np->iocp = NULL;
    }

    poll_cache_cleanup(&np->cache);
    WSACleanup();

    atomic_store(&np->inited, false);
}

// Open fd, register to IOCP
XrPollDesc *xr_netpoll_open(XrNetpoll *np, int fd) {
    if (!atomic_load(&np->inited)) {
        return NULL;
    }

    // Allocate descriptor
    XrPollDesc *pd = xr_poll_cache_alloc(&np->cache);
    if (!pd) {
        return NULL;
    }

    pd->fd = fd;

    // Associate with IOCP
    HANDLE h = CreateIoCompletionPort((HANDLE) (intptr_t) fd, np->iocp, (ULONG_PTR) pd, 0);
    if (h == NULL) {
        xr_poll_cache_free(&np->cache, pd);
        return NULL;
    }

    return pd;
}

// Close fd, remove from IOCP
void xr_netpoll_close(XrNetpoll *np, XrPollDesc *pd) {
    if (!pd)
        return;

    atomic_store(&pd->closing, true);

    // IOCP doesn't need explicit removal

    xr_netpoll_unblock(pd, XR_POLL_READ, false);
    xr_netpoll_unblock(pd, XR_POLL_WRITE, false);

    xr_poll_cache_free(&np->cache, pd);
}

// Poll ready events
XrReadyList xr_netpoll_poll(XrNetpoll *np, int64_t delta_ns) {
    XrReadyList list;
    ready_list_init(&list);

    if (!atomic_load(&np->inited)) {
        return list;
    }

    // Calculate timeout
    DWORD timeout_ms;
    if (delta_ns < 0) {
        timeout_ms = INFINITE;
    } else if (delta_ns == 0) {
        timeout_ms = 0;
    } else {
        timeout_ms = (DWORD) (delta_ns / 1000000);
        if (timeout_ms == 0)
            timeout_ms = 1;
    }

    // Get completion events
    OVERLAPPED_ENTRY entries[128];
    ULONG count = 0;

    BOOL ok = GetQueuedCompletionStatusEx(np->iocp, entries, 128, &count, timeout_ms, FALSE);

    if (!ok) {
        if (GetLastError() != WAIT_TIMEOUT) {
            // Real error
        }
        return list;
    }

    // Process completion events
    for (ULONG i = 0; i < count; i++) {
        XrPollDesc *pd = (XrPollDesc *) entries[i].lpCompletionKey;
        if (!pd)
            continue;

        // Assume both read/write ready
        xr_netpoll_ready(&list, pd, XR_POLL_BOTH);
    }

    return list;
}

// Wake IOCP
void xr_netpoll_break(XrNetpoll *np) {
    if (!atomic_load(&np->inited)) {
        return;
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong(&np->break_pending, &expected, true)) {
        return;
    }

    // Send empty completion event to wake
    PostQueuedCompletionStatus(np->iocp, 0, 0, NULL);
}

#endif  // XR_OS_WINDOWS
