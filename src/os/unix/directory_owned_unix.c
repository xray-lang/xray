/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * directory_owned_unix.c - Explicit boundary for unproved close consumption
 *
 * KEY CONCEPT:
 *   A failed closedir cannot be presumed to retain a reusable DIR pointer.
 *   This interface acquires nothing until that platform protocol is defined.
 */
#include "../os_directory_owned.h"
#include "../../base/xio_policy.inc.h"
XR_FUNC XrOsIoStatus xr_os_io_directory_open(const XrOsDirectoryRequest *request,
    XrOsDirectoryOwner *owner,bool *opened) {
    if(!request || !owner || owner->state || !opened || !request->path.text || !io_policy_valid(request->policy))return XR_OS_IO_BAD_ARGUMENT;
    return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_directory_next(XrOsDirectoryOwner *owner,
    void *context,bool (*current)(void *),XrOsDirectoryText *output) {
    (void)context;(void)current;
    if(!owner || owner->state || !output || output->bytes || output->length)return XR_OS_IO_BAD_ARGUMENT;
    return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_directory_close(XrOsDirectoryOwner *owner) {
    return !owner || owner->state?XR_OS_IO_BAD_ARGUMENT:XR_OS_IO_OK;
}
