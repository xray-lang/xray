/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_os_core.h - Runtime-neutral OS stdlib core helpers.
 */

#ifndef XRAY_SHARED_XR_OS_CORE_H
#define XRAY_SHARED_XR_OS_CORE_H

#include "../base/xplatform.h"
#include <stdbool.h>

typedef const char *(*XrOsCoreGetenvFn)(void *ctx, const char *name);

static inline const char *xr_os_core_platform(void) {
#if defined(XR_OS_WINDOWS) || defined(_WIN64)
    return "windows";
#elif defined(XR_OS_MACOS) && defined(__MACH__)
    return "darwin";
#elif defined(XR_OS_LINUX)
    return "linux";
#elif defined(XR_OS_BSD)
    return "freebsd";
#else
    return "unknown";
#endif
}

static inline const char *xr_os_core_arch(void) {
#if defined(XR_ARCH_ARM64) || defined(_M_ARM64)
    return "arm64";
#elif defined(XR_ARCH_X86_64) || defined(_M_X64)
    return "x64";
#elif defined(XR_ARCH_X86) || defined(_M_IX86)
    return "x86";
#elif defined(XR_ARCH_ARM) || defined(_M_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

static inline const char *xr_os_core_sep(void) {
#ifdef XR_OS_WINDOWS
    return "\\";
#else
    return "/";
#endif
}

static inline const char *xr_os_core_eol(void) {
#ifdef XR_OS_WINDOWS
    return "\r\n";
#else
    return "\n";
#endif
}

static inline bool xr_os_core_has_env_value(const char *value) {
    return value && value[0] != '\0';
}

static inline const char *xr_os_core_tmpdir(XrOsCoreGetenvFn getenv_fn, void *ctx) {
    if (getenv_fn) {
        const char *tmpdir = getenv_fn(ctx, "TMPDIR");
        if (xr_os_core_has_env_value(tmpdir))
            return tmpdir;

        tmpdir = getenv_fn(ctx, "TMP");
        if (xr_os_core_has_env_value(tmpdir))
            return tmpdir;

        tmpdir = getenv_fn(ctx, "TEMP");
        if (xr_os_core_has_env_value(tmpdir))
            return tmpdir;
    }

#ifdef XR_OS_WINDOWS
    return "C:\\Windows\\Temp";
#else
    return "/tmp";
#endif
}

#endif /* XRAY_SHARED_XR_OS_CORE_H */
