/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_raw_scalar_core.h - Runtime-neutral unchecked scalar memory access.
 *
 * This core is intentionally separate from xr_array_core.h. Every pointer is
 * backed by an unsafe caller proof: there is no null check, length, range,
 * error channel, or checked fallback. Fixed-size builtin memcpy keeps the
 * access unaligned-safe and strict-alias-safe while optimizing to native
 * scalar loads/stores.
 */

#ifndef XR_RAW_SCALAR_CORE_H
#define XR_RAW_SCALAR_CORE_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define XR_RAW_COPY(dst, src, size) __builtin_memcpy((dst), (src), (size))
#define XR_RAW_ASSUME_NON_NULL(ptr)                                                                \
    do {                                                                                           \
        if ((ptr) == NULL)                                                                         \
            __builtin_unreachable();                                                               \
    } while (0)
#elif defined(_MSC_VER)
#include <string.h>
#define XR_RAW_COPY(dst, src, size) memcpy((dst), (src), (size))
#define XR_RAW_ASSUME_NON_NULL(ptr) __assume((ptr) != NULL)
#else
#include <string.h>
#define XR_RAW_COPY(dst, src, size) memcpy((dst), (src), (size))
#define XR_RAW_ASSUME_NON_NULL(ptr) ((void) (ptr))
#endif

enum {
    XR_RAW_ENDIAN_NATIVE = 0,
    XR_RAW_ENDIAN_LE = 1,
    XR_RAW_ENDIAN_BE = 2,
};

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) &&                                 \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define XR_RAW_HOST_ENDIAN_KNOWN 1
#define XR_RAW_HOST_IS_LE 1
#elif defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) &&                                  \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define XR_RAW_HOST_ENDIAN_KNOWN 1
#define XR_RAW_HOST_IS_LE 0
#else
#define XR_RAW_HOST_ENDIAN_KNOWN 0
#endif

static inline int xr_raw_host_is_little_endian(void) {
#if XR_RAW_HOST_ENDIAN_KNOWN
    return XR_RAW_HOST_IS_LE;
#else
    const uint16_t one = 1;
    return *((const uint8_t *) &one) == 1;
#endif
}

static inline int xr_raw_endian_matches_host(int64_t endian) {
    if (endian == XR_RAW_ENDIAN_NATIVE)
        return 1;
#if XR_RAW_HOST_ENDIAN_KNOWN
    return endian == (XR_RAW_HOST_IS_LE ? XR_RAW_ENDIAN_LE : XR_RAW_ENDIAN_BE);
#else
    return (endian == XR_RAW_ENDIAN_LE) == xr_raw_host_is_little_endian();
#endif
}

static inline uint16_t xr_raw_bswap_u16(uint16_t value) {
    return (uint16_t) ((value >> 8u) | (value << 8u));
}

static inline uint32_t xr_raw_bswap_u32(uint32_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap32(value);
#else
    return ((value & UINT32_C(0x000000ff)) << 24u) | ((value & UINT32_C(0x0000ff00)) << 8u) |
           ((value & UINT32_C(0x00ff0000)) >> 8u) | ((value & UINT32_C(0xff000000)) >> 24u);
#endif
}

static inline uint64_t xr_raw_bswap_u64(uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(value);
#else
    return ((value & UINT64_C(0x00000000000000ff)) << 56u) |
           ((value & UINT64_C(0x000000000000ff00)) << 40u) |
           ((value & UINT64_C(0x0000000000ff0000)) << 24u) |
           ((value & UINT64_C(0x00000000ff000000)) << 8u) |
           ((value & UINT64_C(0x000000ff00000000)) >> 8u) |
           ((value & UINT64_C(0x0000ff0000000000)) >> 24u) |
           ((value & UINT64_C(0x00ff000000000000)) >> 40u) |
           ((value & UINT64_C(0xff00000000000000)) >> 56u);
#endif
}

