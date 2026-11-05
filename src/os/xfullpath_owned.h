/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfullpath_owned.h - Owned Windows lexical absolute paths without file queries
 */
#ifndef XFULLPATH_OWNED_H
#define XFULLPATH_OWNED_H
#include "../base/xio_policy.inc.h"
#if defined(XR_OS_WINDOWS)
#include "../base/xwindows_utf8.h"
#endif

static inline XrOsIoStatus xr_os_io_fullpath_owned(const XrOsIoPolicy *policy,
    const char *raw,bool (*current)(void *),char **output) {
    if(!io_policy_valid(policy)||!raw||!output||*output)return XR_OS_IO_BAD_ARGUMENT;
    if(current&&!current(policy->context))return XR_OS_IO_CANCELLED;
#if defined(XR_OS_WINDOWS)
    XrIoContext io={policy,XR_OS_IO_OK};wchar_t *wide=NULL,*absolute=NULL;char *text=NULL;
    io_status(&io,xr_win_utf8_text_owned(policy,raw,&wide));
    /* A single fixed caller-owned buffer avoids a separate cwd-dependent
     * sizing query. The UTF-16 locator limit includes its terminator. */
    if(io.status==XR_OS_IO_OK)absolute=io_alloc(&io,32768*sizeof(*absolute));
    if(io.status==XR_OS_IO_OK && current&&!current(policy->context))io.status=XR_OS_IO_CANCELLED;
    if(io.status==XR_OS_IO_OK) {
        DWORD count=GetFullPathNameW(wide,32768,absolute,NULL);
        if(!count)io_status(&io,io_windows_status(GetLastError()));
        else if(count>=32768)io_status(&io,XR_OS_IO_OVERFLOW);
        else if(!absolute[0]||absolute[count])io_status(&io,XR_OS_IO_BAD_ARGUMENT);
    }
    if(io.status==XR_OS_IO_OK && current&&!current(policy->context))io.status=XR_OS_IO_CANCELLED;
    if(io.status==XR_OS_IO_OK)io_status(&io,xr_win_utf16_text_owned(policy,absolute,&text));
    io_free(&io,absolute);io_free(&io,wide);
    if(io.status==XR_OS_IO_OK && current&&!current(policy->context))io.status=XR_OS_IO_CANCELLED;
    if(io.status==XR_OS_IO_OK)*output=text;
    else io_free(&io,text);
    return io.status;
#else
    return XR_OS_IO_UNSUPPORTED;
#endif
}
#endif // XFULLPATH_OWNED_H
