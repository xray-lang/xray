/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * pipe_win.c - Windows implementation of os_pipe.h.
 */

#include "../os_pipe.h"

#include <limits.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HANDLE pipe_handle(XrPipeHandle handle) {
    return (HANDLE) (intptr_t) handle;
}

static int set_inheritable(HANDLE handle, bool inheritable) {
    DWORD flags = inheritable ? HANDLE_FLAG_INHERIT : 0;
    return SetHandleInformation(handle, HANDLE_FLAG_INHERIT, flags) ? 0 : -1;
}

int xr_pipe_create(XrPipe *out, const XrPipeOptions *options) {
    if (!out) {
        return -1;
    }
    out->read = XR_PIPE_INVALID;
    out->write = XR_PIPE_INVALID;

    bool read_inheritable = options && options->read_inheritable;
    bool write_inheritable = options && options->write_inheritable;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = read_inheritable || write_inheritable;

    HANDLE read_handle = NULL;
    HANDLE write_handle = NULL;
    if (!CreatePipe(&read_handle, &write_handle, &sa, 0)) {
        return -1;
    }

    if (set_inheritable(read_handle, read_inheritable) != 0 ||
        set_inheritable(write_handle, write_inheritable) != 0) {
        CloseHandle(read_handle);
        CloseHandle(write_handle);
        return -1;
    }

    out->read = (XrPipeHandle) (intptr_t) read_handle;
    out->write = (XrPipeHandle) (intptr_t) write_handle;
    return 0;
}

int xr_pipe_close(XrPipeHandle handle) {
    if (handle == XR_PIPE_INVALID) {
        return 0;
    }
    return CloseHandle(pipe_handle(handle)) ? 0 : -1;
}

int64_t xr_pipe_read(XrPipeHandle handle, void *buf, size_t len) {
    if (handle == XR_PIPE_INVALID || (!buf && len > 0)) {
        return -1;
    }
    DWORD chunk = len > (size_t) UINT_MAX ? (DWORD) UINT_MAX : (DWORD) len;
    DWORD read_bytes = 0;
    if (!ReadFile(pipe_handle(handle), buf, chunk, &read_bytes, NULL)) {
        return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    }
    return (int64_t) read_bytes;
}

XrPipeIoStatus xr_pipe_try_read(XrPipeHandle handle, void *buf, size_t len, int64_t *out_n) {
    if (out_n) {
        *out_n = xr_pipe_read(handle, buf, len);
        return *out_n < 0 ? XR_PIPE_IO_ERROR : XR_PIPE_IO_OK;
    }
    return XR_PIPE_IO_ERROR;
}

int64_t xr_pipe_write(XrPipeHandle handle, const void *buf, size_t len) {
    if (handle == XR_PIPE_INVALID || (!buf && len > 0)) {
        return -1;
    }
    DWORD chunk = len > (size_t) UINT_MAX ? (DWORD) UINT_MAX : (DWORD) len;
    DWORD written = 0;
    if (!WriteFile(pipe_handle(handle), buf, chunk, &written, NULL)) {
        return -1;
    }
    return (int64_t) written;
}

XrPipeIoStatus xr_pipe_try_write(XrPipeHandle handle, const void *buf, size_t len, int64_t *out_n) {
    if (out_n) {
        *out_n = xr_pipe_write(handle, buf, len);
        return *out_n < 0 ? XR_PIPE_IO_ERROR : XR_PIPE_IO_OK;
    }
    return XR_PIPE_IO_ERROR;
}
