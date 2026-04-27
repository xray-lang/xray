/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdylib.h - Cross-platform dynamic library loading.
 *
 * KEY CONCEPT:
 *   POSIX dlfcn (dlopen / dlsym / dlclose / dlerror) does not
 *   exist on Windows; the equivalent is LoadLibrary /
 *   GetProcAddress / FreeLibrary / FormatMessage(GetLastError()).
 *   This header presents a single API that maps onto either.
 *
 *   Usage:
 *     XrDylib *lib = xr_dylib_open(path);
 *     if (!lib) {
 *         fprintf(stderr, "%s\n", xr_dylib_last_error());
 *         return -1;
 *     }
 *     void *fn = xr_dylib_sym(lib, "init");
 *     ...
 *     xr_dylib_close(lib);
 *
 * SEMANTICS:
 *   xr_dylib_open uses the platform's "resolve all imports
 *   immediately" mode (dlopen RTLD_NOW on POSIX, default for
 *   LoadLibrary on Windows) and local symbol scope (RTLD_LOCAL
 *   on POSIX, also the Windows default). Lazy / global modes
 *   are not exposed: every existing caller in the runtime wants
 *   a fully-resolved private library handle.
 *
 *   xr_dylib_last_error returns a process-wide last-error
 *   message valid until the next library call. It mirrors
 *   dlerror's contract on POSIX; on Windows it formats the
 *   thread's last error into a static thread-local buffer.
 *
 * RELATED MODULES:
 *   - base/xdefs.h for visibility macros
 */

#ifndef XDYLIB_H
#define XDYLIB_H

#include "xdefs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XrDylib XrDylib;

// Open `path` as a dynamic library. Returns NULL on failure;
// xr_dylib_last_error then describes the cause.
XR_FUNC XrDylib *xr_dylib_open(const char *path);

// Resolve symbol `name` to a function or data pointer. Returns
// NULL when the symbol is missing; callers who need to
// distinguish a NULL-valued symbol from a missing symbol must
// consult xr_dylib_last_error.
XR_FUNC void *xr_dylib_sym(XrDylib *lib, const char *name);

// Release the library handle. Safe to call with NULL.
XR_FUNC void xr_dylib_close(XrDylib *lib);

// Last error message from the most recent xr_dylib_* call on
// the current thread. Returns a non-NULL string but the
// contents are unspecified when no error has occurred.
XR_FUNC const char *xr_dylib_last_error(void);

#ifdef __cplusplus
}
#endif

#endif  // XDYLIB_H
