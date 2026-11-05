/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_file_owned.h - Owned files and borrowed CRT streams with cancellable workers
 */
#ifndef XR_OS_FILE_OWNED_H
#define XR_OS_FILE_OWNED_H
#include "../base/xio_policy.h"
#include <stdbool.h>

typedef struct XrOsFileOwner { void *state; bool busy; } XrOsFileOwner;
typedef struct XrOsFileOperation { void *state; bool busy; } XrOsFileOperation;
typedef struct XrOsFileQuery { void *context; bool (*current)(void *); } XrOsFileQuery;
typedef enum XrOsFileAction {
    XR_OS_FILE_OPEN_READ, XR_OS_FILE_OPEN_WRITE,
    XR_OS_FILE_READ, XR_OS_FILE_WRITE, XR_OS_FILE_FLUSH
} XrOsFileAction;
typedef struct XrOsFileRequest {
    XrOsFileAction action;
    const char *path;
    size_t path_length;
    bool append;
    int standard_stream;
    const uint8_t *bytes;
    size_t length;
} XrOsFileRequest;
/* Count always retains actual transferred bytes, including a typed failure.
 * OK/success accepts a nonempty partial transfer even if CRT ferror is set;
 * zero-byte ordinary failure is OK/false, while EOF is OK/true with count zero.
 * Typed cancellation/resource failures take precedence and never request replay. */
typedef struct XrOsFileProgress {
    bool terminal, success;
    XrOsIoStatus status;
    size_t count;
    uint32_t native_error;
    const uint8_t *bytes;
} XrOsFileProgress;

/* Owners and operations are zero-initialized, address-stable and externally
 * serialized. Copies acquire no ownership. Memory policy/context outlive every
 * owner and operation. Queries are borrowed synchronously by begin only; no
 * worker invokes an allocator callback or retains a query, guest value or frame.
 * Failed begin can retain cleanup duties. Outputs change only on OK.
 * Unix entry points return UNSUPPORTED without altering owners or outputs. */
XR_FUNC XrOsIoStatus xr_os_io_file_open_begin(const XrOsIoPolicy *memory,
    const XrOsFileQuery *query,const XrOsFileRequest *request,
    XrOsFileOwner *owner,XrOsFileOperation *operation);
/* An owned operation requires owner and standard_stream==-1. A borrowed stream
 * requires owner==NULL and stream 0/1/2, and uses the caller's actual CRT FILE.
 * A pending opener cannot be used by file_begin; poll must first observe its
 * real terminal result. operation_close also acquires an unpublished open
 * handle before releasing that operation, so late cancellation retains ownership.
 * Path and write spans are borrowed only during begin. Write input is
 * snapshotted before thread creation. Read bytes remain owned by the operation
 * until operation_close succeeds. A zero-byte operation does not read or write
 * the OS. */
XR_FUNC XrOsIoStatus xr_os_io_file_begin(const XrOsIoPolicy *memory,
    const XrOsFileQuery *query,const XrOsFileRequest *request,
    XrOsFileOwner *owner,XrOsFileOperation *operation);
XR_FUNC XrOsIoStatus xr_os_io_file_poll(XrOsFileOperation *operation,XrOsFileProgress *output);
XR_FUNC XrOsIoStatus xr_os_io_file_cancel(XrOsFileOperation *operation);
/* Pending close only requests cancellation and returns OK/false. Release
 * happens after actual thread termination; every failed close retains its
 * exact handle stage for another close-only attempt. Cancellation does not
 * promise rollback of bytes already consumed or written. */
XR_FUNC XrOsIoStatus xr_os_io_file_operation_close(XrOsFileOperation *operation,bool *closed);
/* Active operations return OK/false without closing the file. A successful
 * native close clears ownership; failure preserves it. Borrowed streams are
 * never closed here. Owned flush has no durable-storage guarantee. */
XR_FUNC XrOsIoStatus xr_os_io_file_close(XrOsFileOwner *owner,bool *closed);
#endif // XR_OS_FILE_OWNED_H
