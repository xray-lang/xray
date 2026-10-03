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
static inline XrOsIoStatus xr_win_utf8_owned(const XrOsIoPolicy *policy,
    const char *text, wchar_t **output, size_t *units) {
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
    else { *output = wide; if (units) *units = (size_t)count; }
    return io.status;
}
static inline XrOsIoStatus xr_win_utf8_text_owned(const XrOsIoPolicy *policy,
    const char *text, wchar_t **output) {
    return xr_win_utf8_owned(policy, text, output, NULL);
}
/* Extended locators must not turn ordinary DOS device or trimmed names into
 * different filesystem objects. Already explicit device locators are unchanged. */
static inline bool xr_win_path_component(XrIoContext *io, const wchar_t *text, size_t length) {
    if (!length) return true;
    if (!io_work(io, sizeof(wchar_t))) return false;
    if (text[length - 1] == L'.' || text[length - 1] == L' ')
        return io_status(io, XR_OS_IO_BAD_ARGUMENT);
    wchar_t stem[8]; size_t count = 0;
    while (count < length && count < 8) {
        if (!io_work(io, sizeof(wchar_t))) return false;
        wchar_t c = text[count];
        if (c == L'.' || c == L':') break;
        stem[count++] = c >= L'a' && c <= L'z' ? c - (L'a' - L'A') : c;
    }
    bool device = (count == 3 && ((stem[0] == L'C' && stem[1] == L'O' && stem[2] == L'N') ||
        (stem[0] == L'P' && stem[1] == L'R' && stem[2] == L'N') ||
        (stem[0] == L'A' && stem[1] == L'U' && stem[2] == L'X') ||
        (stem[0] == L'N' && stem[1] == L'U' && stem[2] == L'L'))) ||
        (count == 4 && ((stem[0] == L'C' && stem[1] == L'O' && stem[2] == L'M') ||
        (stem[0] == L'L' && stem[1] == L'P' && stem[2] == L'T')) &&
        ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == 0xB9 || stem[3] == 0xB2 || stem[3] == 0xB3));
    if (count == 6 || count == 7) {
        if (!io_work(io, count * sizeof(wchar_t))) return false;
        device = device || (count == 6 && !memcmp(stem, L"CONIN$", 6 * sizeof(wchar_t))) ||
            (count == 7 && !memcmp(stem, L"CONOUT$", 7 * sizeof(wchar_t)));
    }
    return !device || io_status(io, XR_OS_IO_BAD_ARGUMENT);
}
static inline bool xr_win_path_components(XrIoContext *io, const wchar_t *wide, size_t count) {
    if (!io_work(io, 2 * sizeof(wchar_t))) return false;
    bool unc = count >= 2 && (wide[0] == L'\\' || wide[0] == L'/') &&
        (wide[1] == L'\\' || wide[1] == L'/');
    size_t start = unc ? 2 : count >= 2 && wide[1] == L':' ? 2 : 0;
    unsigned unc_parts = unc ? 2 : 0;
    size_t segment = start;
    for (size_t i = start; i <= count; ++i) {
        if (!io_work(io, sizeof(wchar_t))) return false;
        if (wide[i] != L'\\' && wide[i] != L'/' && wide[i]) continue;
        size_t length = i - segment;
        if (unc_parts && length) --unc_parts;
        else if (length) {
            if (!io_work(io, (length < 3 ? length : 1) * sizeof(wchar_t))) return false;
            bool dot = (length == 1 && wide[segment] == L'.') ||
                (length == 2 && wide[segment] == L'.' && wide[segment + 1] == L'.');
            if (!dot && !xr_win_path_component(io, wide + segment, length)) return false;
        }
        segment = i + 1;
    }
    return true;
}
/* The slot owns a policy allocation. Success may replace it; failure leaves
 * the original allocation intact for the caller's unconditional cleanup. */
static inline XrOsIoStatus xr_win_path_locator_owned(const XrOsIoPolicy *policy,
    wchar_t **slot, size_t count) {
    if (!io_policy_valid(policy) || !slot || !*slot) return XR_OS_IO_BAD_ARGUMENT;
    wchar_t *wide = *slot;
    XrIoContext io = {policy, XR_OS_IO_OK};
    if (count >= 32768) return XR_OS_IO_BUDGET;
    if (count < MAX_PATH - 12) return XR_OS_IO_OK;
    if (!io_work(&io, 4 * sizeof(wchar_t))) return io.status;
    if (wide[0] == L'\\' && wide[1] == L'\\' && (wide[2] == L'?' || wide[2] == L'.') && wide[3] == L'\\') {
        return XR_OS_IO_OK;
    }
    if (!xr_win_path_components(&io, wide, count)) return io.status;
    /* Resolve relative forms exactly once: the current directory is process
     * global. Fixed scratch avoids a query/fill race and is caller-accounted. */
    wchar_t *absolute = io_alloc(&io, 32768 * sizeof(wchar_t)), *locator = NULL;
    DWORD length = 0;
    if (absolute && io_work(&io, 1 + (count + 1 + 32768) * sizeof(wchar_t))) {
        length = GetFullPathNameW(wide, 32768, absolute, NULL);
        if (!length) io_status(&io, io_windows_status(GetLastError()));
        else if (length >= 32768) io_status(&io, XR_OS_IO_BUDGET);
    }
    size_t skip = 0, prefix = 4;
    if (io.status == XR_OS_IO_OK && io_work(&io, 3 * sizeof(wchar_t))) {
        if (absolute[0] == L'\\' && absolute[1] == L'\\') { skip = 2; prefix = 8; }
        else if (!((absolute[0] >= L'A' && absolute[0] <= L'Z') || (absolute[0] >= L'a' && absolute[0] <= L'z')) ||
            absolute[1] != L':' || absolute[2] != L'\\') io_status(&io, XR_OS_IO_BAD_ARGUMENT);
    }
    if (io.status == XR_OS_IO_OK) (void)xr_win_path_components(&io, absolute, length);
    size_t locator_length = prefix + length - skip;
    if (io.status == XR_OS_IO_OK && locator_length >= 32768) io_status(&io, XR_OS_IO_BUDGET);
    if (io.status == XR_OS_IO_OK) locator = io_alloc(&io, (locator_length + 1) * sizeof(wchar_t));
    if (locator && io_copy(&io, locator, skip ? L"\\\\?\\UNC\\" : L"\\\\?\\", prefix * sizeof(wchar_t)))
        io_copy(&io, locator + prefix, absolute + skip, (length - skip + 1) * sizeof(wchar_t));
    io_free(&io, absolute);
    if (io.status == XR_OS_IO_OK) { io_free(&io, wide); *slot = locator; }
    else io_free(&io, locator);
    return io.status;
}
static inline XrOsIoStatus xr_win_utf8_path_owned(const XrOsIoPolicy *policy,
    const char *text, wchar_t **output) {
    if (!output) return XR_OS_IO_BAD_ARGUMENT;
    wchar_t *wide = NULL; size_t count = 0;
    XrOsIoStatus status = xr_win_utf8_owned(policy, text, &wide, &count);
    if (status != XR_OS_IO_OK) return status;
    status = xr_win_path_locator_owned(policy, &wide, count);
    if (status == XR_OS_IO_OK) *output = wide;
    else policy->free(policy->context, wide);
    return status;
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
