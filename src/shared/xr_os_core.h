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
#include <stdint.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

typedef const char *(*XrOsCoreGetenvFn)(void *ctx, const char *name);
typedef const char *(*XrOsCoreStringFn)(void *ctx);
typedef bool (*XrOsCoreEnvEntryFn)(void *ctx, const char *key, size_t key_len, const char *value,
                                   size_t value_len);

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

static inline const char *xr_os_core_username(XrOsCoreStringFn system_username_fn, void *system_ctx,
                                              XrOsCoreGetenvFn getenv_fn, void *env_ctx) {
    if (system_username_fn) {
        const char *user = system_username_fn(system_ctx);
        if (xr_os_core_has_env_value(user))
            return user;
    }

    if (!getenv_fn)
        return NULL;

#ifdef XR_OS_WINDOWS
    const char *user = getenv_fn(env_ctx, "USERNAME");
    if (xr_os_core_has_env_value(user))
        return user;
#endif

    const char *user = getenv_fn(env_ctx, "USER");
    if (xr_os_core_has_env_value(user))
        return user;

    user = getenv_fn(env_ctx, "LOGNAME");
    if (xr_os_core_has_env_value(user))
        return user;

    return NULL;
}

static inline const char *xr_os_core_homedir(XrOsCoreGetenvFn getenv_fn, void *env_ctx,
                                             XrOsCoreStringFn system_homedir_fn, void *system_ctx) {
    if (getenv_fn) {
        const char *home = getenv_fn(env_ctx, "HOME");
        if (xr_os_core_has_env_value(home))
            return home;

#ifdef XR_OS_WINDOWS
        home = getenv_fn(env_ctx, "USERPROFILE");
        if (xr_os_core_has_env_value(home))
            return home;
#endif
    }

    if (system_homedir_fn) {
        const char *home = system_homedir_fn(system_ctx);
        if (xr_os_core_has_env_value(home))
            return home;
    }

    return NULL;
}

static inline bool xr_os_core_environ_entry(const char *entry, XrOsCoreEnvEntryFn fn, void *ctx) {
    if (!entry || !fn)
        return false;

    const char *eq = strchr(entry, '=');
    if (!eq || eq == entry)
        return false;

    const char *value = eq + 1;
    return fn(ctx, entry, (size_t) (eq - entry), value, strlen(value));
}

static inline int64_t xr_os_core_cpu_count(long raw_count) {
    return raw_count > 0 ? (int64_t) raw_count : 1;
}

static inline int64_t xr_os_core_memory_bytes(uint64_t units, uint64_t unit_size) {
    if (units == 0 || unit_size == 0)
        return 0;
    if (units > (uint64_t) INT64_MAX / unit_size)
        return INT64_MAX;
    return (int64_t) (units * unit_size);
}

static inline double xr_os_core_seconds_from_nsec(int64_t sec, int64_t nsec) {
    if (sec < 0)
        return 0.0;
    if (nsec < 0)
        nsec = 0;
    return (double) sec + (double) nsec / 1000000000.0;
}

static inline double xr_os_core_uptime_from_boot_seconds(int64_t now_sec, int64_t boot_sec) {
    if (now_sec <= boot_sec)
        return 0.0;
    return (double) (now_sec - boot_sec);
}

static inline void xr_os_core_loadavg_zero(double out[3]) {
    if (!out)
        return;
    out[0] = 0.0;
    out[1] = 0.0;
    out[2] = 0.0;
}

static inline double xr_os_core_loadavg_from_fixed(uint64_t fixed_load) {
    return (double) fixed_load / 65536.0;
}

static inline void xr_os_core_loadavg_set(double out[3], double one, double five, double fifteen) {
    if (!out)
        return;
    out[0] = one >= 0.0 ? one : 0.0;
    out[1] = five >= 0.0 ? five : 0.0;
    out[2] = fifteen >= 0.0 ? fifteen : 0.0;
}

#endif /* XRAY_SHARED_XR_OS_CORE_H */
