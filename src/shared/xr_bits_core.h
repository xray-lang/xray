/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_bits_core.h - Runtime-neutral bit-manipulation intrinsics.
 *
 * KEY CONCEPT:
 *   Single semantic source for the mem.* bit intrinsics. Both the VM
 *   binding (stdlib/mem/mem.c) and the AOT freestanding wrapper
 *   (src/aot/xrt_mem.h) call these, so VM and AOT produce identical
 *   results by construction. All operate on the 64-bit `int` domain.
 *
 *   Self-contained: depends only on <stdint.h> plus <intrin.h> on MSVC,
 *   so it stays includable from the freestanding AOT runtime.
 */

#ifndef XRAY_SHARED_XR_BITS_CORE_H
#define XRAY_SHARED_XR_BITS_CORE_H

#include <stdint.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

/* Number of set bits in the 64-bit two's-complement representation. */
static inline int64_t xr_bits_core_popcount(int64_t x) {
#if defined(_MSC_VER) && !defined(__clang__)
    return (int64_t) __popcnt64((unsigned long long) x);
#else
    return (int64_t) __builtin_popcountll((unsigned long long) x);
#endif
}

/* Count of leading zero bits (from the most-significant end). 0 -> 64. */
static inline int64_t xr_bits_core_leading_zeros(int64_t x) {
    uint64_t u = (uint64_t) x;
    if (u == 0)
        return 64;
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long idx;
    _BitScanReverse64(&idx, u);
    return (int64_t) (63 - idx);
#else
    return (int64_t) __builtin_clzll(u);
#endif
}

/* Count of trailing zero bits (from the least-significant end). 0 -> 64. */
static inline int64_t xr_bits_core_trailing_zeros(int64_t x) {
    uint64_t u = (uint64_t) x;
    if (u == 0)
        return 64;
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long idx;
    _BitScanForward64(&idx, u);
    return (int64_t) idx;
#else
    return (int64_t) __builtin_ctzll(u);
#endif
}

/* Reverse the byte order of the 64-bit value (endianness swap). */
static inline int64_t xr_bits_core_byteswap(int64_t x) {
#if defined(_MSC_VER) && !defined(__clang__)
    return (int64_t) _byteswap_uint64((unsigned long long) x);
#else
    return (int64_t) __builtin_bswap64((uint64_t) x);
#endif
}

/* Rotate the 64-bit value left by `n` bits (n taken modulo 64). */
static inline int64_t xr_bits_core_rotate_left(int64_t x, int64_t n) {
    uint64_t u = (uint64_t) x;
    unsigned r = (unsigned) (((uint64_t) n) & 63u);
    return (int64_t) ((u << r) | (u >> ((64u - r) & 63u)));
}

/* Rotate the 64-bit value right by `n` bits (n taken modulo 64). */
static inline int64_t xr_bits_core_rotate_right(int64_t x, int64_t n) {
    uint64_t u = (uint64_t) x;
    unsigned r = (unsigned) (((uint64_t) n) & 63u);
    return (int64_t) ((u >> r) | (u << ((64u - r) & 63u)));
}

#endif  // XRAY_SHARED_XR_BITS_CORE_H
