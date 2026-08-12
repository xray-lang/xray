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

#include "xr_semantic_owner_ids_gen.h"
#include <stddef.h>
#include <stdint.h>

/* Serialization-stable scalar kinds shared by Xi raw-pointer access and FFI. */
typedef enum XrRawScalarKind {
    XR_RAW_SCALAR_VOID = 0,
    XR_RAW_SCALAR_BOOL = 1,
    XR_RAW_SCALAR_I8 = 2,
    XR_RAW_SCALAR_U8 = 3,
    XR_RAW_SCALAR_I16 = 4,
    XR_RAW_SCALAR_U16 = 5,
    XR_RAW_SCALAR_I32 = 6,
    XR_RAW_SCALAR_U32 = 7,
    XR_RAW_SCALAR_I64 = 8,
    XR_RAW_SCALAR_U64 = 9,
    XR_RAW_SCALAR_F32 = 10,
    XR_RAW_SCALAR_F64 = 11,
    XR_RAW_SCALAR_PTR = 12,
    XR_RAW_SCALAR_SIZE = 13,
    XR_RAW_SCALAR_SSIZE = 14,
    XR_RAW_SCALAR_COUNT = 15
} XrRawScalarKind;

typedef struct XrRawScalarValue {
    uint64_t bits;
    double floating;
    void *pointer;
} XrRawScalarValue;

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

static inline void *xr_raw_mut_ptr_offset(void *ptr, intptr_t offset, int subtract) {
    uint8_t *base = (uint8_t *) ptr;
    XR_RAW_ASSUME_NON_NULL(base);
    return subtract ? (void *) (base - offset) : (void *) (base + offset);
}

static inline const void *xr_raw_const_ptr_offset(const void *ptr, intptr_t offset, int subtract) {
    const uint8_t *base = (const uint8_t *) ptr;
    XR_RAW_ASSUME_NON_NULL(base);
    return subtract ? (const void *) (base - offset) : (const void *) (base + offset);
}

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

static inline int xr_raw_scalar_kind_is_memory(uint8_t kind) {
    return kind > XR_RAW_SCALAR_VOID && kind < XR_RAW_SCALAR_COUNT;
}

static inline int xr_raw_scalar_kind_is_float(uint8_t kind) {
    return kind == XR_RAW_SCALAR_F32 || kind == XR_RAW_SCALAR_F64;
}

static inline int xr_raw_scalar_kind_is_pointer(uint8_t kind) {
    return kind == XR_RAW_SCALAR_PTR;
}

static inline int xr_raw_scalar_kind_is_signed(uint8_t kind) {
    return kind == XR_RAW_SCALAR_I8 || kind == XR_RAW_SCALAR_I16 ||
           kind == XR_RAW_SCALAR_I32 || kind == XR_RAW_SCALAR_I64 ||
           kind == XR_RAW_SCALAR_SSIZE;
}

static inline uint8_t xr_raw_scalar_width(uint8_t kind, uint8_t pointer_width) {
    switch (kind) {
        case XR_RAW_SCALAR_BOOL:
        case XR_RAW_SCALAR_I8:
        case XR_RAW_SCALAR_U8:
            return 1;
        case XR_RAW_SCALAR_I16:
        case XR_RAW_SCALAR_U16:
            return 2;
        case XR_RAW_SCALAR_I32:
        case XR_RAW_SCALAR_U32:
        case XR_RAW_SCALAR_F32:
            return 4;
        case XR_RAW_SCALAR_I64:
        case XR_RAW_SCALAR_U64:
        case XR_RAW_SCALAR_F64:
            return 8;
        case XR_RAW_SCALAR_PTR:
        case XR_RAW_SCALAR_SIZE:
        case XR_RAW_SCALAR_SSIZE:
            return pointer_width == 4 || pointer_width == 8 ? pointer_width : 0;
        default:
            return 0;
    }
}

