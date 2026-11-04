/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_dylib_owned.h - Owned loader handles and current-thread error snapshots
 */
#ifndef XR_OS_DYLIB_OWNED_H
#define XR_OS_DYLIB_OWNED_H
#include "../base/xio_policy.h"
#include <stdbool.h>

typedef struct XrOsDylibOwner { void *state; bool busy; } XrOsDylibOwner;
typedef struct XrOsDylibText { const char *text; size_t length; } XrOsDylibText;
typedef struct XrOsDylibQuery { void *context; bool (*current)(void *); } XrOsDylibQuery;
typedef struct XrOsDylibErrorText {
    char *text;
    size_t length;
    XrOsIoPolicy memory;
} XrOsDylibErrorText;

/* Zero-initialize a stable owner; copying it grants no ownership. The policy
 * context outlives close. Query and exact UTF-8/NUL-free spans are borrowed
 * only during each call. Open may retain cleanup duties even on failure.
 * Ordinary platform refusal is OK/false; resource, cancellation and protocol
 * failures remain typed. Failure preserves ordinary outputs. No native handle
 * or returned address establishes Root, Source, dereference or FFI authority. */
XR_FUNC XrOsIoStatus xr_os_io_dylib_open(const XrOsIoPolicy *memory,
    const XrOsDylibQuery *query,XrOsDylibText path,XrOsDylibOwner *owner,bool *opened);
XR_FUNC XrOsIoStatus xr_os_io_dylib_symbol(XrOsDylibOwner *owner,
    const XrOsDylibQuery *query,XrOsDylibText name,uintptr_t *address,bool *found);
/* The caller invalidates every associated pointer origin before entering.
 * Failure retains the native handle and exact close-only responsibility;
 * success clears the owner. Retirement needs no query or new allocation. */
XR_FUNC XrOsIoStatus xr_os_io_dylib_close(XrOsDylibOwner *owner,bool *closed);
/* Snapshot the current OS thread's loader error before allocator/query/format
 * callbacks. The thread retains only a native numeric error, never a Domain,
 * request, allocator or borrowed string. Success returns independently owned
 * complete UTF-8, including an owned empty string for no error. Formatting
 * failures do not replace the original thread error. Output must be empty. */
XR_FUNC XrOsIoStatus xr_os_io_dylib_last_error(const XrOsIoPolicy *memory,
    const XrOsDylibQuery *query,XrOsDylibErrorText *output);
XR_FUNC void xr_os_io_dylib_error_free(XrOsDylibErrorText *text);
#endif // XR_OS_DYLIB_OWNED_H
