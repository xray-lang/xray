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
 *   Ops-based IOCP backend for xnetpoll. Provides XrNetpollOps
 *   function pointer table consumed by xnetpoll.c for runtime
 *   backend dispatch.
 *
 *   The backend turns IOCP completions into "fd is readable / writable"
 *   readiness events the same way kqueue/epoll do. The translation
 *   path is wepoll-style: each watched socket has a per-pd
 *   AFD_POLL_INFO + IO_STATUS_BLOCK pair which is submitted to the
 *   AFD kernel driver via IOCTL_AFD_POLL; the completion flows
 *   through the same IOCP that delivers all of the runtime's
 *   asynchronous notifications.
 *
 * SCOPE OF THIS FILE:
 *   - Lifecycle: WSAStartup / IOCP / AFD device init+teardown.
 *   - NT API loading via GetProcAddress (no link-time ntdll.lib).
 *   - AFD wrapper functions declared by netpoll_iocp_afd.h.
 *   - Skeleton for the XrNetpollOps table; per-fd readiness
 *     translation and the update queue land in the next change set.
 *
 * Note: This file is #included by xnetpoll.c, not compiled separately.
 */

#if defined(XR_OS_WINDOWS) && defined(XR_NETPOLL_INCLUDED)

#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include "netpoll_iocp_afd.h"
#include <stdatomic.h>
#include <stdbool.h>

// ============================================================================
// NT API entry points (resolved once via GetProcAddress).
// ============================================================================
//
// We deliberately do NOT link against ntdll.lib at build time:
//   1. It keeps cmake's Windows link line minimal (ws2_32 + synchronization
//      only) and avoids dragging the runtime into NT private API land
//      at the linker level.
//   2. It mirrors what wepoll / libuv / mio / Node do, so we share the
//      same compatibility surface those projects already prove on every
//      supported Windows version.

typedef NTSTATUS(NTAPI *PFN_NtCreateFile)(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
                                          POBJECT_ATTRIBUTES ObjectAttributes,
                                          PIO_STATUS_BLOCK IoStatusBlock,
                                          PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
                                          ULONG ShareAccess, ULONG CreateDisposition,
                                          ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);

typedef NTSTATUS(NTAPI *PFN_NtDeviceIoControlFile)(HANDLE FileHandle, HANDLE Event,
                                                   PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                                                   PIO_STATUS_BLOCK IoStatusBlock,
                                                   ULONG IoControlCode, PVOID InputBuffer,
                                                   ULONG InputBufferLength, PVOID OutputBuffer,
                                                   ULONG OutputBufferLength);

typedef NTSTATUS(NTAPI *PFN_NtCancelIoFileEx)(HANDLE FileHandle, PIO_STATUS_BLOCK IoRequestToCancel,
                                              PIO_STATUS_BLOCK IoStatusBlock);

typedef ULONG(WINAPI *PFN_RtlNtStatusToDosError)(NTSTATUS Status);

typedef VOID(NTAPI *PFN_RtlInitUnicodeString)(PUNICODE_STRING DestinationString,
                                              PCWSTR SourceString);

static struct {
    PFN_NtCreateFile NtCreateFile;
    PFN_NtDeviceIoControlFile NtDeviceIoControlFile;
    PFN_NtCancelIoFileEx NtCancelIoFileEx;
    PFN_RtlNtStatusToDosError RtlNtStatusToDosError;
    PFN_RtlInitUnicodeString RtlInitUnicodeString;
} g_nt_api;

static atomic_int g_nt_api_state;  // 0 = unloaded, 1 = loading, 2 = loaded, 3 = failed

// One-shot resolver. Called from xr_afd_init the first time it runs;
// subsequent calls observe state==2 and skip straight to "ready".
//
// Concurrency: the simple state machine guards against duplicate
// loads but blocks on a tight spin in the (rare) interleave window
// between the first thread's CAS and its store(2). Acceptable: the
// load runs once per process, completes in microseconds, and any
// runner that hits the spin is about to do an IOCP creation that
// dwarfs it.
static int load_nt_api(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_nt_api_state, &expected, 1)) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) {
            atomic_store(&g_nt_api_state, 3);
            return -1;
        }

