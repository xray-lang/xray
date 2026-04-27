/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdylib_unix.c - POSIX implementation of xdylib.h.
 *
 * Thin wrapper over dlfcn. XrDylib is the bare void* handle
 * dlopen returns; we cast it through a struct pointer for the
 * same opaque-handle ergonomics the Windows side gets natively
 * out of HMODULE.
 */

#include "../os_dylib.h"

#ifndef _WIN32

#include <dlfcn.h>

XrDylib *xr_dylib_open(const char *path) {
    if (!path)
        return NULL;
    return (XrDylib *) dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

void *xr_dylib_sym(XrDylib *lib, const char *name) {
    if (!lib || !name)
        return NULL;
    // Per POSIX, dlerror should be cleared before dlsym so a
    // NULL-valued symbol is distinguishable from a missing one.
    dlerror();
    return dlsym((void *) lib, name);
}

void xr_dylib_close(XrDylib *lib) {
    if (lib)
        dlclose((void *) lib);
}

const char *xr_dylib_last_error(void) {
    const char *e = dlerror();
    return e ? e : "";
}

#endif  // !_WIN32
