/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * file_owned_unix.c - Explicitly unavailable owned file worker provider
 */
#include "../os_file_owned.h"
XR_FUNC XrOsIoStatus xr_os_io_file_open_begin(const XrOsIoPolicy *memory,const XrOsFileQuery *query,
    const XrOsFileRequest *request,XrOsFileOwner *owner,XrOsFileOperation *operation) {
    (void)memory;(void)query;(void)request;(void)owner;(void)operation;return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_file_begin(const XrOsIoPolicy *memory,const XrOsFileQuery *query,
    const XrOsFileRequest *request,XrOsFileOwner *owner,XrOsFileOperation *operation) {
    (void)memory;(void)query;(void)request;(void)owner;(void)operation;return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_file_poll(XrOsFileOperation *operation,XrOsFileProgress *output) {
    (void)operation;(void)output;return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_file_cancel(XrOsFileOperation *operation) {
    (void)operation;return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_file_operation_close(XrOsFileOperation *operation,bool *closed) {
    (void)operation;(void)closed;return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_file_close(XrOsFileOwner *owner,bool *closed) {
    (void)owner;(void)closed;return XR_OS_IO_UNSUPPORTED;
}
