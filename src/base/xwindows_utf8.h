/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xwindows_utf8.h - Strict owned UTF-16 locators for Windows filesystem calls
 */
#ifndef XR_WINDOWS_UTF8_H
#define XR_WINDOWS_UTF8_H
#include "xmalloc.h"
#include "xio_policy.inc.h"
#include "../shared/xr_win_utf.h"
typedef enum XrWinPathStatus {
    XR_WIN_PATH_OK, XR_WIN_PATH_INVALID, XR_WIN_PATH_LIMIT, XR_WIN_PATH_OOM
} XrWinPathStatus;
static inline XrOsIoStatus io_windows_status(DWORD error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return XR_OS_IO_NOT_FOUND;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return XR_OS_IO_EXISTS;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return XR_OS_IO_OUT_OF_MEMORY;
    case ERROR_FILENAME_EXCED_RANGE: case ERROR_BUFFER_OVERFLOW: return XR_OS_IO_BUDGET;
    case ERROR_INVALID_NAME: case ERROR_BAD_PATHNAME: case ERROR_INVALID_PARAMETER:
    case ERROR_NO_UNICODE_TRANSLATION: return XR_OS_IO_BAD_ARGUMENT;
    default: return XR_OS_IO_IO;
    }
}
static inline XrOsIoStatus xr_win_utf8_path_owned(const XrOsIoPolicy *policy,
    const char *text, wchar_t **output) {
    if (!io_policy_valid(policy) || !text || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    size_t length = 0;
    if (!io_length(&io, text, &length)) return io.status;
    if (length > INT_MAX) return XR_OS_IO_BUDGET;
    int count = 0;
    if (length) {
        if (!io_work(&io, 1 + length)) return io.status;
        count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)length, NULL, 0);
        if (!count) return io_windows_status(GetLastError());
    }
    if (count >= 32768) return XR_OS_IO_BUDGET;
    size_t bytes = ((size_t)count + 1) * sizeof(wchar_t);
    wchar_t *wide = io_alloc(&io, bytes);
    if (!wide) return io.status;
    if (length && io_work(&io, 1 + length + (size_t)count * sizeof(wchar_t)) &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)length, wide, count) != count)
        io_status(&io, io_windows_status(GetLastError()));
    if (io_work(&io, sizeof(wchar_t))) wide[count] = 0;
    if (io.status != XR_OS_IO_OK) io_free(&io, wide);
    else *output = wide;
    return io.status;
}
static inline XrOsIoStatus xr_win_utf16_text_owned(const XrOsIoPolicy *policy,
    const wchar_t *text, char **output) {
    if (!io_policy_valid(policy) || !text || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    size_t length = 0;
    while (io_work(&io, sizeof(wchar_t))) {
        if (!text[length]) break;
        if (length == INT_MAX) return XR_OS_IO_BUDGET;
        ++length;
    }
    if (io.status != XR_OS_IO_OK) return io.status;
    int count = 0;
    if (length) {
        if (!io_work(&io, 1 + length * sizeof(wchar_t))) return io.status;
        count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, (int)length, NULL, 0, NULL, NULL);
        if (!count) return io_windows_status(GetLastError());
    }
    char *bytes = io_alloc(&io, (size_t)count + 1);
    if (!bytes) return io.status;
    if (length && io_work(&io, 1 + length * sizeof(wchar_t) + (size_t)count) &&
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, (int)length, bytes, count, NULL, NULL) != count)
        io_status(&io, io_windows_status(GetLastError()));
    if (io_work(&io, 1)) bytes[count] = 0;
    if (io.status != XR_OS_IO_OK) io_free(&io, bytes);
    else *output = bytes;
    return io.status;
}
static inline void xr_win_utf8_arguments_free(int count, char **arguments) {
    if (!arguments) return;
    for (int i = 0; i < count; ++i) xr_free(arguments[i]);
    xr_free(arguments);
}
static inline char **xr_win_utf16_arguments(int count, wchar_t **arguments, XrWinPathStatus *status) {
    *status = XR_WIN_PATH_INVALID;
    if (count < 0 || (count && !arguments) || (size_t)count > SIZE_MAX / sizeof(char *) - 1) return NULL;
    char **result = xr_calloc((size_t)count + 1, sizeof(*result));
    if (!result) { *status = XR_WIN_PATH_OOM; return NULL; }
    for (int i = 0; i < count; ++i) {
        XrOsIoPolicy policy = xr_os_io_system_policy();
        XrOsIoStatus converted = xr_win_utf16_text_owned(&policy, arguments[i], &result[i]);
        *status = converted == XR_OS_IO_OK ? XR_WIN_PATH_OK : converted == XR_OS_IO_BUDGET ? XR_WIN_PATH_LIMIT :
            converted == XR_OS_IO_OUT_OF_MEMORY ? XR_WIN_PATH_OOM : XR_WIN_PATH_INVALID;
        if (!result[i]) { xr_win_utf8_arguments_free(count, result); return NULL; }
    }
    *status = XR_WIN_PATH_OK;
    return result;
}
#endif // XR_WINDOWS_UTF8_H
