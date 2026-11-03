/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * pipe_owned_unix.c - Explicitly closed owned-pipe worker capability
 */
#include "../os_pipe_owned.h"
XR_FUNC XrOsIoStatus xr_os_io_pipe_open(const XrOsIoPolicy *memory,const XrOsPipeQuery *query,XrOsPipeOwner *owner,bool *opened){
    (void)memory;(void)query;(void)owner;(void)opened;return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_pipe_begin(XrOsPipeOwner *owner,const XrOsPipeRequest *request,const XrOsPipeQuery *query,XrOsPipeOperation *operation){
    (void)owner;(void)request;(void)query;(void)operation;return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_pipe_poll(XrOsPipeOperation *operation,XrOsPipeProgress *output){
    (void)operation;(void)output;return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_pipe_cancel(XrOsPipeOperation *operation){(void)operation;return XR_OS_IO_UNSUPPORTED;}
XR_FUNC XrOsIoStatus xr_os_io_pipe_operation_close(XrOsPipeOperation *operation,bool *closed){
    (void)operation;(void)closed;return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_pipe_close_end(XrOsPipeOwner *owner,XrOsPipeEnd end,bool *closed){
    (void)owner;(void)end;(void)closed;return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_pipe_dispose(XrOsPipeOwner *owner,bool *closed){(void)owner;(void)closed;return XR_OS_IO_UNSUPPORTED;}
XR_FUNC XrOsIoStatus xr_os_io_pipe_borrow(XrOsPipeOwner *owner,XrOsPipeEnd end,intptr_t *output){
    (void)owner;(void)end;(void)output;return XR_OS_IO_UNSUPPORTED;
}
