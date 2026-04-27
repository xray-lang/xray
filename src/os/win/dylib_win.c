/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdylib_win.c - Windows implementation of xdylib.h.
 *
 * LoadLibraryA / GetProcAddress / FreeLibrary mapping.
 *
 * Error reporting: dlerror returns a string that lives until the
 * next dlfcn call on the same thread. The Win32 equivalent is
 * GetLastError + FormatMessage; we cache the formatted string in
 * a thread-local buffer to match the same single-line ownership
 * contract.
 *
 * UTF-8 note: paths come in as UTF-8 from xray's module loader.
 * LoadLibraryA uses the active code page, which is typically not
 * UTF-8. Non-ASCII module paths are not round-trip safe in this
 * implementation; a wide-char (LoadLibraryW + MultiByteToWideChar)
 * variant is the right long-term fix and is tracked separately.
 */

#include "../os_dylib.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>

// Thread-local error buffer matching dlerror's per-thread
// ownership semantics. Sized for the longest typical Windows
// system message; longer messages are truncated rather than
// allocated dynamically (this is the error path; allocation
// failures here would be doubly bad).
static __declspec(thread) char tls_err_buf[512];

static void format_last_error_(DWORD code) {
    if (code == 0) {
        tls_err_buf[0] = '\0';
        return;
    }
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code,
                             MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), tls_err_buf,
                             (DWORD) sizeof(tls_err_buf), NULL);
    if (n == 0) {
        // FormatMessage itself failed; fall back to numeric.
        snprintf(tls_err_buf, sizeof(tls_err_buf), "Win32 error %lu", (unsigned long) code);
        return;
    }
    // Trim trailing CRLF that FormatMessageA likes to append.
    while (n > 0 && (tls_err_buf[n - 1] == '\r' || tls_err_buf[n - 1] == '\n' ||
                     tls_err_buf[n - 1] == ' ' || tls_err_buf[n - 1] == '.')) {
        tls_err_buf[--n] = '\0';
    }
}

XrDylib *xr_dylib_open(const char *path) {
    if (!path) {
        snprintf(tls_err_buf, sizeof(tls_err_buf), "xr_dylib_open: NULL path");
        return NULL;
    }
    HMODULE h = LoadLibraryA(path);
    if (!h) {
        format_last_error_(GetLastError());
        return NULL;
    }
    tls_err_buf[0] = '\0';
    return (XrDylib *) h;
}

void *xr_dylib_sym(XrDylib *lib, const char *name) {
    if (!lib || !name) {
        snprintf(tls_err_buf, sizeof(tls_err_buf), "xr_dylib_sym: NULL handle or name");
        return NULL;
    }
    FARPROC p = GetProcAddress((HMODULE) lib, name);
    if (!p) {
        format_last_error_(GetLastError());
        return NULL;
    }
    tls_err_buf[0] = '\0';
    // FARPROC → void* cast is permitted by Win32 conventions
    // even though strict ISO C disallows function-to-data
    // conversion. Modules use this to retrieve both function
    // entry points and data exports.
    return (void *) (uintptr_t) p;
}

void xr_dylib_close(XrDylib *lib) {
    if (!lib)
        return;
    if (!FreeLibrary((HMODULE) lib))
        format_last_error_(GetLastError());
}

const char *xr_dylib_last_error(void) {
    return tls_err_buf;
}
