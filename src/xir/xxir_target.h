/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_target.h - XIR target admission and host driver construction
 */
#ifndef XXIR_TARGET_H
#define XXIR_TARGET_H

#include "xxir_value.h"
#include "../base/xplatform.h"
#include "../shared/xr_target_fact.h"

#if defined(XR_ARCH_LE64) && defined(XR_OS_WINDOWS)
#define XR_XIR_HOST_OS XR_TARGET_QUERY_VALUE_OS_WINDOWS
#elif defined(XR_ARCH_LE64) && defined(XR_OS_LINUX)
#define XR_XIR_HOST_OS XR_TARGET_QUERY_VALUE_OS_LINUX
#elif defined(XR_ARCH_LE64) && defined(XR_OS_MACOS)
#define XR_XIR_HOST_OS XR_TARGET_QUERY_VALUE_OS_MACOS
#else
#define XR_XIR_HOST_OS 0u
#endif

#if defined(XR_ARCH_LE64) && defined(XR_ARCH_X86_64)
#define XR_XIR_HOST_ARCHITECTURE XR_TARGET_QUERY_VALUE_ARCH_X86_64
#elif defined(XR_ARCH_LE64) && defined(XR_ARCH_ARM64)
#define XR_XIR_HOST_ARCHITECTURE XR_TARGET_QUERY_VALUE_ARCH_AARCH64
#elif defined(XR_ARCH_LE64) && defined(XR_ARCH_POWERPC64)
#define XR_XIR_HOST_ARCHITECTURE XR_TARGET_QUERY_VALUE_ARCH_POWERPC64
#elif defined(XR_ARCH_LE64) && defined(XR_ARCH_LOONGARCH64)
#define XR_XIR_HOST_ARCHITECTURE XR_TARGET_QUERY_VALUE_ARCH_LOONGARCH64
#else
#define XR_XIR_HOST_ARCHITECTURE 0u
#endif

#define XR_XIR_HOST_TARGET {XR_XIR_HOST_OS, XR_XIR_HOST_ARCHITECTURE, XR_XIR_VALUE_ABI_VERSION}

static inline XrXirTarget xr_xir_target_host(void) {
    XrXirTarget target = XR_XIR_HOST_TARGET;
    return target;
}

static inline bool xr_xir_target_valid(const XrXirTarget *target) {
    return target && target->abi_version == XR_XIR_VALUE_ABI_VERSION &&
        xr_target_os_machine_valid(target->os, target->architecture);
}

static inline bool xr_xir_target_equal(const XrXirTarget *a, const XrXirTarget *b) {
    return a->os == b->os && a->architecture == b->architecture && a->abi_version == b->abi_version;
}

static inline bool xr_xir_target_is_host(const XrXirTarget *target) {
    const XrXirTarget host = xr_xir_target_host();
    return xr_xir_target_valid(target) && xr_xir_target_equal(&host, target);
}

/* Raw XIR with no Source provenance can be checked independent of a target. */
static inline bool xr_xir_target_unbound(const XrXirTarget *target) {
    return !target->os && !target->architecture && !target->abi_version;
}
#endif // XXIR_TARGET_H