static inline uint64_t xr_raw_scalar_load_bits(const void *ptr, uint8_t width, int64_t endian,
                                               int *ok) {
    uint64_t bits = 0;
    switch (width) {
        case 1:
            bits = xr_raw_load_u8_unaligned(ptr);
            break;
        case 2:
            bits = xr_raw_u16_from_endian(xr_raw_load_u16_unaligned(ptr), endian);
            break;
        case 4:
            bits = xr_raw_u32_from_endian(xr_raw_load_u32_unaligned(ptr), endian);
            break;
        case 8:
            bits = xr_raw_u64_from_endian(xr_raw_load_u64_unaligned(ptr), endian);
            break;
        default:
            if (ok)
                *ok = 0;
            return 0;
    }
    if (ok)
        *ok = 1;
    return bits;
}

static inline int64_t xr_raw_scalar_sign_extend(uint64_t bits, uint8_t width) {
    switch (width) {
        case 1:
            return (int64_t) (int8_t) bits;
        case 2:
            return (int64_t) (int16_t) bits;
        case 4:
            return (int64_t) (int32_t) bits;
        case 8:
            return (int64_t) bits;
        default:
            return 0;
    }
}

static inline int xr_raw_scalar_load(const void *ptr, uint8_t kind, uint8_t pointer_width,
                                     int64_t endian, XrRawScalarValue *out) {
    uint8_t width;
    uint64_t bits;
    int ok = 0;
    if (!out || !xr_raw_scalar_kind_is_memory(kind))
        return 0;
    out->bits = 0;
    out->floating = 0.0;
    out->pointer = NULL;
    if (kind == XR_RAW_SCALAR_PTR) {
        if (endian != XR_RAW_ENDIAN_NATIVE)
            return 0;
        out->pointer = xr_raw_load_ptr_unaligned(ptr);
        return 1;
    }
    width = xr_raw_scalar_width(kind, pointer_width);
    bits = xr_raw_scalar_load_bits(ptr, width, endian, &ok);
    if (!ok)
        return 0;
    if (kind == XR_RAW_SCALAR_F32) {
        out->floating = (double) xr_raw_f32_from_bits((uint32_t) bits);
    } else if (kind == XR_RAW_SCALAR_F64) {
        out->floating = xr_raw_f64_from_bits(bits);
    } else if (kind == XR_RAW_SCALAR_BOOL) {
        out->bits = bits != 0;
    } else if (xr_raw_scalar_kind_is_signed(kind)) {
        out->bits = (uint64_t) xr_raw_scalar_sign_extend(bits, width);
    } else {
        out->bits = bits;
    }
    return 1;
}

static inline int xr_raw_scalar_store_bits(void *ptr, uint8_t width, uint64_t bits,
                                           int64_t endian) {
    switch (width) {
        case 1:
            xr_raw_store_u8_unaligned(ptr, (uint8_t) bits);
            return 1;
        case 2:
            xr_raw_store_u16_unaligned(ptr, xr_raw_u16_from_endian((uint16_t) bits, endian));
            return 1;
        case 4:
            xr_raw_store_u32_unaligned(ptr, xr_raw_u32_from_endian((uint32_t) bits, endian));
            return 1;
        case 8:
            xr_raw_store_u64_unaligned(ptr, xr_raw_u64_from_endian(bits, endian));
            return 1;
        default:
            return 0;
    }
}

static inline int xr_raw_scalar_store(void *ptr, uint8_t kind, uint8_t pointer_width,
                                      int64_t endian, XrRawScalarValue value) {
    uint8_t width;
    uint64_t bits;
    if (!xr_raw_scalar_kind_is_memory(kind))
        return 0;
    if (kind == XR_RAW_SCALAR_PTR) {
        if (endian != XR_RAW_ENDIAN_NATIVE)
            return 0;
        xr_raw_store_ptr_unaligned(ptr, value.pointer);
        return 1;
    }
    width = xr_raw_scalar_width(kind, pointer_width);
    if (kind == XR_RAW_SCALAR_F32)
        bits = xr_raw_f32_to_bits((float) value.floating);
    else if (kind == XR_RAW_SCALAR_F64)
        bits = xr_raw_f64_to_bits(value.floating);
    else if (kind == XR_RAW_SCALAR_BOOL)
        bits = value.bits != 0;
    else
        bits = value.bits;
    return xr_raw_scalar_store_bits(ptr, width, bits, endian);
}

