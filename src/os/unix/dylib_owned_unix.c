/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * dylib_owned_unix.c - Closed loader boundary until owned POSIX reception exists
 */
#include "../os_dylib_owned.h"
XR_FUNC XrOsIoStatus xr_os_io_dylib_open(const XrOsIoPolicy *memory,const XrOsDylibQuery *query,
    XrOsDylibText path,XrOsDylibOwner *owner,bool *opened) {
    (void)memory;(void)query;(void)path;(void)owner;(void)opened;return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_dylib_symbol(XrOsDylibOwner *owner,const XrOsDylibQuery *query,
    XrOsDylibText name,uintptr_t *address,bool *found) {
    (void)owner;(void)query;(void)name;(void)address;(void)found;return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_dylib_close(XrOsDylibOwner *owner,bool *closed) {
    (void)owner;(void)closed;return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_dylib_last_error(const XrOsIoPolicy *memory,
    const XrOsDylibQuery *query,XrOsDylibErrorText *output) {
    (void)memory;(void)query;(void)output;return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC void xr_os_io_dylib_error_free(XrOsDylibErrorText *text) {
    if(!text)return;
    if(text->text)text->memory.free(text->memory.context,text->text);
    *text=(XrOsDylibErrorText){0};
}
