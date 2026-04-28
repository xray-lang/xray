/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xplatform.h - Centralized platform detection and CPU primitives
 *
 * KEY CONCEPT:
 *   Single source of truth for OS, architecture, and compiler detection.
 *   All platform-specific #ifdef should reference these macros.
 */

#ifndef XPLATFORM_H
#define XPLATFORM_H

/* ========== OS Detection ========== */

#if defined(_WIN32) || defined(_WIN64)
#define XR_OS_WINDOWS 1
#elif defined(__linux__)
#define XR_OS_LINUX 1
#elif defined(__APPLE__)
#define XR_OS_MACOS 1
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define XR_OS_BSD 1
#endif

#if defined(XR_OS_LINUX) || defined(XR_OS_MACOS) || defined(XR_OS_BSD)
#define XR_OS_POSIX 1
#endif

/* ========== Architecture Detection ========== */

#if defined(__x86_64__) || defined(_M_X64)
#define XR_ARCH_X86_64 1
#elif defined(__i386__) || defined(_M_IX86)
#define XR_ARCH_X86 1
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
#define XR_ARCH_ARM64 1
#elif defined(__arm__) || defined(_M_ARM)
#define XR_ARCH_ARM 1
#elif defined(__riscv) && (__riscv_xlen == 64)
#define XR_ARCH_RISCV64 1
#endif  // ========== Compiler Detection ==========

#if defined(__clang__)
#define XR_COMPILER_CLANG 1
#elif defined(__GNUC__)
#define XR_COMPILER_GCC 1
#elif defined(_MSC_VER)
#define XR_COMPILER_MSVC 1
#endif  // ========== CPU Primitives ==========

// Pause/yield hint for spin loops. MSVC has no inline-asm syntax
// for x64 / ARM64 and uses intrinsics instead.
#if defined(XR_COMPILER_MSVC)
#include <intrin.h>
#if defined(XR_ARCH_X86_64) || defined(XR_ARCH_X86)
#define XR_CPU_PAUSE() _mm_pause()
#elif defined(XR_ARCH_ARM64)
#define XR_CPU_PAUSE() __yield()
#else
#define XR_CPU_PAUSE() ((void) 0)
#endif
#elif defined(XR_ARCH_X86_64) || defined(XR_ARCH_X86)
#define XR_CPU_PAUSE() __asm__ __volatile__("pause" ::: "memory")
#elif defined(XR_ARCH_ARM64)
#define XR_CPU_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
#define XR_CPU_PAUSE() ((void) 0)
#endif  // Cache line size (used for padding to avoid false sharing)
#define XR_CACHE_LINE 64

/* ========== MSVC GCC-builtin Compatibility ========== */

#if defined(XR_COMPILER_MSVC) && !defined(__clang__)
#include <stdlib.h>

// Branch prediction hints (no-op on MSVC; the optimizer handles it)
#define __builtin_expect(expr, val) (expr)

// Bit manipulation intrinsics
#include <intrin.h>
static __forceinline int __builtin_ctz(unsigned int x) {
    unsigned long idx;
    _BitScanForward(&idx, x);
    return (int) idx;
}
static __forceinline int __builtin_ctzll(unsigned long long x) {
    unsigned long idx;
    _BitScanForward64(&idx, x);
    return (int) idx;
}
static __forceinline int __builtin_popcountll(unsigned long long x) {
    return (int) __popcnt64(x);
}

// Overflow-checked arithmetic (C23 ckd_* style; MSVC lacks builtins)
#include <stdint.h>
#include <limits.h>
static __forceinline int __builtin_add_overflow(int64_t a, int64_t b, int64_t *res) {
    *res = a + b;
    return ((b > 0) && (a > INT64_MAX - b)) || ((b < 0) && (a < INT64_MIN - b));
}
static __forceinline int __builtin_sub_overflow(int64_t a, int64_t b, int64_t *res) {
    *res = a - b;
    return ((b < 0) && (a > INT64_MAX + b)) || ((b > 0) && (a < INT64_MIN + b));
}
static __forceinline int __builtin_mul_overflow(int64_t a, int64_t b, int64_t *res) {
    *res = a * b;
    if (a == 0 || b == 0)
        return 0;
    return (*res / a != b);
}
#endif  // XR_COMPILER_MSVC builtins

/* ========== MSVC POSIX Function Shims ========== */

#if defined(XR_COMPILER_MSVC)
#include <string.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strtok_r strtok_s
#endif

#endif  // XPLATFORM_H
