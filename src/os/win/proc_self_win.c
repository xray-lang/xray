/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * proc_self_win.c - Current-process queries without spawning dependencies
 */
#include "../os_proc.h"
#include "../../shared/xr_win_utf.h"
#include "../../base/xwindows_utf8.h"

/* Both runtime and owned queries read the executable through this one call.
 * The fixed-buffer runtime query does not acquire a compiler resource domain. */
static XrOsIoStatus proc_self_executable(wchar_t *buffer, DWORD capacity, DWORD *length) {
    DWORD count = GetModuleFileNameW(NULL, buffer, capacity);
    if (!count) return io_windows_status(GetLastError());
    if (count >= capacity) return XR_OS_IO_OVERFLOW;
    *length = count;
    return XR_OS_IO_OK;
}
static XrOsIoStatus proc_environment_error(DWORD error) {
    if (error == ERROR_ENVVAR_NOT_FOUND) return XR_OS_IO_NOT_FOUND;
    XrOsIoStatus status = io_windows_status(error);
    return status == XR_OS_IO_NOT_FOUND ? XR_OS_IO_IO : status;
}

XR_FUNC XrOsIoStatus xr_os_io_self_exe_path(const XrOsIoPolicy *policy, char **output) {
    if (!io_policy_valid(policy) || !output || *output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    char *result = NULL;
    for (DWORD capacity = 256; io.status == XR_OS_IO_OK; capacity *= 2) {
        size_t bytes = (size_t)capacity * sizeof(wchar_t);
        wchar_t *wide = io_alloc(&io, bytes);
        DWORD length = 0;
        XrOsIoStatus queried = XR_OS_IO_OK;
        if (wide && ((&io)->status == XR_OS_IO_OK)) queried = proc_self_executable(wide, capacity, &length);
        if (io.status == XR_OS_IO_OK && queried == XR_OS_IO_OVERFLOW && capacity < 32768) {
            io_free(&io, wide);
            continue;
        }
        io_status(&io, queried);
        if (io.status == XR_OS_IO_OK) io_status(&io, xr_win_utf16_text_owned(policy, wide, &result));
        io_free(&io, wide);
        break;
    }
    if (io.status == XR_OS_IO_OK) *output = result;
    return io.status;
}

XR_FUNC XrOsIoStatus xr_os_io_environment_get(const XrOsIoPolicy *policy,
    const char *name, char **output) {
    if (!io_policy_valid(policy) || !name || !output || *output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    size_t length = 0;
    if (!io_length(&io, name, &length)) return io.status;
    if (!length) return XR_OS_IO_BAD_ARGUMENT;
    for (size_t i = 0; i < length; ++i) {
        if (!((&io)->status == XR_OS_IO_OK)) return io.status;
        if (name[i] == '=') return XR_OS_IO_BAD_ARGUMENT;
    }
    wchar_t *key = NULL, *value = NULL;
    char *result = NULL;
    io_status(&io, xr_win_utf8_text_owned(policy, name, &key));
    DWORD capacity = 0;
    if ((&io)->status == XR_OS_IO_OK) {
        SetLastError(ERROR_SUCCESS);
        capacity = GetEnvironmentVariableW(key, NULL, 0);
        DWORD error = GetLastError();
        if (!capacity && error != ERROR_SUCCESS)
            io_status(&io, proc_environment_error(error));
    }
    if (io.status == XR_OS_IO_OK) {
        if (capacity > 32768) io_status(&io, XR_OS_IO_OVERFLOW);
        else {
            if (!capacity) capacity = 1;
            value = io_alloc(&io, (size_t)capacity * sizeof(wchar_t));
        }
    }
    if (value && ((&io)->status == XR_OS_IO_OK)) {
        SetLastError(ERROR_SUCCESS);
        DWORD count = GetEnvironmentVariableW(key, value, capacity);
        DWORD error = GetLastError();
        if (!count && error != ERROR_SUCCESS)
            io_status(&io, proc_environment_error(error));
        else if (count >= capacity) io_status(&io, XR_OS_IO_IO);
        else if ((&io)->status == XR_OS_IO_OK) value[count] = 0;
    }
    if (io.status == XR_OS_IO_OK) io_status(&io, xr_win_utf16_text_owned(policy, value, &result));
    io_free(&io, value); io_free(&io, key);
    if (io.status == XR_OS_IO_OK) *output = result;
    return io.status;
}

XR_FUNC XrOsIoStatus xr_os_io_environment_set(const XrOsIoPolicy *policy,
    const char *name,const char *value,bool (*current)(void *context),bool *output) {
    if(!io_policy_valid(policy) || !name || !current || !output)return XR_OS_IO_BAD_ARGUMENT;
    if(!*name || strchr(name,'=')) { *output=false;return XR_OS_IO_OK; }
    XrIoContext io={policy,XR_OS_IO_OK};wchar_t *key=NULL,*text=NULL;
    const char *inputs[2]={name,value};
    for(uint32_t i=0;i<(value?2u:1u);++i) {
        size_t length=0;if(!io_length(&io,inputs[i],&length))return io.status;
        if(length>INT_MAX)return XR_OS_IO_OVERFLOW;
        SetLastError(ERROR_SUCCESS);
        int required=xr_win_utf8_to_utf16_required(inputs[i],length);
        if(!required)return GetLastError()==ERROR_NO_UNICODE_TRANSLATION?XR_OS_IO_BAD_ARGUMENT:
            length==INT_MAX?XR_OS_IO_OVERFLOW:XR_OS_IO_BAD_ARGUMENT;
        /* The count includes NUL. Platform text limits are normal refusals;
         * allocation and arithmetic overflow keep their resource status. */
        if(required>32768) {
            if(!current(policy->context))return XR_OS_IO_CANCELLED;
            *output=false;return XR_OS_IO_OK;
        }
    }
    io_status(&io,xr_win_utf8_text_owned(policy,name,&key));
    if(io.status==XR_OS_IO_OK && value)io_status(&io,xr_win_utf8_text_owned(policy,value,&text));
    if(io.status==XR_OS_IO_OK && !current(policy->context))io_status(&io,XR_OS_IO_CANCELLED);
    if(io.status==XR_OS_IO_OK)*output=SetEnvironmentVariableW(key,text)!=0;
    io_free(&io,text);io_free(&io,key);return io.status;
}

XR_FUNC XrOsIoStatus xr_os_io_environment_snapshot_open(XrOsEnvironmentSnapshot *snapshot) {
    if(!snapshot || snapshot->block)return XR_OS_IO_BAD_ARGUMENT;
    snapshot->block=GetEnvironmentStringsW();
    return snapshot->block?XR_OS_IO_OK:io_windows_status(GetLastError());
}
XR_FUNC XrOsIoStatus xr_os_io_environment_snapshot_each(const XrOsIoPolicy *policy,
    const XrOsEnvironmentSnapshot *snapshot,bool (*current)(void *context,size_t work),
    XrOsIoStatus (*visitor)(void *context,const char *text,size_t length)) {
    if(!io_policy_valid(policy) || !snapshot || !snapshot->block || !current || !visitor)
        return XR_OS_IO_BAD_ARGUMENT;
    for(const wchar_t *entry=snapshot->block;*entry;) {
        size_t length=0;
        for(;;) {
            if(!(length%256) && !current(policy->context,256))return XR_OS_IO_CANCELLED;
            if(!entry[length])break;
            if(length==INT_MAX)return XR_OS_IO_OVERFLOW;
            ++length;
        }
        if(length>(SIZE_MAX-1)/8)return XR_OS_IO_OVERFLOW;
        if(!current(policy->context,8*length+1))return XR_OS_IO_CANCELLED;
        char *text=NULL;
        XrOsIoStatus status=xr_win_utf16_text_owned(policy,entry,&text);
        if(status==XR_OS_IO_OK)status=visitor(policy->context,text,strlen(text));
        if(text)policy->free(policy->context,text);
        if(status!=XR_OS_IO_OK)return status;
        entry+=length+1;
    }
    return current(policy->context,1)?XR_OS_IO_OK:XR_OS_IO_CANCELLED;
}
XR_FUNC XrOsIoStatus xr_os_io_environment_snapshot_close(XrOsEnvironmentSnapshot *snapshot) {
    if(!snapshot)return XR_OS_IO_BAD_ARGUMENT;
    if(!snapshot->block)return XR_OS_IO_OK;
    if(!FreeEnvironmentStringsW(snapshot->block))return XR_OS_IO_IO;
    snapshot->block=NULL;return XR_OS_IO_OK;
}

int64_t xr_proc_self_pid(void) {
    return (int64_t) GetCurrentProcessId();
}

int xr_proc_self_exe_path(char *buf, size_t size) {
    if (buf == NULL || size == 0) {
        return -1;
    }
    wchar_t wide[32768];
    DWORD n = 0;
    /* n == 0 means failure; n == capacity means truncation. */
    if (proc_self_executable(wide, (DWORD)(sizeof(wide) / sizeof(wide[0])), &n) != XR_OS_IO_OK ||
        !xr_win_utf16_to_utf8(wide, (size_t) n, buf, size)) {
        return -1;
    }
    return 0;
}

bool xr_proc_debugger_attached(void) {
    return IsDebuggerPresent() ? true : false;
}
