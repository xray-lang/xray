/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_directory_owned.h - Directory cursors with explicit persistent ownership
 *
 * KEY CONCEPT:
 *   Allocation policy outlives the cursor. Request control is synchronously
 *   borrowed, while every returned name has independent policy-owned storage.
 */
#ifndef XR_OS_DIRECTORY_OWNED_H
#define XR_OS_DIRECTORY_OWNED_H
#include "os_file_mutation.h"
typedef struct XrOsDirectoryOwner { void *state; } XrOsDirectoryOwner;
typedef struct XrOsDirectoryRequest {
    const XrOsIoPolicy *policy;
    XrOsPathSpan path;
    void *context;
    bool (*current)(void *context);
} XrOsDirectoryRequest;
typedef struct XrOsDirectoryText { char *bytes; size_t length; } XrOsDirectoryText;

/* Owner starts empty. OK publishes opened; false is a concrete path/OS
 * refusal. Typed failure preserves opened and can retain a cursor if its
 * cleanup close failed. The stable policy's context/callbacks must survive
 * the last close and the last returned-name free. Neither request nor
 * current/context is retained. */
XR_FUNC XrOsIoStatus xr_os_io_directory_open(const XrOsDirectoryRequest *request,
    XrOsDirectoryOwner *owner,bool *opened);
/* Output starts empty. OK publishes one complete strict UTF-8 name, excluding
 * dot entries; caller frees bytes with the original policy. It remains valid
 * after later reads or cursor close. END alone means actual end-of-stream;
 * END/failure preserves output. A typed read fault stops this cursor until
 * close, so failed publication cannot silently skip an entry on a retry. */
XR_FUNC XrOsIoStatus xr_os_io_directory_next(XrOsDirectoryOwner *owner,
    void *context,bool (*current)(void *context),XrOsDirectoryText *output);
/* One synchronous native close attempt. Success clears/frees owner; failure
 * retains the real handle and all allocation responsibility for a later
 * close-only retry. A closing cursor cannot enumerate again. Empty is OK.
 * Close uses no request control, allocation, path lookup or guest callback.
 * Windows uses ordinary directory handles, never FindFirst search handles.
 * Unix ownership/failed-close consumption is not implemented by this API. */
XR_FUNC XrOsIoStatus xr_os_io_directory_close(XrOsDirectoryOwner *owner);
#endif // XR_OS_DIRECTORY_OWNED_H
