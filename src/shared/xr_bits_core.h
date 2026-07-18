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
 *   Single semantic source for exact-width integer bit intrinsics. Xi stores
 *   the receiver's XrNativeType in aux_int; the VM and constant folders call
 *   this core, while AOT emits the equivalent compiler-recognizable native
 *   expression.  Values use an int64 payload, including uint64 bit patterns.
 *
 *   Self-contained: depends only on <stdint.h> plus <intrin.h> on MSVC,
 *   so it stays includable from the freestanding AOT runtime.
 */

#ifndef XRAY_SHARED_XR_BITS_CORE_H
#define XRAY_SHARED_XR_BITS_CORE_H

#include <stdint.h>
#include "xr_native_type_core.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

#ifndef XR_HAS_BUILTIN
#if defined(__has_builtin)
#define XR_HAS_BUILTIN(x) __has_builtin(x)
#else
#define XR_HAS_BUILTIN(x) 0
#endif
#endif

/* AOT emits these macro intrinsics for exact-width rotates. Clang's rotate
 * builtins preserve the scalar rotate operation through optimization; spelling
 * a rotate as shifts plus OR can trigger unprofitable SLP vectorization on
 * targets whose SIMD ISA has no rotate instruction. The fallback remains
 * defined for C toolchains without rotate builtins and masks both counts so
 * zero and negative counts never cause an invalid-width shift. */
static inline uint32_t xr_bits_aot_rotate_input_u32(uint32_t value) {
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    /* AArch64 NEON has no lane-wise rotate. Without this zero-instruction
     * scalar fence, Clang can SLP-pack four independent uint32 rotates into
     * shift/shift/or vectors that are substantially slower than scalar ror.
     * Keep other targets free to use their native vector rotate cost model. */
    __asm__("" : "+r"(value));
#endif
    return value;
}

static inline uint8_t xr_bits_aot_rotl8(uint8_t value, int64_t count) {
#if XR_HAS_BUILTIN(__builtin_rotateleft8)
    return __builtin_rotateleft8(value, (uint8_t) count);
#else
    const uint64_t n = (uint64_t) count;
    return (uint8_t) ((value << (n & 7u)) | (value >> ((-n) & 7u)));
#endif
}

static inline uint8_t xr_bits_aot_rotr8(uint8_t value, int64_t count) {
#if XR_HAS_BUILTIN(__builtin_rotateright8)
    return __builtin_rotateright8(value, (uint8_t) count);
#else
    const uint64_t n = (uint64_t) count;
    return (uint8_t) ((value >> (n & 7u)) | (value << ((-n) & 7u)));
#endif
}

static inline uint16_t xr_bits_aot_rotl16(uint16_t value, int64_t count) {
#if XR_HAS_BUILTIN(__builtin_rotateleft16)
    return __builtin_rotateleft16(value, (uint16_t) count);
#else
    const uint64_t n = (uint64_t) count;
    return (uint16_t) ((value << (n & 15u)) | (value >> ((-n) & 15u)));
#endif
}

static inline uint16_t xr_bits_aot_rotr16(uint16_t value, int64_t count) {
#if XR_HAS_BUILTIN(__builtin_rotateright16)
    return __builtin_rotateright16(value, (uint16_t) count);
#else
    const uint64_t n = (uint64_t) count;
    return (uint16_t) ((value >> (n & 15u)) | (value << ((-n) & 15u)));
#endif
}

static inline uint32_t xr_bits_aot_rotl32(uint32_t value, int64_t count) {
    value = xr_bits_aot_rotate_input_u32(value);
#if XR_HAS_BUILTIN(__builtin_rotateleft32)
    return __builtin_rotateleft32(value, (uint32_t) count);
#else
    const uint64_t n = (uint64_t) count;
    return (value << (n & 31u)) | (value >> ((-n) & 31u));
#endif
}

static inline uint32_t xr_bits_aot_rotr32(uint32_t value, int64_t count) {
    value = xr_bits_aot_rotate_input_u32(value);
#if XR_HAS_BUILTIN(__builtin_rotateright32)
    return __builtin_rotateright32(value, (uint32_t) count);
#else
    const uint64_t n = (uint64_t) count;
    return (value >> (n & 31u)) | (value << ((-n) & 31u));
#endif
}

static inline uint64_t xr_bits_aot_rotl64(uint64_t value, int64_t count) {
#if XR_HAS_BUILTIN(__builtin_rotateleft64)
    return __builtin_rotateleft64(value, (uint64_t) count);
#else
    const uint64_t n = (uint64_t) count;
    return (value << (n & 63u)) | (value >> ((-n) & 63u));
#endif
}

static inline uint64_t xr_bits_aot_rotr64(uint64_t value, int64_t count) {
#if XR_HAS_BUILTIN(__builtin_rotateright64)
    return __builtin_rotateright64(value, (uint64_t) count);
#else
    const uint64_t n = (uint64_t) count;
    return (value >> (n & 63u)) | (value << ((-n) & 63u));
#endif
}

/* Keep generated C independent of compiler-specific builtin spellings while
 * evaluating both operands exactly once on every toolchain. */
