/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xpoll_iocp.h - IOCP backend for xpoll (Windows)
 */

#ifndef XPOLL_IOCP_H
#define XPOLL_IOCP_H

// ============================================================================
// Platform: IOCP (Windows) — lightweight transport-level poll wrapper
// ============================================================================
//
// KNOWN BUG (XR_BUG_OS_POLL_IOCP_FAKE_READINESS):
//   xr_poll_wait below treats every IOCP completion as
//   "both readable and writable" (XR_POLL_IN | XR_POLL_OUT). This
//   is incorrect under any real workload:
//
//     - A plain CreateIoCompletionPort association on a SOCKET only
//       delivers completions when the user-mode code itself issues
//       overlapped I/O (WSARecv / WSASend with an OVERLAPPED). Until
//       that happens the IOCP queue is empty; nothing here surfaces
//       readiness.
//     - When a completion does arrive, lpCompletionKey identifies
//       the user_data pointer but says nothing about IN vs OUT;
//       the dwNumberOfBytesTransferred + lpOverlapped tell us which
//       request completed and how, and that distinction is dropped.
//
//   Coroutine-side I/O does NOT go through this header — the
//   netpoll backend (src/os/netpoll/netpoll_iocp.c) implements a
//   correct \Device\Afd-based readiness translation and is the path
//   used by stdlib net / http / ws. The lightweight xpoll lives in
//   the LSP / MCP / CLI transports, where the traffic is small and
//   single-direction enough that the misreport has not surfaced as
//   a visible failure.
//
//   Fixing this means rebuilding xpoll on top of the AFD wrapper
//   (or at minimum keeping a per-fd "what did the caller actually
//   request" map and decoding bytes_transferred). Tracked as a
//   separate piece of work; the netpoll-focused IOCP rewrite leaves
//   it untouched on purpose so the change set stays scoped.

static inline int xr_poll_init(XrPoll *p) {
    memset(p, 0, sizeof(XrPoll));

    // Initialize Winsock (from xnetpoll_iocp.c)
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return -1;
    }
    p->winsock_inited = true;

    // Create IOCP (from xnetpoll_iocp.c)
    p->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (p->iocp == NULL) {
        WSACleanup();
        p->winsock_inited = false;
        return -1;
    }

    p->wakeup_pipe[0] = -1;
    p->wakeup_pipe[1] = -1;
    p->wakeup_pending = false;
    p->initialized = true;

    return 0;
}

static inline void xr_poll_destroy(XrPoll *p) {
    if (!p->initialized)
        return;

    if (p->iocp != NULL) {
        CloseHandle(p->iocp);
        p->iocp = NULL;
    }

    if (p->winsock_inited) {
        WSACleanup();
        p->winsock_inited = false;
    }

    p->initialized = false;
}

static inline int xr_poll_add(XrPoll *p, int fd, int events, void *user_data) {
    if (!p->initialized)
        return -1;
    if (p->entry_count >= XR_POLL_MAX_FDS)
        return -1;

    // Associate with IOCP (from xnetpoll_iocp.c: xr_netpoll_open)
    HANDLE h = CreateIoCompletionPort((HANDLE) (intptr_t) fd, p->iocp, (ULONG_PTR) user_data, 0);
    if (h == NULL) {
        return -1;
    }

    // Track entry
    XrPollEntry *entry = &p->entries[p->entry_count++];
    entry->fd = fd;
    entry->events = events;
    entry->user_data = user_data;
    entry->active = true;

    return 0;
}

static inline int xr_poll_del(XrPoll *p, int fd) {
    if (!p->initialized)
        return -1;

    // IOCP doesn't need explicit removal (from xnetpoll_iocp.c)

    // Remove from entries
    for (int i = 0; i < p->entry_count; i++) {
        if (p->entries[i].fd == fd && p->entries[i].active) {
            p->entries[i].active = false;
            break;
        }
    }

    return 0;
}

static inline int xr_poll_wait(XrPoll *p, XrPollEvent *out, int max_events, int timeout_ms) {
    if (!p->initialized)
        return -1;
    if (max_events > XR_POLL_MAX_EVENTS)
        max_events = XR_POLL_MAX_EVENTS;

    // Calculate timeout (from xnetpoll_iocp.c: xr_netpoll_poll)
    DWORD timeout;
    if (timeout_ms < 0) {
        timeout = INFINITE;
    } else {
        timeout = (DWORD) timeout_ms;
    }

    // Get completion events (from xnetpoll_iocp.c)
    OVERLAPPED_ENTRY entries[XR_POLL_MAX_EVENTS];
    ULONG count = 0;

    BOOL ok =
        GetQueuedCompletionStatusEx(p->iocp, entries, (ULONG) max_events, &count, timeout, FALSE);

    if (!ok) {
        DWORD err = GetLastError();
        if (err == WAIT_TIMEOUT)
            return 0;
        return -1;
    }

    // Process completion events (from xnetpoll_iocp.c)
    int result = 0;
    for (ULONG i = 0; i < count; i++) {
        // Check for wakeup (NULL completion key)
        if (entries[i].lpCompletionKey == 0) {
            p->wakeup_pending = false;
            continue;
        }

        out[result].user_data = (void *) entries[i].lpCompletionKey;
        out[result].events = XR_POLL_IN | XR_POLL_OUT;  // IOCP: assume both ready
        out[result].fd = -1;                            // Need to find from entries

        // Find fd
        for (int j = 0; j < p->entry_count; j++) {
            if (p->entries[j].user_data == out[result].user_data && p->entries[j].active) {
                out[result].fd = p->entries[j].fd;
                break;
            }
        }

        result++;
    }

    return result;
}

static inline void xr_poll_wakeup(XrPoll *p) {
    if (!p->initialized)
        return;

    // Avoid duplicate wakeup (from xnetpoll_iocp.c: xr_netpoll_break)
    if (p->wakeup_pending)
        return;
    p->wakeup_pending = true;

    // Send empty completion event to wake (from xnetpoll_iocp.c)
    PostQueuedCompletionStatus(p->iocp, 0, 0, NULL);
}

#endif  // XPOLL_IOCP_H
