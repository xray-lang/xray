/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_byte_slice_scalar_core.h - Byte-slice scalar I/O semantic owner.
 */

#ifndef XR_BYTE_SLICE_SCALAR_CORE_H
#define XR_BYTE_SLICE_SCALAR_CORE_H

#if !defined(XR_BYTE_SLICE_SCALAR_C90)
#include "xr_elem_type.h"
#include "xr_semantic_owner_ids_gen.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define XR_BYTE_SLICE_SCALAR_INLINE static inline
#else
/* The restricted C90 runtime provides the fixed-width types, bool, size_t,
 * XR_ELEM_U8, and endian constants before including this semantic core. */
#define XR_BYTE_SLICE_SCALAR_INLINE static
#endif

#if !defined(XR_BYTE_SLICE_SCALAR_C90)
typedef enum XrEndianCore {
    XR_ENDIAN_NATIVE = 0,
    XR_ENDIAN_LE = 1,
    XR_ENDIAN_BE = 2,
} XrEndianCore;
#endif

#ifndef XRT_TARGET_NATIVE_ENDIAN
#if defined(XR_AOT_TARGET_LITTLE_ENDIAN)
#if XR_AOT_TARGET_LITTLE_ENDIAN
#define XRT_TARGET_NATIVE_ENDIAN XR_ENDIAN_LE
#else
#define XRT_TARGET_NATIVE_ENDIAN XR_ENDIAN_BE
#endif
#else
#define XRT_TARGET_NATIVE_ENDIAN XR_ENDIAN_NATIVE
#endif
#endif

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) &&                                 \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define XR_ARRAY_CORE_HOST_ENDIAN_KNOWN 1
#define XR_ARRAY_CORE_HOST_IS_LE 1
#elif defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) &&                                  \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define XR_ARRAY_CORE_HOST_ENDIAN_KNOWN 1
#define XR_ARRAY_CORE_HOST_IS_LE 0
#else
#define XR_ARRAY_CORE_HOST_ENDIAN_KNOWN 0
#endif

#define XR_BYTE_SLICE_SCALAR_OWNER_GUARD(owner_hi, owner_lo)                                      \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_byte_slice_scalar                                   \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_HI &&          \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_LO)             \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_BYTE_SLICE_SCALAR_CONSUMER_GUARD(consumer_bit)                                         \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_byte_slice_scalar                      \
            : (((uint32_t) (consumer_bit) != 0 &&                                                 \
                (XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_CONSUMERS &                             \
                 (uint32_t) (consumer_bit)) != 0)                                                 \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_BYTE_SLICE_SCALAR_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, expression)            \
    (XR_BYTE_SLICE_SCALAR_OWNER_GUARD((owner_hi), (owner_lo)),                                    \
     XR_BYTE_SLICE_SCALAR_CONSUMER_GUARD((consumer_bit)), (expression))

#if !defined(XR_BYTE_SLICE_SCALAR_C90)
#define XR_BYTE_SLICE_COMPARE_OWNER_GUARD(owner_hi, owner_lo)                                     \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_byte_slice_compare                                  \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMPARE_HI &&         \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMPARE_LO)            \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_BYTE_SLICE_COMPARE_CONSUMER_GUARD(consumer_bit)                                        \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_byte_slice_compare                     \
            : (((uint32_t) (consumer_bit) != 0 &&                                                 \
                (XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMPARE_CONSUMERS &                            \
                 (uint32_t) (consumer_bit)) != 0)                                                 \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_BYTE_SLICE_COMPARE_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, expression)           \
    (XR_BYTE_SLICE_COMPARE_OWNER_GUARD((owner_hi), (owner_lo)),                                   \
     XR_BYTE_SLICE_COMPARE_CONSUMER_GUARD((consumer_bit)), (expression))

