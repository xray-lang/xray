/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * file_metadata_path.inc.h - Owned NUL termination for exact UTF-8 path spans
 */
#ifndef XR_OS_FILE_METADATA_PATH_INTERNAL_H
#define XR_OS_FILE_METADATA_PATH_INTERNAL_H
#include "../base/xio_policy.inc.h"
#include "../shared/xr_utf8_core.h"
#include <limits.h>

static XrOsIoStatus metadata_path_owned(const XrOsIoPolicy *policy,
    const char *path, size_t length, bool (*current)(void *context), char **output) {
    if (!io_policy_valid(policy) || !path || !output || *output) return XR_OS_IO_BAD_ARGUMENT;
    if (current && !current(policy->context)) return XR_OS_IO_CANCELLED;
    if (length > INT_MAX) return XR_OS_IO_OVERFLOW;
    if (memchr(path, 0, length) ||
        xr_utf8_core_scan_strict((const uint8_t *)path, length).error != XR_UTF8_OK)
        return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    char *owned = io_alloc(&io, length + 1);
    if (!owned) return io.status == XR_OS_IO_OK ? XR_OS_IO_BAD_ARGUMENT : io.status;
    memcpy(owned, path, length);
    owned[length] = 0;
    if (current && !current(policy->context)) {
        io_free(&io, owned);
        return XR_OS_IO_CANCELLED;
    }
    *output = owned;
    return XR_OS_IO_OK;
}
#endif // XR_OS_FILE_METADATA_PATH_INTERNAL_H
