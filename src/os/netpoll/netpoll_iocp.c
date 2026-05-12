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
        g_nt_api.name = (PFN_##name)(uintptr_t) GetProcAddress(ntdll, #name);                      \
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

int xr_afd_init(XrAfdContext *ctx, HANDLE iocp, ULONG_PTR completion_key) {
    XR_DCHECK(ctx != NULL, "xr_afd_init: NULL ctx");
    XR_DCHECK(iocp != NULL, "xr_afd_init: NULL iocp");

    if (load_nt_api() < 0)
        return -1;

    ctx->iocp = iocp;
    ctx->afd_device = NULL;
    ctx->completion_key = completion_key;
    ctx->update_queue_head = NULL;

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
    NTSTATUS status =
        g_nt_api.NtCreateFile(&afd, SYNCHRONIZE, &attrs, &iosb, NULL, 0,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0, NULL, 0);
    if (!NT_SUCCESS(status))
        return set_error_from_nt(status);

    // Associate the AFD device with our IOCP. Every poll completion
    // submitted via this device handle will be delivered through the
    // same IOCP carrying self-wakeup packets, so the dispatch loop
    // distinguishes them by completion_key (AFD = supplied sentinel,
    // wakeup = 0 via PostQueuedCompletionStatus).
    if (CreateIoCompletionPort(afd, iocp, completion_key, 0) == NULL) {
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

    NTSTATUS status =
        g_nt_api.NtDeviceIoControlFile(ctx->afd_device, NULL, NULL, iosb, iosb, IOCTL_AFD_POLL,
                                       info, sizeof(*info), info, sizeof(*info));

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

// Completion-key sentinels.
//
//   WAKEUP — PostQueuedCompletionStatus(NULL) packets emitted by
//            iocp_wakeup. Hard-coded to 0 so the kernel's default
//            "no key" packets cannot collide with a live entry.
//   AFD    — Stamped onto every AFD poll completion via the
//            CreateIoCompletionPort association in xr_afd_init.
//            Any non-zero, page-aligned-stable value works; 1 is
//            small, never collides with a heap pointer (heap blocks
//            are aligned to MEMORY_ALLOCATION_ALIGNMENT >= 8), and
//            keeps the dispatch switch trivial.
#define XR_IOCP_KEY_WAKEUP ((ULONG_PTR) 0)
#define XR_IOCP_KEY_AFD ((ULONG_PTR) 1)

// ----------------------------------------------------------------------------
// Per-pd IOCP state.
//
// Lives in the opaque iocp_state[XR_IOCP_PD_STATE_SIZE] buffer
// reserved on every XrPollDesc. We pin its layout via _Static_assert
// so growth is caught at build time rather than at runtime as a
// silent buffer overrun.
//
// iosb MUST be the first field: AFD completions deliver lpOverlapped
// pointing at &state->iosb, and we recover the owning XrPollDesc by
// subtracting offsetof(XrPollDesc, iocp_state) from that pointer.
// ----------------------------------------------------------------------------

enum {
    XR_IOCP_POLL_IDLE = 0,       // No outstanding AFD poll
    XR_IOCP_POLL_PENDING = 1,    // AFD poll submitted; awaiting completion
    XR_IOCP_POLL_CANCELLED = 2,  // NtCancelIoFileEx called; awaiting drain completion
};

typedef struct XrIocpPdState {
    IO_STATUS_BLOCK iosb;            // MUST be first; lpOverlapped recovery
    XR_AFD_POLL_INFO poll_info;      // Submitted to NtDeviceIoControlFile
    SOCKET base_socket;              // Resolved once on add_fd
    uint32_t user_events;            // AFD event mask we want monitored
    uint32_t pending_events;         // Snapshot at submit time (debug aid)
    uint8_t poll_status;             // XR_IOCP_POLL_*
    uint8_t in_update_queue;         // 1 if linked into ctx->update_queue_head
    uint8_t pad[6];                  // Explicit padding so update_link is 8-byte aligned
    struct XrPollDesc *update_link;  // Intrusive single-linked stack
} XrIocpPdState;

_Static_assert(sizeof(XrIocpPdState) <= XR_IOCP_PD_STATE_SIZE,
               "XrIocpPdState must fit in XR_IOCP_PD_STATE_SIZE");
_Static_assert(offsetof(XrIocpPdState, iosb) == 0,
               "iosb must be at offset 0 for lpOverlapped pd recovery");

static inline XrIocpPdState *iocp_pd(XrPollDesc *pd) {
    return (XrIocpPdState *) pd->iocp_state;
}

// Recover the owning XrPollDesc from an IO_STATUS_BLOCK pointer
// returned via OVERLAPPED_ENTRY.lpOverlapped. iosb is the first
// field of XrIocpPdState which lives at offset
// offsetof(XrPollDesc, iocp_state) inside the pd, so a single
// subtraction reverses the pointer arithmetic.
static inline XrPollDesc *pd_from_iosb(IO_STATUS_BLOCK *iosb) {
    return (XrPollDesc *) ((unsigned char *) iosb - offsetof(XrPollDesc, iocp_state));
}

// Translate an AFD event mask into the readiness mode bits the
// platform-independent layer understands. Mirrors wepoll's mapping
// but collapses everything into XR_POLL_READ / XR_POLL_WRITE since
// xnetpoll has no priority-band or hangup channels to speak of.
//
// AFD_POLL_LOCAL_CLOSE is handled by the caller before this point
// (it short-circuits to "both ready" so the caller's next syscall
// surfaces the EOF / error itself).
static int iocp_afd_events_to_mode(ULONG afd_events) {
    int mode = 0;
    if (afd_events & (AFD_POLL_RECEIVE | AFD_POLL_RECEIVE_EXPEDITED | AFD_POLL_DISCONNECT |
                      AFD_POLL_ACCEPT | AFD_POLL_ABORT))
        mode |= XR_POLL_READ;
    if (afd_events & (AFD_POLL_SEND | AFD_POLL_CONNECT_FAIL))
        mode |= XR_POLL_WRITE;
    // CONNECT_FAIL also surfaces as "readable" so an in-flight
    // connect() can wake on either side and observe the error
    // through getsockopt(SO_ERROR). Mirrors what libuv does.
    if (afd_events & AFD_POLL_CONNECT_FAIL)
        mode |= XR_POLL_READ;
    return mode;
}

// ----------------------------------------------------------------------------
// Update queue: pds whose AFD poll just completed and which are
// still interested in events get pushed here, then drained back into
// the AFD driver as a fresh poll request the next time iocp_poll_events
// runs (or when the caller passes through it). The queue is single-
// threaded — only the worker calling iocp_poll_events touches it —
// so a plain pointer + intrusive link is enough.
// ----------------------------------------------------------------------------

static void iocp_update_queue_push(XrAfdContext *ctx, XrPollDesc *pd) {
    XrIocpPdState *st = iocp_pd(pd);
    if (st->in_update_queue)
        return;
    st->in_update_queue = 1;
    st->update_link = (XrPollDesc *) ctx->update_queue_head;
    ctx->update_queue_head = pd;
}

static int iocp_submit_poll(XrAfdContext *ctx, XrPollDesc *pd) {
    XrIocpPdState *st = iocp_pd(pd);
    if (st->poll_status != XR_IOCP_POLL_IDLE)
        return 0;  // Already pending or cancelled; do nothing

    // Prime the AFD_POLL_INFO. NumberOfHandles=1 is the only shape
    // we ever use; Exclusive=FALSE allows multiple poll requests on
    // the same fd to coexist (we don't issue them, but FALSE is the
    // documented default).
    st->poll_info.Timeout.QuadPart = INT64_MAX;
    st->poll_info.NumberOfHandles = 1;
    st->poll_info.Exclusive = FALSE;
    st->poll_info.Handles[0].Handle = (HANDLE) st->base_socket;
    st->poll_info.Handles[0].Status = 0;
    st->poll_info.Handles[0].Events = st->user_events;

    if (xr_afd_poll(ctx, &st->poll_info, &st->iosb) < 0) {
        // Submission failed; leave the state IDLE so the next caller
        // can decide whether to retry or surface the error. We do
        // NOT mark it as PENDING because no completion will arrive.
        return -1;
    }

    st->poll_status = XR_IOCP_POLL_PENDING;
    st->pending_events = st->user_events;
    return 0;
}

static void iocp_drain_update_queue(XrAfdContext *ctx) {
    XrPollDesc *head = (XrPollDesc *) ctx->update_queue_head;
    ctx->update_queue_head = NULL;

    while (head != NULL) {
        XrIocpPdState *st = iocp_pd(head);
        XrPollDesc *next = st->update_link;
        st->update_link = NULL;
        st->in_update_queue = 0;

        // Skip pds that have been closed in the meantime; their
        // outstanding poll (if any) has already been cancelled and
        // the deferred-free path will reclaim the memory.
        if (!atomic_load(&head->closing) && st->user_events != 0) {
            (void) iocp_submit_poll(ctx, head);
        }

        head = next;
    }
}

// ----------------------------------------------------------------------------
// Completion handling: one AFD completion turned into readiness for
// the platform-independent layer. The caller has already verified
// the completion came from the AFD device (key == XR_IOCP_KEY_AFD)
// and recovered the owning pd from the IO_STATUS_BLOCK pointer.
// ----------------------------------------------------------------------------

static void iocp_handle_completion(XrPollDesc *pd, XrReadyList *list) {
    XrIocpPdState *st = iocp_pd(pd);

    bool was_cancelled = (st->poll_status == XR_IOCP_POLL_CANCELLED);
    st->poll_status = XR_IOCP_POLL_IDLE;
    st->pending_events = 0;

    // STATUS_CANCELLED arrives in two scenarios:
    //   1. iocp_del_fd / xr_netpoll_close called NtCancelIoFileEx
    //      and we are now draining the canceled completion.
    //   2. A re-arm was scheduled with an updated event mask while
    //      a previous poll was still pending — the previous one was
    //      cancelled to make way. (xray's current event-mask is
    //      static so this branch is theoretical until we add
    //      dynamic mask updates, but the handling cost is zero.)
    //
    // In both cases we just consume and do not surface readiness.
    LONG nt_status = (LONG) st->iosb.Status;
    if (was_cancelled || nt_status == STATUS_CANCELLED)
        return;

    // pd is closing: the close path already cleared fdmap and queued
    // the deferred free; we just drop the completion on the floor so
    // we don't write a stale ready event into a freed pd.
    if (atomic_load(&pd->closing))
        return;

    int mode = 0;
    if (NT_SUCCESS(nt_status) && st->poll_info.NumberOfHandles >= 1) {
        ULONG events = st->poll_info.Handles[0].Events;
        if (events & AFD_POLL_LOCAL_CLOSE) {
            // Socket was closed under us (closesocket() on the
            // user-visible handle). Surface as both R+W ready so
            // the caller's next syscall sees -1/EBADF and tears
            // down the connection.
            mode = XR_POLL_BOTH;
        } else {
            mode = iocp_afd_events_to_mode(events);
        }
    } else if (!NT_SUCCESS(nt_status)) {
        // The poll itself errored (e.g. ERROR_INVALID_HANDLE because
        // the base socket was closed without going through xray).
        // Wake both modes so the next syscall surfaces the failure.
        mode = XR_POLL_BOTH;
    }

    if (mode != 0)
        xr_netpoll_ready(list, pd, mode);
}

// ----------------------------------------------------------------------------
// Backend ops
// ----------------------------------------------------------------------------

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

    if (xr_afd_init(ctx, iocp, XR_IOCP_KEY_AFD) < 0) {
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

    // The update queue is a transient list of pds awaiting re-arm;
    // by the time we reach cleanup, xnetpoll has already torn the
    // pollDesc cache down so the linked pds are dead. Just drop the
    // head pointer; nothing else owns these nodes.
    ctx->update_queue_head = NULL;

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
    XR_DCHECK(pd != NULL, "iocp_add_fd: NULL pd");
    XrAfdContext *ctx = iocp_ctx(np);
    if (ctx == NULL || ctx->afd_device == NULL)
        return -1;
    if (fd < 0)
        return -1;

    // Resolve the base socket once. Layered Service Providers can
    // wrap the user-visible SOCKET; AFD only operates on the
    // bottom-of-stack handle. SIO_BSP_HANDLE_POLL is the modern,
    // LSP-aware ioctl with SIO_BASE_HANDLE as fallback.
    SOCKET base = INVALID_SOCKET;
    if (xr_afd_get_base_socket((SOCKET) fd, &base) < 0)
        return -1;

    XrIocpPdState *st = iocp_pd(pd);
    memset(st, 0, sizeof(*st));
    st->base_socket = base;
    st->poll_status = XR_IOCP_POLL_IDLE;

    // We watch every event AFD knows about. The platform-independent
    // layer demultiplexes into XR_POLL_READ / XR_POLL_WRITE based on
    // which side of the pd's CAS state machine has a waiter; over-
    // reporting is harmless because the unblock path is idempotent.
    // AFD_POLL_LOCAL_CLOSE is always included so the backend learns
    // about closesocket() races even if no user-side I/O is pending.
    st->user_events = XR_AFD_ALL_POLL_EVENTS;

    if (iocp_submit_poll(ctx, pd) < 0)
        return -1;

    return 0;
}

static void iocp_del_fd(XrNetpoll *np, int fd, XrPollDesc *pd) {
    (void) fd;
    XR_DCHECK(pd != NULL, "iocp_del_fd: NULL pd");
    XrAfdContext *ctx = iocp_ctx(np);
    if (ctx == NULL)
        return;

    XrIocpPdState *st = iocp_pd(pd);

    // If the pd is queued for re-submission, it is no longer eligible —
    // pull it off lazily by clearing the flag; the drain loop already
    // skips closing pds. We cannot easily unlink from the middle of
    // the intrusive single-linked stack, so the entry stays in the
    // chain but iocp_drain_update_queue ignores it.
    st->in_update_queue = 0;

    // No outstanding poll: nothing to cancel. The caller will free
    // the pd memory immediately, which is safe because no future
    // AFD completion will reference it.
    if (st->poll_status != XR_IOCP_POLL_PENDING)
        return;

    // Hand pd ownership to the completion path BEFORE issuing the
    // cancel. From this point on a completion (cancellation or
    // legitimate event) will arrive at iocp_handle_completion with
    // lpOverlapped pointing at &st->iosb; xr_netpoll_close skips
    // the cache free when this flag is set so the kernel never
    // dereferences a recycled pd. Even if xr_afd_cancel fails the
    // pending completion is still in flight, so we set the flag
    // unconditionally here.
    atomic_store(&pd->iocp_holds_ref, true);

    if (xr_afd_cancel(ctx, &st->iosb) < 0) {
        // Cancel failed for an unexpected reason. The pending poll
        // will eventually fire on its own (e.g., AFD_POLL_LOCAL_CLOSE
        // when the user closes the socket); the closing flag still
        // suppresses the readiness translation, and iocp_holds_ref
        // still funnels the eventual completion through the reap
        // path that frees pd.
        return;
    }

    st->poll_status = XR_IOCP_POLL_CANCELLED;
}

static int iocp_poll_events(XrNetpoll *np, int64_t delta_ns, XrReadyList *list) {
    XrAfdContext *ctx = iocp_ctx(np);
    if (ctx == NULL || ctx->iocp == NULL)
        return -1;

    // Re-arm any pds whose previous poll completed but still have
    // outstanding interest. Doing this before the dequeue ensures a
    // fresh AFD poll is in flight by the time we wait, so a freshly
    // ready socket isn't stuck for an extra round trip.
    iocp_drain_update_queue(ctx);

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
    BOOL ok = GetQueuedCompletionStatusEx(ctx->iocp, entries,
                                          (ULONG) (sizeof(entries) / sizeof(entries[0])), &count,
                                          timeout_ms, FALSE);

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

        if (key != XR_IOCP_KEY_AFD) {
            // Foreign completion (the IOCP is dedicated to netpoll
            // today, but be defensive in case future code shares it).
            continue;
        }

        IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *) entries[i].lpOverlapped;
        if (iosb == NULL)
            continue;

        XrPollDesc *pd = pd_from_iosb(iosb);
        iocp_handle_completion(pd, list);

        bool is_closing = atomic_load(&pd->closing);
        XrIocpPdState *st = iocp_pd(pd);

        // Re-enqueue for re-submission unless closing or unmonitored.
        // The push is a no-op if pd is already in the queue, so a
        // burst of completions on the same pd doesn't duplicate.
        if (!is_closing && st->user_events != 0 && st->poll_status == XR_IOCP_POLL_IDLE) {
            iocp_update_queue_push(ctx, pd);
        }

        // Reap pds whose async ref the backend held during close.
        // xr_netpoll_close deliberately skipped the cache free when
        // iocp_holds_ref was set; this completion is the contract's
        // signal that the kernel no longer references the iosb, so
        // we own the pd outright and can return it to the cache.
        // No further iocp_state access on pd after this point.
        if (is_closing && atomic_load(&pd->iocp_holds_ref)) {
            atomic_store(&pd->iocp_holds_ref, false);
            XrNetpoll *owning_np = pd->netpoll;
            if (owning_np != NULL) {
                xr_poll_cache_free(&owning_np->cache, pd);
            }
        }
    }

    // Drain again so the freshly enqueued pds get re-armed before
    // the caller releases control. This is the wepoll-style
    // submit-after-process pattern: completions come in, we generate
    // readiness, then immediately re-arm so the next iteration of
    // the runtime loop sees the next batch promptly.
    iocp_drain_update_queue(ctx);

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
