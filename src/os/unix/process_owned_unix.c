/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * process_owned_unix.c - Closed boundary until owned child retirement is available
 */
#include "../os_process_owned.h"
XR_FUNC XrOsIoStatus xr_os_io_process_prepare(const XrOsIoPolicy *memory,
    const XrOsProcessQuery *query,const XrOsProcessRequest *request,XrOsProcessOwner *owner,bool *created) {
    (void)memory;(void)query;(void)request;(void)owner;(void)created;return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_process_start(XrOsProcessOwner *owner) {(void)owner;return XR_OS_IO_UNSUPPORTED;}
XR_FUNC XrOsIoStatus xr_os_io_process_poll(XrOsProcessOwner *owner,XrOsProcessProgress *output) {
    (void)owner;(void)output;return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_process_kill(XrOsProcessOwner *owner,int64_t signal,bool *sent) {
    (void)owner;(void)signal;(void)sent;return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_process_close(XrOsProcessOwner *owner,bool *closed) {
    (void)owner;(void)closed;return XR_OS_IO_UNSUPPORTED;
}