static inline int64_t xr_raw_scalar_load_i64(const void *ptr, uint8_t kind,
                                             uint8_t pointer_width, int64_t endian) {
    XrRawScalarValue value;
    return xr_raw_scalar_load(ptr, kind, pointer_width, endian, &value)
               ? (int64_t) value.bits
               : 0;
}

static inline double xr_raw_scalar_load_f64(const void *ptr, uint8_t kind,
                                            uint8_t pointer_width, int64_t endian) {
    XrRawScalarValue value;
    return xr_raw_scalar_load(ptr, kind, pointer_width, endian, &value) ? value.floating : 0.0;
}

static inline void *xr_raw_scalar_load_pointer(const void *ptr, uint8_t kind,
                                               uint8_t pointer_width, int64_t endian) {
    XrRawScalarValue value;
    return xr_raw_scalar_load(ptr, kind, pointer_width, endian, &value) ? value.pointer : NULL;
}

static inline void xr_raw_scalar_store_i64(void *ptr, uint8_t kind, uint8_t pointer_width,
                                           int64_t endian, int64_t input) {
    XrRawScalarValue value;
    value.bits = (uint64_t) input;
    value.floating = 0.0;
    value.pointer = NULL;
    (void) xr_raw_scalar_store(ptr, kind, pointer_width, endian, value);
}

static inline void xr_raw_scalar_store_f64(void *ptr, uint8_t kind, uint8_t pointer_width,
                                           int64_t endian, double input) {
    XrRawScalarValue value;
    value.bits = 0;
    value.floating = input;
    value.pointer = NULL;
    (void) xr_raw_scalar_store(ptr, kind, pointer_width, endian, value);
}

static inline void xr_raw_scalar_store_pointer(void *ptr, uint8_t kind, uint8_t pointer_width,
                                               int64_t endian, const void *input) {
    XrRawScalarValue value;
    value.bits = 0;
    value.floating = 0.0;
    value.pointer = (void *) input;
    (void) xr_raw_scalar_store(ptr, kind, pointer_width, endian, value);
}

#define xr_raw_scalar_load_aggregate(type, ptr) (*(const type *) (ptr))
#define xr_raw_scalar_store_aggregate(type, ptr, value) ((*(type *) (ptr)) = (value))

#define XR_RAW_SCALAR_ACCESS_OWNER_GUARD(owner_hi, owner_lo)                                     \
    ((void) sizeof(struct {                                                                      \
        unsigned int owner_id_must_be_shared_raw_scalar_access                                   \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI &&          \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO)             \
                   ? 1                                                                          \
                   : -1);                                                                       \
    }))

#define XR_RAW_SCALAR_ACCESS_CONSUMER_GUARD(consumer_bit)                                       \
    ((void) sizeof(struct {                                                                      \
        unsigned int consumer_must_be_declared_for_shared_raw_scalar_access                      \
            : (((uint32_t) (consumer_bit) != 0 &&                                               \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&          \
                (XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_CONSUMERS &                            \
                 (uint32_t) (consumer_bit)) != 0)                                               \
                   ? 1                                                                          \
                   : -1);                                                                       \
    }))

#define XR_RAW_SCALAR_ACCESS_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, expression)           \
    (XR_RAW_SCALAR_ACCESS_OWNER_GUARD((owner_hi), (owner_lo)),                                   \
     XR_RAW_SCALAR_ACCESS_CONSUMER_GUARD((consumer_bit)), (expression))

#undef XR_RAW_COPY
#undef XR_RAW_ASSUME_NON_NULL

#endif /* XR_RAW_SCALAR_CORE_H */
