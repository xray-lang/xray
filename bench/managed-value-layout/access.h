/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * access.h - Nonowning access to the same canonical Array payload
 *
 * KEY CONCEPT:
 *   A caller-proven exclusive window permits direct access without another pin.
 *   Calls, alias observation, retain and rebinding close that window.
 */
#ifndef XR_MVL_ACCESS_H
#define XR_MVL_ACCESS_H

#include "layout.h"

typedef struct {
    /* Borrowed copy for a window that excludes all observers of the binding. */
    XrMvlArray value;
    int64_t *data;
    size_t capacity;
    bool writable;
} XrMvlArrayAccess;

typedef struct {
    XrMvlStatus status;
    XrMvlArrayAccess access;
    /* Identifies the caller's old reference, never an additional reference. */
    XrMvlArrayBacking *previous;
} XrMvlArrayWrite;

/* Starting or closing access does not allocate, clone, retain or check unique.
 * Read the current binding here only after RHS effects have completed. */
static inline XrMvlArrayAccess xr_mvl_array_access_start(XrMvlArray current) {
    return (XrMvlArrayAccess) {current, NULL, 0, false};
}

/* Inputs and outputs contain values, never a binding or automatic owner address.
 * On success publish access.value and replace cached access immediately. A
 * unique resize can consume the original owner and invalidate all old pointers.
 * Only COW supplies a previous reference to drop after publication. Its fresh
 * owner may instead be dropped to abandon that replacement.
 * The caller supplies real ownership and a window excluding aliases and calls;
 * RC uniqueness alone does not establish that authority. */
XR_FUNC XrMvlArrayWrite xr_mvl_array_access_write(XrMvlArrayAccess access,
    size_t index, int64_t value, bool append);

static inline XrMvlArrayWrite xr_mvl_array_access_push(XrMvlArrayAccess access, int64_t value) {
    if (!access.value.owner || !access.writable)
        return (XrMvlArrayWrite) {XR_MVL_INVALID_OWNER, access, NULL};
    if (access.value.length == access.capacity)
        return xr_mvl_array_access_write(access, 0, value, true);
    access.data[access.value.length] = value;
    ++access.value.length;
    return (XrMvlArrayWrite) {XR_MVL_OK, access, NULL};
}

static inline XrMvlArrayWrite xr_mvl_array_access_set(XrMvlArrayAccess access,
                                                    size_t index, int64_t value) {
    if (!access.value.owner || !access.writable)
        return (XrMvlArrayWrite) {XR_MVL_INVALID_OWNER, access, NULL};
    if (index >= access.value.length)
        return (XrMvlArrayWrite) {XR_MVL_BOUNDS, access, NULL};
    access.data[index] = value;
    return (XrMvlArrayWrite) {XR_MVL_OK, access, NULL};
}

#endif // XR_MVL_ACCESS_H