#define XR_RAW_ENDIAN_CONVERSIONS(width, type, bswap)                                              \
    static inline type xr_raw_u##width##_from_le(type value) {                                     \
        return xr_raw_host_is_little_endian() ? value : bswap(value);                              \
    }                                                                                              \
    static inline type xr_raw_u##width##_from_be(type value) {                                     \
        return xr_raw_host_is_little_endian() ? bswap(value) : value;                              \
    }                                                                                              \
    static inline type xr_raw_u##width##_from_endian(type value, int64_t endian) {                 \
        return xr_raw_endian_matches_host(endian) ? value : bswap(value);                          \
    }

XR_RAW_ENDIAN_CONVERSIONS(16, uint16_t, xr_raw_bswap_u16)
XR_RAW_ENDIAN_CONVERSIONS(32, uint32_t, xr_raw_bswap_u32)
XR_RAW_ENDIAN_CONVERSIONS(64, uint64_t, xr_raw_bswap_u64)

#undef XR_RAW_ENDIAN_CONVERSIONS

static inline uint8_t xr_raw_load_u8_unaligned(const void *ptr) {
    uint8_t value;
    XR_RAW_ASSUME_NON_NULL(ptr);
    XR_RAW_COPY(&value, ptr, sizeof(value));
    return value;
}

static inline uint16_t xr_raw_load_u16_unaligned(const void *ptr) {
    uint16_t value;
    XR_RAW_ASSUME_NON_NULL(ptr);
    XR_RAW_COPY(&value, ptr, sizeof(value));
    return value;
}

static inline uint32_t xr_raw_load_u32_unaligned(const void *ptr) {
    uint32_t value;
    XR_RAW_ASSUME_NON_NULL(ptr);
    XR_RAW_COPY(&value, ptr, sizeof(value));
    return value;
}

static inline uint64_t xr_raw_load_u64_unaligned(const void *ptr) {
    uint64_t value;
    XR_RAW_ASSUME_NON_NULL(ptr);
    XR_RAW_COPY(&value, ptr, sizeof(value));
    return value;
}

static inline void xr_raw_store_u8_unaligned(void *ptr, uint8_t value) {
    XR_RAW_ASSUME_NON_NULL(ptr);
    XR_RAW_COPY(ptr, &value, sizeof(value));
}

static inline void xr_raw_store_u16_unaligned(void *ptr, uint16_t value) {
    XR_RAW_ASSUME_NON_NULL(ptr);
    XR_RAW_COPY(ptr, &value, sizeof(value));
}

static inline void xr_raw_store_u32_unaligned(void *ptr, uint32_t value) {
    XR_RAW_ASSUME_NON_NULL(ptr);
    XR_RAW_COPY(ptr, &value, sizeof(value));
}

static inline void xr_raw_store_u64_unaligned(void *ptr, uint64_t value) {
    XR_RAW_ASSUME_NON_NULL(ptr);
    XR_RAW_COPY(ptr, &value, sizeof(value));
}

static inline float xr_raw_f32_from_bits(uint32_t bits) {
    float value;
    XR_RAW_COPY(&value, &bits, sizeof(value));
    return value;
}

static inline double xr_raw_f64_from_bits(uint64_t bits) {
    double value;
    XR_RAW_COPY(&value, &bits, sizeof(value));
    return value;
}

static inline uint32_t xr_raw_f32_to_bits(float value) {
    uint32_t bits;
    XR_RAW_COPY(&bits, &value, sizeof(bits));
    return bits;
}

static inline uint64_t xr_raw_f64_to_bits(double value) {
    uint64_t bits;
    XR_RAW_COPY(&bits, &value, sizeof(bits));
    return bits;
}

static inline void *xr_raw_load_ptr_unaligned(const void *ptr) {
    void *value;
    XR_RAW_ASSUME_NON_NULL(ptr);
    XR_RAW_COPY(&value, ptr, sizeof(value));
    return value;
}

static inline void xr_raw_store_ptr_unaligned(void *ptr, const void *value) {
    XR_RAW_ASSUME_NON_NULL(ptr);
    XR_RAW_COPY(ptr, &value, sizeof(value));
}

#undef XR_RAW_COPY
#undef XR_RAW_ASSUME_NON_NULL

#endif /* XR_RAW_SCALAR_CORE_H */
