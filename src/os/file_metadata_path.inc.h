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

typedef struct MetadataPathScan {
    const XrOsIoPolicy *policy;
    bool (*current)(void *context);
    size_t reads;
    XrOsIoStatus status;
} MetadataPathScan;
static int metadata_path_read(void *context, const uint8_t *address, uint8_t *output) {
    MetadataPathScan *scan = context;
    if ((scan->reads++ & 4095u) == 0 && scan->current && !scan->current(scan->policy->context)) {
        scan->status = XR_OS_IO_CANCELLED;
        return 0;
    }
    uint8_t byte = *address;
    if (!byte) { scan->status = XR_OS_IO_BAD_ARGUMENT; return 0; }
    *output = byte;
    return 1;
}
static XrOsIoStatus metadata_path_owned(const XrOsIoPolicy *policy,
    const char *path, size_t length, bool (*current)(void *context), char **output) {
    if (!io_policy_valid(policy) || !path || !output || *output) return XR_OS_IO_BAD_ARGUMENT;
    if (current && !current(policy->context)) return XR_OS_IO_CANCELLED;
    if (length > INT_MAX) return XR_OS_IO_OVERFLOW;
    MetadataPathScan scan = {policy, current, 0, XR_OS_IO_OK};
    XrUtf8ScanResult scanned;
    if (!xr_utf8_core_scan_strict_read((const uint8_t *)path, length, metadata_path_read, &scan, &scanned))
        return scan.status == XR_OS_IO_OK ? XR_OS_IO_BAD_ARGUMENT : scan.status;
    if (scanned.error != XR_UTF8_OK) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    char *owned = io_alloc(&io, length + 1);
    if (!owned) return io.status == XR_OS_IO_OK ? XR_OS_IO_BAD_ARGUMENT : io.status;
    size_t offset = 0;
    while (offset < length) {
        if (current && !current(policy->context)) {
            io_free(&io, owned);
            return XR_OS_IO_CANCELLED;
        }
        size_t chunk = length - offset < 4096 ? length - offset : 4096;
        memcpy(owned + offset, path + offset, chunk);
        offset += chunk;
    }
    owned[length] = 0;
    if (current && !current(policy->context)) {
        io_free(&io, owned);
        return XR_OS_IO_CANCELLED;
    }
    *output = owned;
    return XR_OS_IO_OK;
}
#endif // XR_OS_FILE_METADATA_PATH_INTERNAL_H
