/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcwd_owned.h - One policy-owned current-directory query for OS consumers
 *
 * KEY CONCEPT:
 *   Only a complete, strictly encoded directory is published. Capacity growth
 *   spends the caller's policy and never decodes an insufficient OS buffer.
 */
#ifndef XCWD_OWNED_H
#define XCWD_OWNED_H
#include "../base/xio_policy.inc.h"
#if defined(XR_OS_WINDOWS)
#include "../base/xwindows_utf8.h"
#else
#include "../shared/xr_utf8_core.h"
#include <unistd.h>
#endif

static inline XrOsIoStatus xr_os_io_getcwd_owned(const XrOsIoPolicy *policy,char **output) {
    if(!io_policy_valid(policy) || !output || *output)return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io={policy,XR_OS_IO_OK};
#if defined(XR_OS_WINDOWS)
    DWORD capacity=GetCurrentDirectoryW(0,NULL);
    if(!capacity)return io_windows_status(GetLastError());
    for(;;) {
        if((uint64_t)capacity>SIZE_MAX/sizeof(wchar_t))return XR_OS_IO_OVERFLOW;
        wchar_t *wide=io_alloc(&io,(size_t)capacity*sizeof(*wide));
        if(!wide)return io.status;
        DWORD length=GetCurrentDirectoryW(capacity,wide);
        if(!length)io_status(&io,io_windows_status(GetLastError()));
        if(io.status==XR_OS_IO_OK && length>=capacity) {
            io_free(&io,wide);
            if(capacity==UINT32_MAX)return XR_OS_IO_OVERFLOW;
            DWORD doubled=capacity>UINT32_MAX/2?UINT32_MAX:capacity*2;
            capacity=length>doubled?length:doubled;continue;
        }
        if(io.status==XR_OS_IO_OK && (!wide[0] || wide[length]))io_status(&io,XR_OS_IO_BAD_ARGUMENT);
        char *text=NULL;
        if(io.status==XR_OS_IO_OK)io_status(&io,xr_win_utf16_text_owned(policy,wide,&text));
        io_free(&io,wide);
        if(io.status==XR_OS_IO_OK)*output=text;
        else io_free(&io,text);
        return io.status;
    }
#else
    size_t capacity=256;
    for(;;) {
        char *text=io_alloc(&io,capacity);
        if(!text)return io.status;
        if(!getcwd(text,capacity)) {
            int error=errno;io_free(&io,text);
            if(error!=ERANGE)return io_errno_status(error);
            if(capacity>SIZE_MAX/2)return XR_OS_IO_OVERFLOW;
            capacity*=2;continue;
        }
        size_t length=0;
        if(io_length(&io,text,&length) && (!length || xr_utf8_core_scan_strict((const uint8_t *)text,length).error!=XR_UTF8_OK))
            io_status(&io,XR_OS_IO_BAD_ARGUMENT);
        if(io.status==XR_OS_IO_OK)*output=text;
        else io_free(&io,text);
        return io.status;
    }
#endif
}
#endif // XCWD_OWNED_H
