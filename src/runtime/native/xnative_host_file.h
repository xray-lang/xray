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
#include "xnative_backing_types.h"
#include "xnative_host_nullable.h"
#include "xnative_value_image.h"
#include "xray_native_values.h"

typedef struct XrtHostFileOpen {
    XrtManagedStatus managed;
    XrOsIoStatus host;
    bool opened;
} XrtHostFileOpen;
typedef struct XrtHostFileRead {
    XrtManagedStatus managed;
    XrOsIoStatus host;
    bool present;
} XrtHostFileRead;
typedef XrtHostNullableOwner XrtHostFileOptional;
typedef struct XrtHostFileStagedResult {
    XrtNativeFileResult location;
    size_t temporary_owner_offset;
} XrtHostFileStagedResult;

XR_FUNC XrtManagedStatus xrt_host_file_bind(XrtBillingOwner *, XrtManagedLease identity, XrtManagedOps **);
XR_FUNC XrtHostFileOpen xrt_host_file_open_read(XrtManagedFrame *, size_t slot, XrtBoundClass,
    const XrOsIoPolicy *, const XrtStringBacking *);
XR_FUNC XrtHostFileOpen xrt_host_file_open_write(XrtManagedFrame *, size_t slot, XrtBoundClass,
    const XrOsIoPolicy *, const XrtStringBacking *, bool append);
/* Publish an exact byte Array to the slot only after one successful read.
 * EOF and valid zero-length requests publish an empty Array. Ordinary
 * refusals leave the slot and its current owner unchanged. */
XR_FUNC XrtHostFileRead xrt_host_file_read(XrtManagedFrame *, size_t slot, XrtBoundClass,
    const XrtManagedHeader *file, const XrtBackingTypes *, int64_t max_bytes);
XR_FUNC void xrt_host_file_open_publish(const XrtNativeFileResult *, XrtBoundClass,
    const XrtStringBacking *, bool append, uint32_t source_id);
/* The caller admits the nominal image type. The bridge preflights the current
 * frame and the exact staged owner byte offset before touching the host.
 * Some and None replace the old outcome. Typed failures preserve it. */
XR_FUNC XrtHostFileOpen xrt_host_file_open_read_publish(const XrtHostFileStagedResult *, XrtBoundClass,
    const XrtStringBacking *, uint32_t source_id);
XR_FUNC XrtHostFileRead xrt_host_file_read_publish(const XrtHostFileStagedResult *,
    XrtBoundClass, const XrtManagedHeader *file, const XrtBackingTypes *, int64_t max_bytes, uint32_t source_id);
XR_FUNC XrtManagedStatus xrt_host_file_close(XrtBoundClass, const XrtManagedHeader *, bool *closed);
XR_FUNC XrOsIoStatus xrt_host_file_write(XrtBoundClass, XrNativeHostShape, const XrtManagedHeader *file,
    const XrtManagedHeader *data, int64_t offset, int64_t *result);
#endif // XNATIVE_HOST_FILE_H
