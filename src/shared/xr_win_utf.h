/*
 * xray - Windows UTF-8/UTF-16 boundary helpers
 *
 * Windows process and environment APIs use UTF-16.  Xray-owned text uses
 * strict UTF-8.  These allocation-free primitives keep the conversion policy
 * shared by the compiler process shim and the embedded AOT runtime while
 * leaving allocation ownership with each caller.
 */

#ifndef XR_WIN_UTF_H
#define XR_WIN_UTF_H

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>

static inline int xr_win_utf8_to_utf16_required(const char *input, size_t input_len) {
    if (!input || input_len > INT_MAX || memchr(input, '\0', input_len) != NULL)
        return 0;
    if (input_len == 0)
        return 1;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, (int) input_len, NULL, 0);
    return count > 0 && count < INT_MAX ? count + 1 : 0;
}

static inline int xr_win_utf8_to_utf16(const char *input, size_t input_len, wchar_t *output,
                                       size_t output_cap) {
    int required = xr_win_utf8_to_utf16_required(input, input_len);
    if (required == 0 || !output || output_cap < (size_t) required)
        return 0;
    if (input_len > 0 &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, (int) input_len, output,
                            required - 1) != required - 1)
        return 0;
    output[required - 1] = L'\0';
    return required;
}

static inline int xr_win_utf16_to_utf8_required(const wchar_t *input, size_t input_len) {
    if (!input || input_len > INT_MAX || wmemchr(input, L'\0', input_len) != NULL)
        return 0;
    if (input_len == 0)
        return 1;
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, (int) input_len, NULL, 0,
                                    NULL, NULL);
    return count > 0 && count < INT_MAX ? count + 1 : 0;
}

static inline int xr_win_utf16_to_utf8(const wchar_t *input, size_t input_len, char *output,
                                       size_t output_cap) {
    int required = xr_win_utf16_to_utf8_required(input, input_len);
    if (required == 0 || !output || output_cap < (size_t) required)
        return 0;
    if (input_len > 0 &&
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, (int) input_len, output,
                            required - 1, NULL, NULL) != required - 1)
        return 0;
    output[required - 1] = '\0';
    return required;
}

#endif /* _WIN32 */

#endif /* XR_WIN_UTF_H */
