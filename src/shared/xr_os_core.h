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