#define XR_BYTE_SLICE_COMMON_PREFIX_OWNER_GUARD(owner_hi, owner_lo)                               \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_byte_slice_common_prefix                            \
            : (((uint64_t) (owner_hi) ==                                                         \
                XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMMON_PREFIX_HI &&                            \
                (uint64_t) (owner_lo) ==                                                         \
                    XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMMON_PREFIX_LO)                           \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_BYTE_SLICE_COMMON_PREFIX_CONSUMER_GUARD(consumer_bit)                                  \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_byte_slice_common_prefix               \
            : (((uint32_t) (consumer_bit) != 0 &&                                                 \
                (XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMMON_PREFIX_CONSUMERS &                      \
                 (uint32_t) (consumer_bit)) != 0)                                                 \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_BYTE_SLICE_COMMON_PREFIX_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, expression)     \
    (XR_BYTE_SLICE_COMMON_PREFIX_OWNER_GUARD((owner_hi), (owner_lo)),                             \
     XR_BYTE_SLICE_COMMON_PREFIX_CONSUMER_GUARD((consumer_bit)), (expression))
#endif

XR_BYTE_SLICE_SCALAR_INLINE int64_t xr_byte_slice_compare_core(
    const void *left_data, int64_t left_length, uint8_t left_elem_type, const void *right_data,
    int64_t right_length, uint8_t right_elem_type, bool *ok) {
    bool valid = left_length >= 0 && right_length >= 0 && left_elem_type == XR_ELEM_U8 &&
                 right_elem_type == XR_ELEM_U8 && (left_length == 0 || left_data) &&
                 (right_length == 0 || right_data);
    int64_t shared_length = 0;
    int ordering = 0;
    if (ok)
        *ok = valid;
    if (!valid)
        return 0;

    shared_length = left_length < right_length ? left_length : right_length;
    ordering = shared_length > 0 ? memcmp(left_data, right_data, (size_t) shared_length) : 0;
    if (ordering < 0)
        return -1;
    if (ordering > 0)
        return 1;
    if (left_length < right_length)
        return -1;
    if (left_length > right_length)
        return 1;
    return 0;
}

XR_BYTE_SLICE_SCALAR_INLINE int xr_byte_slice_common_prefix_diff_byte64(uint64_t diff) {
#if defined(__GNUC__) || defined(__clang__)
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) &&                                    \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_clzll(diff) >> 3;
#else
    return __builtin_ctzll(diff) >> 3;
#endif
#else
    int n = 0;
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) &&                                    \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    while (((diff >> ((7 - n) * 8)) & UINT64_C(0xff)) == 0)
        n++;
#else
    while (((diff >> (n * 8)) & UINT64_C(0xff)) == 0)
        n++;
#endif
    return n;
#endif
}

XR_BYTE_SLICE_SCALAR_INLINE int64_t xr_byte_slice_common_prefix_core(
    const void *left_data, int64_t left_length, uint8_t left_elem_type, const void *right_data,
    int64_t right_length, uint8_t right_elem_type, bool *ok) {
    bool valid = left_length >= 0 && right_length >= 0 && left_elem_type == XR_ELEM_U8 &&
                 right_elem_type == XR_ELEM_U8;
    int64_t shared_length = 0;
    int64_t i = 0;
    const uint8_t *left = (const uint8_t *) left_data;
    const uint8_t *right = (const uint8_t *) right_data;
    if (!valid)
        goto invalid;

    shared_length = left_length < right_length ? left_length : right_length;
    if (shared_length > 0 && (!left_data || !right_data))
        goto invalid;
    if (ok)
        *ok = true;
    while (i + 8 <= shared_length) {
        uint64_t left_word = 0;
        uint64_t right_word = 0;
        uint64_t diff = 0;
        memcpy(&left_word, left + i, sizeof(left_word));
        memcpy(&right_word, right + i, sizeof(right_word));
        diff = left_word ^ right_word;
        if (diff)
            return i + xr_byte_slice_common_prefix_diff_byte64(diff);
        i += 8;
    }
    while (i < shared_length && left[i] == right[i])
        i++;
    return i;

invalid:
    if (ok)
        *ok = false;
    return 0;
}

