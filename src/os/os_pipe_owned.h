/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_pipe_owned.h - Owned synchronous pipes with isolated single-operation workers
 */
#ifndef XR_OS_PIPE_OWNED_H
#define XR_OS_PIPE_OWNED_H
#include "../base/xio_policy.h"
#include <stdbool.h>

typedef struct XrOsPipeOwner { void *state; bool busy; } XrOsPipeOwner;
typedef struct XrOsPipeOperation { void *state; bool busy; } XrOsPipeOperation;
typedef enum XrOsPipeEnd { XR_OS_PIPE_READ, XR_OS_PIPE_WRITE } XrOsPipeEnd;
typedef struct XrOsPipeQuery { void *context; bool (*current)(void *); } XrOsPipeQuery;
typedef struct XrOsPipeRequest {
    XrOsPipeEnd end;
    const uint8_t *bytes;
    size_t length;
} XrOsPipeRequest;
typedef struct XrOsPipeProgress {
    bool terminal;
    XrOsIoStatus status;
    size_t count;
    uint32_t native_error;
    const uint8_t *bytes;
} XrOsPipeProgress;

/* Zero-initialize owners and keep their addresses stable through dispose.
 * Calls on a pair and its operations are externally serialized. Copies do not
 * acquire ownership. The copied memory policy/context outlive the pair and all
 * operations. A supplied query/current is borrowed only inside open/begin.
 * Misaligned policy allocations are freed and rejected before typed access.
 * No worker calls the allocator, query or any guest code.
 *
 * Failed creation or begin may retain an owner with cleanup duties; always
 * dispose it. Ordinary outputs change only on OK. Unsupported Unix entry
 * points preserve every output and perform no operation. */
XR_FUNC XrOsIoStatus xr_os_io_pipe_open(const XrOsIoPolicy *memory,
    const XrOsPipeQuery *query,XrOsPipeOwner *owner,bool *opened);
/* Read requires bytes==NULL. Write copies the entire selected chunk before
 * starting its one syscall; no input span is retained. Windows selects at most
 * DWORD_MAX bytes, the actual syscall representation limit, not a private quota.
 * Zero-length operations complete without creating a thread. */
XR_FUNC XrOsIoStatus xr_os_io_pipe_begin(XrOsPipeOwner *owner,
    const XrOsPipeRequest *request,const XrOsPipeQuery *query,XrOsPipeOperation *operation);
/* Each poll is nonblocking. CANCELLED is a real pre-syscall cancellation or an
 * acknowledged ERROR_OPERATION_ABORTED; an OK cancel request alone is neither.
 * END means a closed write peer. Read EOF is OK with count zero. Read bytes
 * remain owned by the operation until operation_close succeeds. */
XR_FUNC XrOsIoStatus xr_os_io_pipe_poll(XrOsPipeOperation *operation,XrOsPipeProgress *output);
XR_FUNC XrOsIoStatus xr_os_io_pipe_cancel(XrOsPipeOperation *operation);
/* Pending close requests cancellation and returns OK/false, preserving all
 * resources. Only a truly terminated worker can be joined and its duplicate,
 * snapshot and thread owner freed. Close failures preserve the exact stage. */
XR_FUNC XrOsIoStatus xr_os_io_pipe_operation_close(XrOsPipeOperation *operation,bool *closed);
/* An end with a live operation cannot close; OK/false preserves that end.
 * Closing the other end may release a blocked read or write. */
XR_FUNC XrOsIoStatus xr_os_io_pipe_close_end(XrOsPipeOwner *owner,XrOsPipeEnd end,bool *closed);
/* Requires every operation to have closed. Failed per-end native closes retain
 * only the remaining duties; success clears the pair and is idempotent. */
XR_FUNC XrOsIoStatus xr_os_io_pipe_dispose(XrOsPipeOwner *owner,bool *closed);
/* C-only synchronous borrow for immediate child redirection/duplication.
 * The borrower must not close or retain this handle. No script token is a raw
 * HANDLE, and this function establishes no Root or Source authority. */
XR_FUNC XrOsIoStatus xr_os_io_pipe_borrow(XrOsPipeOwner *owner,XrOsPipeEnd end,intptr_t *output);
#endif // XR_OS_PIPE_OWNED_H
