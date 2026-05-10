/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * netpoll_iocp.c - IOCP backend (Windows)
 *
 * KEY CONCEPT:
 *   Ops-based IOCP backend for xnetpoll. Provides XrNetpollOps function
 *   pointer table consumed by xnetpoll.c for runtime backend dispatch.
 *
 * SCOPE:
 *   This file currently exposes the ops shape and IOCP handle / Winsock
 *   lifecycle only. Readiness translation via \Device\Afd lands in the
 *   next change set; until then iocp_add_fd reports failure so the
 *   runtime surfaces the gap immediately rather than silently accepting
 *   fds it cannot poll.
 *
 * Note: This file is #included by xnetpoll.c, not compiled separately.
 */

#if defined(XR_OS_WINDOWS) && defined(XR_NETPOLL_INCLUDED)

#include "../../base/xchecks.h"

// winsock2.h must precede windows.h: the legacy <winsock.h> that
// <windows.h> would otherwise pull in collides on the same struct
// names (sockaddr_in, fd_set, ...) and produces a wall of redefinition
// errors. WIN32_LEAN_AND_MEAN further narrows what windows.h drags in.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

// Sentinel completion key reserved for backend self-wakeup.
//
// Real fds register their XrPollDesc* as the completion key, which is
// always a non-NULL heap pointer; 0 cleanly identifies the
// PostQueuedCompletionStatus(NULL) packets emitted by iocp_wakeup, so
// the dispatch loop can drop them without consulting any side table.
#define XR_IOCP_KEY_WAKEUP ((ULONG_PTR) 0)

// XrNetpoll has a generic int poll_fd (used by epoll/kqueue) and a
// void* backend_state for everything else; the IOCP HANDLE is 64-bit
// on Win64 so it must live in backend_state, not be squeezed into
// poll_fd. iocp_handle() centralises the cast.
static inline HANDLE iocp_handle(XrNetpoll *np) {
    return (HANDLE) np->backend_state;
}

// ========== IOCP backend ops ==========

static int iocp_init(XrNetpoll *np) {
    // WSAStartup is reference-counted by the OS; pairing it with
    // WSACleanup on the cleanup path keeps the ref count balanced
    // even if multiple netpolls are spun up across the lifetime of
    // a process (tests, isolated runtimes, etc.).
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return -1;
    }

    HANDLE h = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (h == NULL) {
        WSACleanup();
        return -1;
    }

    np->backend_state = h;
    np->poll_fd = -1;  // unused on Windows; HANDLE lives in backend_state
    return 0;
}

static void iocp_cleanup(XrNetpoll *np) {
    HANDLE h = iocp_handle(np);
    if (h != NULL) {
        CloseHandle(h);
        np->backend_state = NULL;
    }
    WSACleanup();
}

static int iocp_add_fd(XrNetpoll *np, int fd, XrPollDesc *pd) {
    (void) np;
    (void) fd;
    (void) pd;
    // Real readiness wiring requires \Device\Afd registration, which
    // is the next change set. Returning -1 here makes xr_netpoll_open
    // fail loudly so any caller that exercises networking surfaces
    // the missing backend at first use rather than silently hanging
    // on a poll that never arms. Ops-only skeleton until then.
    return -1;
}

static void iocp_del_fd(XrNetpoll *np, int fd) {
    (void) np;
    (void) fd;
    // Companion to iocp_add_fd; nothing registered means nothing to
    // unwind until AFD wiring lands.
}

static int iocp_poll_events(XrNetpoll *np, int64_t delta_ns, XrReadyList *list) {
    HANDLE h = iocp_handle(np);
    if (h == NULL)
        return -1;

    DWORD timeout_ms;
    if (delta_ns < 0) {
        timeout_ms = INFINITE;
    } else if (delta_ns == 0) {
        timeout_ms = 0;
    } else {
        // ns -> ms with a floor of 1ms: GetQueuedCompletionStatusEx
        // treats 0 as "poll, do not wait", so any positive nanosecond
        // budget below 1ms must round up rather than collapse to a
        // non-blocking probe.
        int64_t ms = delta_ns / 1000000;
        timeout_ms = (ms <= 0) ? 1 : (DWORD) ms;
    }

    OVERLAPPED_ENTRY entries[128];
    ULONG count = 0;
    BOOL ok =
        GetQueuedCompletionStatusEx(h, entries, 128, &count, timeout_ms, FALSE);

    if (!ok) {
        return (GetLastError() == WAIT_TIMEOUT) ? 0 : -1;
    }

    for (ULONG i = 0; i < count; i++) {
        ULONG_PTR key = entries[i].lpCompletionKey;

        if (key == XR_IOCP_KEY_WAKEUP) {
            // Self-wakeup posted by iocp_wakeup. Reset the
            // duplicate-suppression flag so the next break can
            // re-arm; do not surface this to the ready list.
            atomic_store(&np->break_pending, false);
            continue;
        }

        // Real socket completions get translated into IN/OUT
        // readiness when the AFD wiring lands. Until then any
        // non-wakeup completion is dropped on purpose rather than
        // forwarded as a bogus "both ready" guess (the prior
        // implementation's silent corruption mode).
        (void) entries[i];
        (void) list;
    }

    return (int) count;
}

static void iocp_wakeup(XrNetpoll *np) {
    HANDLE h = iocp_handle(np);
    if (h == NULL)
        return;

    // Coalesce concurrent wakeups: only one PostQueuedCompletionStatus
    // is in flight at a time. The flag is cleared in iocp_poll_events
    // when the wakeup packet is dequeued.
    bool expected = false;
    if (!atomic_compare_exchange_strong(&np->break_pending, &expected, true))
        return;

    PostQueuedCompletionStatus(h, 0, XR_IOCP_KEY_WAKEUP, NULL);
}

static const XrNetpollOps iocp_ops = {
    .name = "iocp",
    .init = iocp_init,
    .cleanup = iocp_cleanup,
    .add_fd = iocp_add_fd,
    .del_fd = iocp_del_fd,
    .poll_events = iocp_poll_events,
    .wakeup = iocp_wakeup,
};

#endif  // XR_OS_WINDOWS