#define XR_AFD_LOAD(name)                                                                          \
    do {                                                                                           \
        g_nt_api.name = (PFN_##name) (uintptr_t) GetProcAddress(ntdll, #name);                     \
        if (!g_nt_api.name) {                                                                      \
            atomic_store(&g_nt_api_state, 3);                                                      \
            return -1;                                                                             \
        }                                                                                          \
    } while (0)

        XR_AFD_LOAD(NtCreateFile);
        XR_AFD_LOAD(NtDeviceIoControlFile);
        XR_AFD_LOAD(NtCancelIoFileEx);
        XR_AFD_LOAD(RtlNtStatusToDosError);
        XR_AFD_LOAD(RtlInitUnicodeString);

#undef XR_AFD_LOAD

        atomic_store(&g_nt_api_state, 2);
        return 0;
    }

    // Another thread is loading or has already finished. Spin briefly
    // until the terminal state is reached.
    for (;;) {
        int s = atomic_load(&g_nt_api_state);
        if (s == 2)
            return 0;
        if (s == 3)
            return -1;
        // s == 1: loader still in flight; back off cheaply.
        YieldProcessor();
    }
}

// Translate an NTSTATUS into a Win32 error and stamp it into the
// thread's last-error slot. Mirrors wepoll's err_set_win_error.
static int set_error_from_nt(NTSTATUS status) {
    SetLastError(g_nt_api.RtlNtStatusToDosError(status));
    return -1;
}

// ============================================================================
// AFD context lifecycle
// ============================================================================

// Each XrNetpoll gets a unique \Device\Afd object name suffix so two
// runtimes inside the same process never share an AFD device handle
// by accident. Generated lazily from a process-wide counter.
static atomic_uint g_afd_device_seq;

int xr_afd_init(XrAfdContext *ctx, HANDLE iocp) {
    XR_DCHECK(ctx != NULL, "xr_afd_init: NULL ctx");
    XR_DCHECK(iocp != NULL, "xr_afd_init: NULL iocp");

    if (load_nt_api() < 0)
        return -1;

    ctx->iocp = iocp;
    ctx->afd_device = NULL;

    // Build a per-instance device path: \Device\Afd\Xray<seq>.
    // The exact name is irrelevant; AFD treats anything under
    // \Device\Afd\ as a fresh handle to the same driver.
    WCHAR namebuf[64];
    unsigned seq = atomic_fetch_add(&g_afd_device_seq, 1) + 1;
    int n = swprintf(namebuf, sizeof(namebuf) / sizeof(namebuf[0]), L"\\Device\\Afd\\Xray%u", seq);
    if (n < 0)
        return -1;

    UNICODE_STRING device_name;
    g_nt_api.RtlInitUnicodeString(&device_name, namebuf);

    OBJECT_ATTRIBUTES attrs;
    InitializeObjectAttributes(&attrs, &device_name, 0, NULL, NULL);

    HANDLE afd = NULL;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status = g_nt_api.NtCreateFile(&afd, SYNCHRONIZE, &attrs, &iosb, NULL, 0,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0, NULL,
                                            0);
    if (!NT_SUCCESS(status))
        return set_error_from_nt(status);

    // Associate the AFD device with our IOCP. Every poll completion
    // submitted via this device handle will be delivered through the
    // same IOCP that carries wakeups and (eventually) other backend
    // notifications.
    if (CreateIoCompletionPort(afd, iocp, 0, 0) == NULL) {
        DWORD err = GetLastError();
        CloseHandle(afd);
        SetLastError(err);
        return -1;
    }

    // FILE_SKIP_SET_EVENT_ON_HANDLE: tell the kernel not to signal the
    // file handle's internal event when the IOCTL completes; we only
    // care about the IOCP completion, and skipping the event saves a
    // kernel/user round-trip per poll.
    if (!SetFileCompletionNotificationModes(afd, FILE_SKIP_SET_EVENT_ON_HANDLE)) {
        DWORD err = GetLastError();
        CloseHandle(afd);
        SetLastError(err);
        return -1;
    }

    ctx->afd_device = afd;
    return 0;
}

