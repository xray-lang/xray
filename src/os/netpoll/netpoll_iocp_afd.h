/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * netpoll_iocp_afd.h - \Device\Afd private API for IOCP readiness
 *
 * KEY CONCEPT:
 *   The Windows public socket API does not expose readiness
 *   notifications: there is no way to learn that a socket has become
 *   readable / writable without actually issuing a recv / send. The
 *   one route to readiness on Windows is the AFD kernel driver, the
 *   bottom layer of the Winsock stack, which speaks an undocumented
 *   IOCTL (IOCTL_AFD_POLL) that completes asynchronously through
 *   IOCP when the requested events fire.
 *
 *   This is a private NT API. The shape has been stable since
 *   Windows XP and is the foundation of libuv, mio (Rust), Node.js,
 *   Bun, Deno and Microsoft's own WSL2 socket bridge. The wrapper
 *   here is a minimal port of the relevant pieces of wepoll
 *   (https://github.com/piscisaureus/wepoll), keeping only what the
 *   xray netpoll abstraction needs.
 *
 * SCOPE:
 *   This header defines the AFD types, constants and the
 *   xr_afd_* wrapper API. It is included only by netpoll_iocp.c
 *   (which is in turn #included by xnetpoll.c) and is meaningless
 *   on non-Windows targets. Implementations live in the same
 *   translation unit so we don't have to teach CMake about a new
 *   platform-conditional source file.
 */

#ifndef NETPOLL_IOCP_AFD_H
#define NETPOLL_IOCP_AFD_H

#ifdef XR_OS_WINDOWS

// winsock2.h must precede windows.h to suppress the legacy
// <winsock.h> that windows.h would otherwise drag in (struct
// redefinition errors otherwise).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// winternl.h gives us IO_STATUS_BLOCK, OBJECT_ATTRIBUTES,
// UNICODE_STRING and PIO_APC_ROUTINE without forcing a link-time
// dependency on ntdll.lib (the actual entry points are resolved
// through GetProcAddress at init time).
#include <winternl.h>
#include <stdint.h>

// ============================================================================
// AFD constants (private; documented only via reverse engineering)
// ============================================================================

// IOCTL_AFD_POLL request code consumed by NtDeviceIoControlFile.
#define IOCTL_AFD_POLL 0x00012024

// Event mask bits accepted by AFD_POLL and reflected back in the
// completion. Mirrors wepoll / libuv constants verbatim.
#define AFD_POLL_RECEIVE 0x0001
#define AFD_POLL_RECEIVE_EXPEDITED 0x0002
#define AFD_POLL_SEND 0x0004
#define AFD_POLL_DISCONNECT 0x0008
#define AFD_POLL_ABORT 0x0010
#define AFD_POLL_LOCAL_CLOSE 0x0020
#define AFD_POLL_ACCEPT 0x0080
#define AFD_POLL_CONNECT_FAIL 0x0100

// All the events we ever care about on a watched socket. Always
// includes AFD_POLL_LOCAL_CLOSE so we get a notification when the
// user closes a SOCKET that's still registered with us.
#define XR_AFD_ALL_POLL_EVENTS                                                                     \
    (AFD_POLL_RECEIVE | AFD_POLL_RECEIVE_EXPEDITED | AFD_POLL_SEND | AFD_POLL_DISCONNECT |         \
     AFD_POLL_ABORT | AFD_POLL_LOCAL_CLOSE | AFD_POLL_ACCEPT | AFD_POLL_CONNECT_FAIL)

// ============================================================================
// NTSTATUS values we examine. winternl.h does not expose these.
// ============================================================================

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((LONG) 0x00000000L)
#endif
#ifndef STATUS_PENDING
#define STATUS_PENDING ((LONG) 0x00000103L)
#endif
#ifndef STATUS_CANCELLED
#define STATUS_CANCELLED ((LONG) 0xC0000120L)
#endif
#ifndef STATUS_NOT_FOUND
#define STATUS_NOT_FOUND ((LONG) 0xC0000225L)
#endif
#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((LONG) (status)) >= 0)
#endif

// ============================================================================
// AFD_POLL_INFO: input/output structure for IOCTL_AFD_POLL
// ============================================================================

// Single-handle variant. The Windows kernel can poll many sockets in
// one IOCTL via the variable-length Handles[] array, but xray uses
// one outstanding poll request per socket so the single-handle layout
// is enough and lives entirely on the per-pd allocation.
typedef struct _XR_AFD_POLL_HANDLE_INFO {
    HANDLE Handle;
    ULONG Events;
    LONG Status;  // NTSTATUS
} XR_AFD_POLL_HANDLE_INFO;

typedef struct _XR_AFD_POLL_INFO {
    LARGE_INTEGER Timeout;  // INT64_MAX = "wait forever"
    ULONG NumberOfHandles;  // Always 1 in this wrapper
    ULONG Exclusive;        // FALSE — multiple poll requests may coexist on a fd
    XR_AFD_POLL_HANDLE_INFO Handles[1];
} XR_AFD_POLL_INFO;

// ============================================================================
// AFD context: per-netpoll AFD device handle and IOCP backend state.
// ============================================================================

// The AFD device handle is a single per-IOCP resource: every poll
// request submitted through this handle completes via the IOCP it
// was associated with at creation time. xray stores one
// XrAfdContext per XrNetpoll on the heap (pointed to from
// XrNetpoll.backend_state).
typedef struct XrAfdContext {
    HANDLE iocp;                   // Borrowed; XrNetpoll owns the lifetime
    HANDLE afd_device;             // Owned; opened via NtCreateFile on \Device\Afd\XrayN
    ULONG_PTR completion_key;      // Stored at CreateIoCompletionPort time; lets dispatch distinguish AFD completions from self-wakeup
    void *update_queue_head;       // XrPollDesc*; intrusive single-linked stack of pds awaiting AFD poll re-submit
} XrAfdContext;

// ============================================================================
// Public API (implemented in netpoll_iocp.c)
// ============================================================================

// Initialize the AFD context: open \Device\Afd, associate it with
// the given IOCP under the supplied completion_key, and resolve the
// NT API entry points needed for later xr_afd_poll / xr_afd_cancel
// calls. The completion_key is stamped onto every AFD-driven IOCP
// completion; pick a non-zero value so wakeup packets posted with
// key=0 stay distinguishable.
//
// On the first call this also performs one-time global setup
// (GetProcAddress for NtCreateFile / NtDeviceIoControlFile /
// NtCancelIoFileEx / RtlNtStatusToDosError / RtlInitUnicodeString);
// thread-safe via a one-shot init guard.
//
// Returns 0 on success, -1 on failure with the caller-side state
// untouched. GetLastError() holds a translated Win32 error.
int xr_afd_init(XrAfdContext *ctx, HANDLE iocp, ULONG_PTR completion_key);

// Close the AFD device handle. Safe to call on a never-initialized
// or partially-initialized context (any field that is NULL / 0 is
// skipped). Does not touch the IOCP — the caller still owns it.
void xr_afd_destroy(XrAfdContext *ctx);

// Submit an asynchronous AFD_POLL request. The completion is
// delivered through the IOCP the AFD device was associated with;
// the IO_STATUS_BLOCK acts as the OVERLAPPED equivalent and must
// stay valid until the completion is dequeued.
//
// Returns:
//   0   request submitted (will complete via IOCP)
//  -1   submission failed; GetLastError() set
int xr_afd_poll(XrAfdContext *ctx, XR_AFD_POLL_INFO *info, IO_STATUS_BLOCK *iosb);

// Cancel an outstanding AFD_POLL request. The completion still
// flows through the IOCP (with iosb->Status == STATUS_CANCELLED),
// so the caller MUST wait for it before recycling the iosb /
// poll_info.
//
// Returns 0 on success or "already complete" (no-op), -1 on
// hard failure.
int xr_afd_cancel(XrAfdContext *ctx, IO_STATUS_BLOCK *iosb);

// Resolve the base socket of a (possibly layered) socket. AFD
// only operates on the base provider's SOCKET handle; layered
// service providers (LSPs) wrap it and report their own. Tries
// SIO_BSP_HANDLE_POLL (modern, LSP-aware) first and falls back
// to SIO_BASE_HANDLE.
//
// Returns 0 on success and writes the base SOCKET to *out;
// -1 on failure with GetLastError() set. xray treats failure
// as "this fd cannot be polled" and propagates the error rather
// than introducing a select-loop fallback.
int xr_afd_get_base_socket(SOCKET sock, SOCKET *out);

#endif  // XR_OS_WINDOWS

#endif  // NETPOLL_IOCP_AFD_H
