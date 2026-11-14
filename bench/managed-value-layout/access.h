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
    XrMvlArray *owner;
    int64_t *data;
    size_t length;
    size_t capacity;
    bool writable;
} XrMvlArrayAccess;

typedef struct {
    XrMvlStatus status;
    XrMvlArrayAccess access;
    /* Identifies the caller's old reference, never an additional reference. */
    XrMvlArray *previous;
} XrMvlArrayWrite;

/* Starting or closing access does not allocate, clone, retain or check unique.
 * Read the current binding here only after RHS effects have completed. */
static inline XrMvlArrayAccess xr_mvl_array_access_start(XrMvlArray *current) {
    return (XrMvlArrayAccess) {current, NULL, 0, 0, false};
}

/* Inputs and outputs contain values, never a binding or automatic owner address.
 * On success publish access.owner immediately, then drop the caller's previous
 * reference if present. A fresh owner must be dropped if publication is aborted.
 * The caller supplies real ownership and a window excluding aliases and calls;
 * RC uniqueness alone does not establish that authority. */
XR_FUNC XrMvlArrayWrite xr_mvl_array_access_write(XrMvlArrayAccess access,
    size_t index, int64_t value, bool append);

static inline XrMvlArrayWrite xr_mvl_array_access_push(XrMvlArrayAccess access, int64_t value) {
    if (!access.owner || !access.writable)
        return (XrMvlArrayWrite) {XR_MVL_INVALID_OWNER, access, NULL};
    if (access.length == access.capacity)
        return xr_mvl_array_access_write(access, 0, value, true);
    access.data[access.length++] = value;
    access.owner->length = access.length;
    return (XrMvlArrayWrite) {XR_MVL_OK, access, NULL};
}

static inline XrMvlArrayWrite xr_mvl_array_access_set(XrMvlArrayAccess access,
                                                    size_t index, int64_t value) {
    if (!access.owner || !access.writable)
        return (XrMvlArrayWrite) {XR_MVL_INVALID_OWNER, access, NULL};
    if (index >= access.length)
        return (XrMvlArrayWrite) {XR_MVL_BOUNDS, access, NULL};
    access.data[index] = value;
    return (XrMvlArrayWrite) {XR_MVL_OK, access, NULL};
}

#endif // XR_MVL_ACCESS_H
