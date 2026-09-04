/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_os_core.h - Runtime-neutral OS stdlib core helpers.
 *
 * One owner for the parts of the OS surface that neither runtime gets to spell
 * differently: the platform/arch ladders and the pure arithmetic that
 * normalizes raw syscall output. Both the VM stdlib
 * (stdlib/os/os.c) and the freestanding AOT runtime (src/aot/xrt_os.h) call in
 * here, so everything stays header-only static inline and touches no allocator
 * and no value representation - the caller supplies both.
 *
 * Deliberately absent: anything the public Xray module already owns. Path
 * separator, line ending, homedir and username resolution, and NAME=VALUE
 * splitting are module policy and live in stdlib/os/os.xr; the C layer only
 * publishes the raw host facts they build on.
 */

#ifndef XRAY_SHARED_XR_OS_CORE_H
#define XRAY_SHARED_XR_OS_CORE_H

#include "../base/xplatform.h"
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

#ifdef XR_OS_WINDOWS
#include <process.h>
#else
#include <unistd.h>
#endif

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
#elif defined(XR_ARCH_POWERPC64)
    return "ppc64";
#elif defined(XR_ARCH_LOONGARCH64)
    return "loongarch64";
#elif defined(XR_ARCH_RISCV64)
    return "riscv64";
#else
    return "unknown";
#endif
}

static inline int64_t xr_os_core_getpid(void) {
#ifdef XR_OS_WINDOWS
    return (int64_t) _getpid();
#else
    return (int64_t) getpid();
#endif
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