void xr_afd_destroy(XrAfdContext *ctx) {
    if (!ctx)
        return;
    if (ctx->afd_device != NULL) {
        CloseHandle(ctx->afd_device);
        ctx->afd_device = NULL;
    }
    ctx->iocp = NULL;
}

int xr_afd_poll(XrAfdContext *ctx, XR_AFD_POLL_INFO *info, IO_STATUS_BLOCK *iosb) {
    XR_DCHECK(ctx != NULL, "xr_afd_poll: NULL ctx");
    XR_DCHECK(info != NULL, "xr_afd_poll: NULL info");
    XR_DCHECK(iosb != NULL, "xr_afd_poll: NULL iosb");
    if (ctx->afd_device == NULL || g_nt_api.NtDeviceIoControlFile == NULL) {
        SetLastError(ERROR_INVALID_STATE);
        return -1;
    }

    // Mark the iosb as pending so a stray inspect-before-completion
    // can distinguish a still-in-flight request from a completed one.
    iosb->Status = STATUS_PENDING;

    NTSTATUS status = g_nt_api.NtDeviceIoControlFile(ctx->afd_device, NULL, NULL, iosb, iosb,
                                                     IOCTL_AFD_POLL, info, sizeof(*info), info,
                                                     sizeof(*info));

    // STATUS_PENDING is the success case for an async submission: the
    // poll is now in flight and will land on the IOCP. STATUS_SUCCESS
    // means it completed inline (extremely rare; AFD effectively
    // always returns pending). Anything else is a real failure.
    if (status == STATUS_SUCCESS || status == STATUS_PENDING)
        return 0;
    return set_error_from_nt(status);
}

int xr_afd_cancel(XrAfdContext *ctx, IO_STATUS_BLOCK *iosb) {
    XR_DCHECK(ctx != NULL, "xr_afd_cancel: NULL ctx");
    XR_DCHECK(iosb != NULL, "xr_afd_cancel: NULL iosb");
    if (ctx->afd_device == NULL || g_nt_api.NtCancelIoFileEx == NULL) {
        SetLastError(ERROR_INVALID_STATE);
        return -1;
    }

    // If the request already completed (Status flipped off PENDING)
    // there is nothing to cancel and the cancellation API would
    // return STATUS_NOT_FOUND. Short-circuit that path so the caller
    // cannot accidentally cancel-after-completion.
    if (iosb->Status != STATUS_PENDING)
        return 0;

    IO_STATUS_BLOCK cancel_iosb;
    NTSTATUS status = g_nt_api.NtCancelIoFileEx(ctx->afd_device, iosb, &cancel_iosb);

    // STATUS_NOT_FOUND is benign: completion raced cancellation and
    // arrived first. Both terminal states are observable by the caller
    // through the original iosb.Status when the IOCP packet is dequeued.
    if (status == STATUS_SUCCESS || status == STATUS_NOT_FOUND)
        return 0;
    return set_error_from_nt(status);
}

int xr_afd_get_base_socket(SOCKET sock, SOCKET *out) {
    XR_DCHECK(out != NULL, "xr_afd_get_base_socket: NULL out");
    if (sock == INVALID_SOCKET) {
        SetLastError(WSAENOTSOCK);
        return -1;
    }

    DWORD bytes = 0;
    SOCKET base = INVALID_SOCKET;

    // SIO_BSP_HANDLE_POLL is the modern, LSP-aware ioctl: it returns
    // the SOCKET handle that should be used for the actual poll
    // operation. Available since Windows 7. Falls back to the older
    // SIO_BASE_HANDLE on systems where SIO_BSP_HANDLE_POLL is not
    // recognised by every layered service in the chain.
    if (WSAIoctl(sock, SIO_BSP_HANDLE_POLL, NULL, 0, &base, sizeof(base), &bytes, NULL, NULL) ==
            0 &&
        base != INVALID_SOCKET) {
        *out = base;
        return 0;
    }

    if (WSAIoctl(sock, SIO_BASE_HANDLE, NULL, 0, &base, sizeof(base), &bytes, NULL, NULL) == 0 &&
        base != INVALID_SOCKET) {
        *out = base;
        return 0;
    }

    return -1;
}