XR_BYTE_SLICE_SCALAR_INLINE bool xr_array_core_bytes_range_ok(int64_t length, uint8_t elem_type,
                                                              int64_t offset, int64_t width) {
    return elem_type == XR_ELEM_U8 && length >= 0 && offset >= 0 && width >= 0 &&
           offset <= length && width <= length - offset;
}

XR_BYTE_SLICE_SCALAR_INLINE bool xr_array_core_host_is_little_endian(void) {
    const uint16_t one = 1;
    return *((const uint8_t *) &one) == 1;
}

XR_BYTE_SLICE_SCALAR_INLINE bool xr_array_core_effective_little_endian(int64_t endian) {
    if (endian == XR_ENDIAN_BE)
        return false;
    if (endian == XR_ENDIAN_LE)
        return true;
    return xr_array_core_host_is_little_endian();
}

XR_BYTE_SLICE_SCALAR_INLINE bool xr_array_core_endian_matches_host(int64_t endian) {
    if (endian == XR_ENDIAN_NATIVE)
        return true;
#if XR_ARRAY_CORE_HOST_ENDIAN_KNOWN
    return (endian == XR_ENDIAN_LE) == (XR_ARRAY_CORE_HOST_IS_LE != 0);
#else
    return xr_array_core_effective_little_endian(endian) == xr_array_core_host_is_little_endian();
#endif
}

XR_BYTE_SLICE_SCALAR_INLINE uint16_t xr_array_core_bswap16(uint16_t value) {
    return (uint16_t) ((value >> 8) | (value << 8));
}

XR_BYTE_SLICE_SCALAR_INLINE uint32_t xr_array_core_bswap32(uint32_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap32(value);
#else
    return ((value & UINT32_C(0x000000ff)) << 24) | ((value & UINT32_C(0x0000ff00)) << 8) |
           ((value & UINT32_C(0x00ff0000)) >> 8) | ((value & UINT32_C(0xff000000)) >> 24);
#endif
}

XR_BYTE_SLICE_SCALAR_INLINE uint64_t xr_array_core_bswap64(uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(value);
#else
    return ((value & UINT64_C(0x00000000000000ff)) << 56) |
           ((value & UINT64_C(0x000000000000ff00)) << 40) |
           ((value & UINT64_C(0x0000000000ff0000)) << 24) |
           ((value & UINT64_C(0x00000000ff000000)) << 8) |
           ((value & UINT64_C(0x000000ff00000000)) >> 8) |
           ((value & UINT64_C(0x0000ff0000000000)) >> 24) |
           ((value & UINT64_C(0x00ff000000000000)) >> 40) |
           ((value & UINT64_C(0xff00000000000000)) >> 56);
#endif
}

XR_BYTE_SLICE_SCALAR_INLINE void xr_byte_slice_scalar_copy(void *destination, const void *source,
                                                           size_t byte_count) {
    uint8_t *dst = (uint8_t *) destination;
    const uint8_t *src = (const uint8_t *) source;
    size_t index;
    for (index = 0; index < byte_count; index++)
        dst[index] = src[index];
}

XR_BYTE_SLICE_SCALAR_INLINE uint16_t
xr_byte_slice_scalar_load_u16_unchecked(const void *data, int64_t offset, int64_t endian) {
    uint16_t value = 0;
    xr_byte_slice_scalar_copy(&value, (const uint8_t *) data + offset, sizeof(value));
    return xr_array_core_endian_matches_host(endian) ? value : xr_array_core_bswap16(value);
}

XR_BYTE_SLICE_SCALAR_INLINE uint32_t
xr_byte_slice_scalar_load_u32_unchecked(const void *data, int64_t offset, int64_t endian) {
    uint32_t value = 0;
    xr_byte_slice_scalar_copy(&value, (const uint8_t *) data + offset, sizeof(value));
    return xr_array_core_endian_matches_host(endian) ? value : xr_array_core_bswap32(value);
}