#define XR_BITS_ROTL8(value, count) xr_bits_aot_rotl8((uint8_t) (value), (int64_t) (count))
#define XR_BITS_ROTR8(value, count) xr_bits_aot_rotr8((uint8_t) (value), (int64_t) (count))
#define XR_BITS_ROTL16(value, count) xr_bits_aot_rotl16((uint16_t) (value), (int64_t) (count))
#define XR_BITS_ROTR16(value, count) xr_bits_aot_rotr16((uint16_t) (value), (int64_t) (count))
#define XR_BITS_ROTL32(value, count) xr_bits_aot_rotl32((uint32_t) (value), (int64_t) (count))
#define XR_BITS_ROTR32(value, count) xr_bits_aot_rotr32((uint32_t) (value), (int64_t) (count))
#define XR_BITS_ROTL64(value, count) xr_bits_aot_rotl64((uint64_t) (value), (int64_t) (count))
#define XR_BITS_ROTR64(value, count) xr_bits_aot_rotr64((uint64_t) (value), (int64_t) (count))

static inline uint8_t xr_bits_exact_width(uint8_t native_type) {
    switch (native_type) {
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
            return 8;
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
            return 16;
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
            return 32;
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
            return (uint8_t) (sizeof(void *) * 8u);
        case XR_NATIVE_I64:
        case XR_NATIVE_U64:
        default:
            return 64;
    }
}

static inline int xr_bits_exact_is_signed(uint8_t native_type) {
    switch (native_type) {
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
        case XR_NATIVE_USIZE:
            return 0;
        default:
            return 1;
    }
}

static inline uint64_t xr_bits_exact_mask(uint8_t width) {
    return width >= 64 ? UINT64_MAX : (UINT64_C(1) << width) - UINT64_C(1);
}

static inline uint64_t xr_bits_exact_pattern(int64_t x, uint8_t native_type) {
    return (uint64_t) x & xr_bits_exact_mask(xr_bits_exact_width(native_type));
}

/* Restore the exact receiver type in the VM's int64 payload convention. */
static inline int64_t xr_bits_exact_restore(uint64_t bits, uint8_t native_type) {
    uint8_t width = xr_bits_exact_width(native_type);
    uint64_t mask = xr_bits_exact_mask(width);
    bits &= mask;
    if (xr_bits_exact_is_signed(native_type) && width < 64 &&
        (bits & (UINT64_C(1) << (width - 1u))) != 0)
        bits |= ~mask;
    return (int64_t) bits;
}

/* Number of set bits in the receiver's exact-width bit pattern. */
static inline int64_t xr_bits_exact_popcount(int64_t x, uint8_t native_type) {
    uint64_t u = xr_bits_exact_pattern(x, native_type);
#if defined(_MSC_VER) && !defined(__clang__)
    return (int64_t) __popcnt64((unsigned long long) u);
#else
    return (int64_t) __builtin_popcountll((unsigned long long) u);
#endif
}

/* Count leading zeros in the exact receiver width.  Zero returns that width. */
static inline int64_t xr_bits_exact_leading_zeros(int64_t x, uint8_t native_type) {
    uint8_t width = xr_bits_exact_width(native_type);
    uint64_t u = xr_bits_exact_pattern(x, native_type);
    if (u == 0)
        return (int64_t) width;
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long idx;
    _BitScanReverse64(&idx, u);
    return (int64_t) (width - 1u - (uint8_t) idx);
#else
    return (int64_t) (__builtin_clzll(u) - (64u - width));
#endif
}

/* Count trailing zeros in the exact receiver width.  Zero returns that width. */
static inline int64_t xr_bits_exact_trailing_zeros(int64_t x, uint8_t native_type) {
    uint8_t width = xr_bits_exact_width(native_type);
    uint64_t u = xr_bits_exact_pattern(x, native_type);
    if (u == 0)
        return (int64_t) width;
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long idx;
    _BitScanForward64(&idx, u);
    return (int64_t) idx;
#else
    return (int64_t) __builtin_ctzll(u);
#endif
}

/* Reverse byte order inside the exact receiver width. */
static inline int64_t xr_bits_exact_byteswap(int64_t x, uint8_t native_type) {
    uint8_t width = xr_bits_exact_width(native_type);
    uint64_t u = xr_bits_exact_pattern(x, native_type);
    uint64_t out = 0;
    for (uint8_t shift = 0; shift < width; shift = (uint8_t) (shift + 8u)) {
        out = (out << 8u) | ((u >> shift) & UINT64_C(0xff));
    }
    return xr_bits_exact_restore(out, native_type);
}

/* Rotate counts use Euclidean modulo W. W is always a power of two, so the
 * unsigned mask also gives the required result for negative counts. */
static inline int64_t xr_bits_exact_rotate_left(int64_t x, int64_t n, uint8_t native_type) {
    uint8_t width = xr_bits_exact_width(native_type);
    uint64_t mask = xr_bits_exact_mask(width);
    uint64_t u = (uint64_t) x & mask;
    unsigned r = (unsigned) ((uint64_t) n & (uint64_t) (width - 1u));
    uint64_t out = ((u << r) | (u >> ((width - r) & (width - 1u)))) & mask;
    return xr_bits_exact_restore(out, native_type);
}

static inline int64_t xr_bits_exact_rotate_right(int64_t x, int64_t n, uint8_t native_type) {
    uint8_t width = xr_bits_exact_width(native_type);
    uint64_t mask = xr_bits_exact_mask(width);
    uint64_t u = (uint64_t) x & mask;
    unsigned r = (unsigned) ((uint64_t) n & (uint64_t) (width - 1u));
    uint64_t out = ((u >> r) | (u << ((width - r) & (width - 1u)))) & mask;
    return xr_bits_exact_restore(out, native_type);
}

#endif  // XRAY_SHARED_XR_BITS_CORE_H