// ============================================================================
// IOCP backend ops
// ============================================================================
//
// XrNetpoll.backend_state owns a heap XrAfdContext for the lifetime of
// the netpoll. iocp_ctx() centralises the cast so every backend op
// can reach the IOCP and AFD device without rummaging through
// pointer arithmetic.
static inline XrAfdContext *iocp_ctx(XrNetpoll *np) {
    return (XrAfdContext *) np->backend_state;
}

// Sentinel completion key reserved for backend self-wakeup.
//
// Real fds register their XrPollDesc* as the completion key, which is
// always a non-NULL heap pointer; 0 cleanly identifies the
// PostQueuedCompletionStatus(NULL) packets emitted by iocp_wakeup so
// the dispatch loop can drop them without consulting any side table.
#define XR_IOCP_KEY_WAKEUP ((ULONG_PTR) 0)

static int iocp_init(XrNetpoll *np) {
    // WSAStartup is reference-counted by the OS; pairing it with
    // WSACleanup on the cleanup path keeps the count balanced even
    // if multiple netpolls are spun up across the lifetime of the
    // process (tests, isolated runtimes, etc.).
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return -1;

    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (iocp == NULL) {
        WSACleanup();
        return -1;
    }

    XrAfdContext *ctx = (XrAfdContext *) xr_calloc(1, sizeof(XrAfdContext));
    if (!ctx) {
        CloseHandle(iocp);
        WSACleanup();
        return -1;
    }

    if (xr_afd_init(ctx, iocp) < 0) {
        DWORD err = GetLastError();
        xr_free(ctx);
        CloseHandle(iocp);
        WSACleanup();
        SetLastError(err);
        return -1;
    }

    np->backend_state = ctx;
    np->poll_fd = -1;  // unused on Windows; HANDLE/AFD live in backend_state
    return 0;
}

static void iocp_cleanup(XrNetpoll *np) {
    XrAfdContext *ctx = iocp_ctx(np);
    if (ctx == NULL) {
        WSACleanup();
        return;
    }

    // Order matters: tear down the AFD device before closing the IOCP.
    // AFD's outstanding completions still depend on the IOCP being
    // alive when CloseHandle(afd_device) drains them.
    HANDLE iocp = ctx->iocp;
    xr_afd_destroy(ctx);
    if (iocp != NULL)
        CloseHandle(iocp);

    xr_free(ctx);
    np->backend_state = NULL;
    WSACleanup();
}

static int iocp_add_fd(XrNetpoll *np, int fd, XrPollDesc *pd) {
    (void) np;
    (void) fd;
    (void) pd;
    // Real readiness wiring (base-socket resolution + initial AFD
    // poll submission) lands in the next change set. Returning -1
    // here makes xr_netpoll_open fail loudly so any caller that
    // exercises networking surfaces the missing wiring at first
    // use rather than silently hanging on a poll that never arms.
    return -1;
}

static void iocp_del_fd(XrNetpoll *np, int fd) {
    (void) np;
    (void) fd;
    // Companion to iocp_add_fd; nothing registered means nothing
    // to unwind until AFD wiring lands.
}

static int iocp_poll_events(XrNetpoll *np, int64_t delta_ns, XrReadyList *list) {
    XrAfdContext *ctx = iocp_ctx(np);
    if (ctx == NULL || ctx->iocp == NULL)
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
        GetQueuedCompletionStatusEx(ctx->iocp, entries, 128, &count, timeout_ms, FALSE);

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
        // forwarded as a bogus "both ready" guess.
        (void) entries[i];
        (void) list;
    }

    return (int) count;
}

static void iocp_wakeup(XrNetpoll *np) {
    XrAfdContext *ctx = iocp_ctx(np);
    if (ctx == NULL || ctx->iocp == NULL)
        return;

    // Coalesce concurrent wakeups: only one PostQueuedCompletionStatus
    // is in flight at a time. The flag is cleared in iocp_poll_events
    // when the wakeup packet is dequeued.
    bool expected = false;
    if (!atomic_compare_exchange_strong(&np->break_pending, &expected, true))
        return;

    PostQueuedCompletionStatus(ctx->iocp, 0, XR_IOCP_KEY_WAKEUP, NULL);
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