XR_BYTE_SLICE_SCALAR_INLINE uint64_t
xr_byte_slice_scalar_load_u64_unchecked(const void *data, int64_t offset, int64_t endian) {
    uint64_t value = 0;
    xr_byte_slice_scalar_copy(&value, (const uint8_t *) data + offset, sizeof(value));
    return xr_array_core_endian_matches_host(endian) ? value : xr_array_core_bswap64(value);
}

XR_BYTE_SLICE_SCALAR_INLINE void xr_byte_slice_scalar_store_u16_unchecked(
    void *data, int64_t offset, uint16_t value, int64_t endian) {
    if (!xr_array_core_endian_matches_host(endian))
        value = xr_array_core_bswap16(value);
    xr_byte_slice_scalar_copy((uint8_t *) data + offset, &value, sizeof(value));
}

XR_BYTE_SLICE_SCALAR_INLINE void xr_byte_slice_scalar_store_u32_unchecked(
    void *data, int64_t offset, uint32_t value, int64_t endian) {
    if (!xr_array_core_endian_matches_host(endian))
        value = xr_array_core_bswap32(value);
    xr_byte_slice_scalar_copy((uint8_t *) data + offset, &value, sizeof(value));
}

XR_BYTE_SLICE_SCALAR_INLINE void xr_byte_slice_scalar_store_u64_unchecked(
    void *data, int64_t offset, uint64_t value, int64_t endian) {
    if (!xr_array_core_endian_matches_host(endian))
        value = xr_array_core_bswap64(value);
    xr_byte_slice_scalar_copy((uint8_t *) data + offset, &value, sizeof(value));
}

XR_BYTE_SLICE_SCALAR_INLINE uint16_t xr_array_core_bytes_load_u16(
    const void *data, int64_t length, uint8_t elem_type, int64_t offset, int64_t endian, bool *ok) {
    bool valid = data && xr_array_core_bytes_range_ok(length, elem_type, offset, 2);
    if (ok)
        *ok = valid;
    return valid ? xr_byte_slice_scalar_load_u16_unchecked(data, offset, endian) : 0;
}

XR_BYTE_SLICE_SCALAR_INLINE uint32_t xr_array_core_bytes_load_u32(
    const void *data, int64_t length, uint8_t elem_type, int64_t offset, int64_t endian, bool *ok) {
    bool valid = data && xr_array_core_bytes_range_ok(length, elem_type, offset, 4);
    if (ok)
        *ok = valid;
    return valid ? xr_byte_slice_scalar_load_u32_unchecked(data, offset, endian) : 0;
}

XR_BYTE_SLICE_SCALAR_INLINE uint64_t xr_array_core_bytes_load_u64(
    const void *data, int64_t length, uint8_t elem_type, int64_t offset, int64_t endian, bool *ok) {
    bool valid = data && xr_array_core_bytes_range_ok(length, elem_type, offset, 8);
    if (ok)
        *ok = valid;
    return valid ? xr_byte_slice_scalar_load_u64_unchecked(data, offset, endian) : 0;
}

XR_BYTE_SLICE_SCALAR_INLINE float xr_array_core_f32_from_bits(uint32_t bits) {
    float value = 0.0f;
    xr_byte_slice_scalar_copy(&value, &bits, sizeof(value));
    return value;
}

XR_BYTE_SLICE_SCALAR_INLINE double xr_array_core_f64_from_bits(uint64_t bits) {
    double value = 0.0;
    xr_byte_slice_scalar_copy(&value, &bits, sizeof(value));
    return value;
}

XR_BYTE_SLICE_SCALAR_INLINE uint32_t xr_array_core_f32_to_bits(float value) {
    uint32_t bits = 0;
    xr_byte_slice_scalar_copy(&bits, &value, sizeof(bits));
    return bits;
}

XR_BYTE_SLICE_SCALAR_INLINE uint64_t xr_array_core_f64_to_bits(double value) {
    uint64_t bits = 0;
    xr_byte_slice_scalar_copy(&bits, &value, sizeof(bits));
    return bits;
}

