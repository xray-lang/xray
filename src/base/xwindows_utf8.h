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
#include "../shared/xr_win_utf.h"
typedef enum XrWinPathStatus {
    XR_WIN_PATH_OK, XR_WIN_PATH_INVALID, XR_WIN_PATH_LIMIT, XR_WIN_PATH_OOM
} XrWinPathStatus;
static inline wchar_t *xr_win_utf8_path(const char *text, XrWinPathStatus *status) {
    *status = XR_WIN_PATH_INVALID;
    if (!text) return NULL;
    size_t length = strlen(text);
    int units = xr_win_utf8_to_utf16_required(text, length);
    if (units <= 0) return NULL;
    if (units > 32768) { *status = XR_WIN_PATH_LIMIT; return NULL; }
    wchar_t *result = xr_malloc((size_t)units * sizeof(wchar_t));
    if (!result) { *status = XR_WIN_PATH_OOM; return NULL; }
    if (xr_win_utf8_to_utf16(text, length, result, (size_t)units) != units) {
        xr_free(result); return NULL;
    }
    *status = XR_WIN_PATH_OK;
    return result;
}
static inline char *xr_win_utf16_text(const wchar_t *text, XrWinPathStatus *status) {
    *status = XR_WIN_PATH_INVALID;
    if (!text) return NULL;
    size_t length = wcslen(text);
    int bytes = xr_win_utf16_to_utf8_required(text, length);
    if (bytes <= 0) return NULL;
    char *result = xr_malloc((size_t)bytes);
    if (!result) { *status = XR_WIN_PATH_OOM; return NULL; }
    if (xr_win_utf16_to_utf8(text, length, result, (size_t)bytes) != bytes) {
        xr_free(result); return NULL;
    }
    *status = XR_WIN_PATH_OK;
    return result;
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
        result[i] = xr_win_utf16_text(arguments[i], status);
        if (!result[i]) { xr_win_utf8_arguments_free(count, result); return NULL; }
    }
    *status = XR_WIN_PATH_OK;
    return result;
}
#endif // XR_WINDOWS_UTF8_H
