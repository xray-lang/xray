/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_file.h - Runtime-owned synchronous file storage
 */
#ifndef XNATIVE_HOST_FILE_H
#define XNATIVE_HOST_FILE_H
#include "os/os_file_owned.h"
#include "shared/xnative_host_leaf.h"
#include "xnative_value_image.h"
#include "xray_native_values.h"

typedef struct XrtHostFileOpen {
    XrtManagedStatus managed;
    XrOsIoStatus host;
    bool opened;
} XrtHostFileOpen;
typedef struct XrtHostFileOptional {
    uint8_t present;
    XrtManagedHeader *owner;
} XrtHostFileOptional;

XR_FUNC XrtManagedStatus xrt_host_file_bind(XrtBillingOwner *, XrtManagedLease identity, XrtManagedOps **);
XR_FUNC XrtHostFileOpen xrt_host_file_open_write(XrtManagedFrame *, size_t slot, XrtBoundClass,
    const XrOsIoPolicy *, const XrtStringBacking *, bool append);
XR_FUNC void xrt_host_file_open_publish(const XrtNativeFileResult *, XrtBoundClass,
    const XrtStringBacking *, bool append, uint32_t source_id);
XR_FUNC XrtManagedStatus xrt_host_file_close(XrtBoundClass, const XrtManagedHeader *, bool *closed);
XR_FUNC XrOsIoStatus xrt_host_file_write(XrtBoundClass, XrNativeHostShape, const XrtManagedHeader *file,
    const XrtManagedHeader *data, int64_t offset, int64_t *result);
#endif // XNATIVE_HOST_FILE_H