XR_BYTE_SLICE_SCALAR_INLINE float xr_array_core_bytes_load_f32(
    const void *data, int64_t length, uint8_t elem_type, int64_t offset, int64_t endian, bool *ok) {
    return xr_array_core_f32_from_bits(
        xr_array_core_bytes_load_u32(data, length, elem_type, offset, endian, ok));
}

XR_BYTE_SLICE_SCALAR_INLINE double xr_array_core_bytes_load_f64(
    const void *data, int64_t length, uint8_t elem_type, int64_t offset, int64_t endian, bool *ok) {
    return xr_array_core_f64_from_bits(
        xr_array_core_bytes_load_u64(data, length, elem_type, offset, endian, ok));
}

XR_BYTE_SLICE_SCALAR_INLINE bool xr_array_core_bytes_store_u16(
    void *data, int64_t length, uint8_t elem_type, int64_t offset, uint16_t value, int64_t endian) {
    if (!data || !xr_array_core_bytes_range_ok(length, elem_type, offset, 2))
        return false;
    xr_byte_slice_scalar_store_u16_unchecked(data, offset, value, endian);
    return true;
}

XR_BYTE_SLICE_SCALAR_INLINE bool xr_array_core_bytes_store_u32(
    void *data, int64_t length, uint8_t elem_type, int64_t offset, uint32_t value, int64_t endian) {
    if (!data || !xr_array_core_bytes_range_ok(length, elem_type, offset, 4))
        return false;
    xr_byte_slice_scalar_store_u32_unchecked(data, offset, value, endian);
    return true;
}

XR_BYTE_SLICE_SCALAR_INLINE bool xr_array_core_bytes_store_u64(
    void *data, int64_t length, uint8_t elem_type, int64_t offset, uint64_t value, int64_t endian) {
    if (!data || !xr_array_core_bytes_range_ok(length, elem_type, offset, 8))
        return false;
    xr_byte_slice_scalar_store_u64_unchecked(data, offset, value, endian);
    return true;
}

XR_BYTE_SLICE_SCALAR_INLINE bool xr_array_core_bytes_store_f32(
    void *data, int64_t length, uint8_t elem_type, int64_t offset, float value, int64_t endian) {
    return xr_array_core_bytes_store_u32(data, length, elem_type, offset,
                                         xr_array_core_f32_to_bits(value), endian);
}

XR_BYTE_SLICE_SCALAR_INLINE bool xr_array_core_bytes_store_f64(
    void *data, int64_t length, uint8_t elem_type, int64_t offset, double value, int64_t endian) {
    return xr_array_core_bytes_store_u64(data, length, elem_type, offset,
                                         xr_array_core_f64_to_bits(value), endian);
}

XR_BYTE_SLICE_SCALAR_INLINE uint16_t xr_array_core_bytes_load_u16_le(
    const void *data, int64_t length, uint8_t elem_type, int64_t offset, bool *ok) {
    return xr_array_core_bytes_load_u16(data, length, elem_type, offset, XR_ENDIAN_LE, ok);
}

XR_BYTE_SLICE_SCALAR_INLINE uint32_t xr_array_core_bytes_load_u32_le(
    const void *data, int64_t length, uint8_t elem_type, int64_t offset, bool *ok) {
    return xr_array_core_bytes_load_u32(data, length, elem_type, offset, XR_ENDIAN_LE, ok);
}

XR_BYTE_SLICE_SCALAR_INLINE uint64_t xr_array_core_bytes_load_u64_le(
    const void *data, int64_t length, uint8_t elem_type, int64_t offset, bool *ok) {
    return xr_array_core_bytes_load_u64(data, length, elem_type, offset, XR_ENDIAN_LE, ok);
}

#undef XR_BYTE_SLICE_SCALAR_INLINE

#endif /* XR_BYTE_SLICE_SCALAR_CORE_H */
