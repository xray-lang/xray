/*
 * xrt_c90.h - restricted ISO C90 kernel surface for generated AOT code
 *
 * This header intentionally exposes only the scalar/POD operations accepted by
 * XI_CGEN_C_DIALECT_C90.  It is not the hosted or general freestanding runtime.
 */

#ifndef XRT_C90_H
#define XRT_C90_H

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef signed short int16_t;
typedef unsigned short uint16_t;

#if UINT_MAX == 0xffffffffU
typedef signed int int32_t;
typedef unsigned int uint32_t;
#elif ULONG_MAX == 0xffffffffUL
typedef signed long int32_t;
typedef unsigned long uint32_t;
#else
#error "restricted C90 output requires a 32-bit int or long type"
#endif

#if ULONG_MAX > 0xffffffffUL
typedef signed long int64_t;
typedef unsigned long uint64_t;
typedef signed long intptr_t;
typedef unsigned long uintptr_t;
#define INT64_C(value) value##L
#define UINT64_C(value) value##UL
#else
#error "restricted C90 output currently requires an LP64 target without long long"
#endif

#define INT32_C(value) value
#define UINT32_C(value) value##U

typedef int bool;
#define false 0
#define true 1

#if defined(__GNUC__) || defined(__clang__)
#define XR_EXPORT_SYM __attribute__((visibility("default")))
#define XRT_INTERNAL __attribute__((visibility("hidden")))
#define XRT_FN_CONST __attribute__((const))
#define XRT_FN_PURE __attribute__((pure))
#else
#define XR_EXPORT_SYM
#define XRT_INTERNAL
#define XRT_FN_CONST
#define XRT_FN_PURE
#endif

#define XR_FORCEINLINE
#define XR_AINLINE
#define XR_LIKELY(value) (value)
#define XR_UNLIKELY(value) (value)
#define XR_ASSUME(value) ((void) 0)

#define XR_ENDIAN_NATIVE 0
#define XR_ENDIAN_LE 1
#define XR_ENDIAN_BE 2
#define XRT_TARGET_NATIVE_ENDIAN XR_ENDIAN_NATIVE
#define XR_ELEM_U8 6

#define XR_BITS_ROTL32(value, count)                                                     \
    ((uint32_t) (((uint32_t) (value) << ((uint32_t) (count) & UINT32_C(31))) |          \
                 ((uint32_t) (value) >>                                                  \
                  ((UINT32_C(32) - ((uint32_t) (count) & UINT32_C(31))) & UINT32_C(31)))))

typedef struct xrt_closure {
    void *reserved;
} xrt_closure_t;

typedef struct xr_span {
    void *data;
    int64_t length;
} xr_span_t;

/* Restricted generated kernels do not use dynamic values at their public ABI.
 * The compact compatibility record remains available for fixed-array address
 * projections that the current Xi lowering represents through `.ptr`. */
typedef struct XrValue {
    int64_t i;
    double f;
    void *ptr;
    uint32_t count;
    uint16_t aux;
    uint8_t elem_type;
    uint8_t tag;
} XrValue;

static XrValue xrt_c90_null_value(void) {
    XrValue value;
    memset(&value, 0, sizeof(value));
    return value;
}

static XrValue xrt_c90_int_value(int64_t input) {
    XrValue value = xrt_c90_null_value();
    value.i = input;
    return value;
}

#define XR_NULL_VAL xrt_c90_null_value()
#define XR_FROM_INT(value) xrt_c90_int_value((int64_t) (value))
#define XR_TO_INT(value) ((value).i)

static XrValue xr_array_ref(void *ptr, uint8_t elem_native_type, uint32_t elem_count) {
    XrValue value = xrt_c90_null_value();
    value.ptr = ptr;
    value.elem_type = elem_native_type;
    value.count = elem_count;
    return value;
}

static xr_span_t xrt_span_empty(void) {
    xr_span_t span;
    span.data = NULL;
    span.length = INT64_C(0);
    return span;
}

static xr_span_t xrt_c90_span_from_ptr(const void *ptr, int64_t length) {
    xr_span_t span;
    span.data = (void *) ptr;
    span.length = length;
    return span;
}

static uint8_t xr_raw_load_u8_unaligned(const void *ptr) {
    uint8_t value;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

static void xr_raw_store_u8_unaligned(void *ptr, uint8_t value) {
    memcpy(ptr, &value, sizeof(value));
}

static uint32_t xrt_c90_load_u32(const void *ptr) {
    uint32_t value;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

static uint64_t xrt_c90_load_u64(const void *ptr) {
    uint64_t value;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

static void xrt_c90_store_u32(void *ptr, uint32_t value) {
    memcpy(ptr, &value, sizeof(value));
}

static void xrt_c90_store_u64(void *ptr, uint64_t value) {
    memcpy(ptr, &value, sizeof(value));
}

static int xrt_c90_host_is_little_endian(void) {
    const uint16_t one = 1;
    return *((const uint8_t *) &one) == 1;
}

static uint32_t xrt_c90_bswap32(uint32_t value) {
    return ((value & UINT32_C(0x000000ff)) << 24) |
           ((value & UINT32_C(0x0000ff00)) << 8) |
           ((value & UINT32_C(0x00ff0000)) >> 8) |
           ((value & UINT32_C(0xff000000)) >> 24);
}

static uint64_t xrt_c90_bswap64(uint64_t value) {
    uint32_t low = (uint32_t) value;
    uint32_t high = (uint32_t) (value >> 32);
    return ((uint64_t) xrt_c90_bswap32(low) << 32) | (uint64_t) xrt_c90_bswap32(high);
}

static int xrt_c90_endian_matches_host(int64_t endian) {
    if (endian == XR_ENDIAN_NATIVE)
        return 1;
    return (endian == XR_ENDIAN_LE) == xrt_c90_host_is_little_endian();
}

static int64_t xrt_byte_slice_load_u32_unchecked_raw(xr_span_t span, int64_t offset,
                                                      int64_t endian) {
    uint32_t value = xrt_c90_load_u32((const uint8_t *) span.data + offset);
    if (!xrt_c90_endian_matches_host(endian))
        value = xrt_c90_bswap32(value);
    return (int64_t) value;
}

static int64_t xrt_byte_slice_load_u32_le_unchecked_raw(xr_span_t span, int64_t offset) {
    return xrt_byte_slice_load_u32_unchecked_raw(span, offset, XR_ENDIAN_LE);
}

static int64_t xrt_byte_slice_load_u64_unchecked_raw(xr_span_t span, int64_t offset,
                                                      int64_t endian) {
    uint64_t value = xrt_c90_load_u64((const uint8_t *) span.data + offset);
    if (!xrt_c90_endian_matches_host(endian))
        value = xrt_c90_bswap64(value);
    return (int64_t) value;
}

static void xrt_byte_slice_store_u32_unchecked_raw(xr_span_t span, int64_t offset,
                                                    uint32_t value, int64_t endian) {
    if (!xrt_c90_endian_matches_host(endian))
        value = xrt_c90_bswap32(value);
    xrt_c90_store_u32((uint8_t *) span.data + offset, value);
}

static void xrt_byte_slice_store_u64_unchecked_raw(xr_span_t span, int64_t offset,
                                                    uint64_t value, int64_t endian) {
    if (!xrt_c90_endian_matches_host(endian))
        value = xrt_c90_bswap64(value);
    xrt_c90_store_u64((uint8_t *) span.data + offset, value);
}

static void xrt_freestanding_trap(const char *message) {
    (void) message;
    abort();
}

static void xrt_index_oob(int64_t index, int64_t length) {
    (void) index;
    (void) length;
    xrt_freestanding_trap("index out of bounds");
}

static void xrt_fixed_index_oob(int64_t index, int64_t length) {
    xrt_index_oob(index, length);
}

static xr_span_t xrt_c90_span_window_unchecked(xr_span_t source, int64_t start,
                                                int64_t count, size_t element_size) {
    xr_span_t result;
    result.data = source.data;
    if (count > 0)
        result.data = (void *) ((uint8_t *) source.data + (size_t) start * element_size);
    result.length = count;
    return result;
}

static xr_span_t xrt_c90_span_window(xr_span_t source, int64_t start, int64_t count,
                                      size_t element_size) {
    if (source.length < 0 || start < 0 || count < 0 || start > source.length ||
        count > source.length - start || (count > 0 && source.data == NULL))
        xrt_index_oob(start, source.length);
    return xrt_c90_span_window_unchecked(source, start, count, element_size);
}

static uint8_t xrt_c90_span_u8_get_unchecked(xr_span_t span, int64_t index) {
    return ((const uint8_t *) span.data)[index];
}

static uint8_t xrt_c90_span_u8_get(xr_span_t span, int64_t index) {
    if (index < 0 || index >= span.length || span.data == NULL)
        xrt_index_oob(index, span.length);
    return xrt_c90_span_u8_get_unchecked(span, index);
}

static void xrt_c90_span_u8_set_unchecked(xr_span_t span, int64_t index, uint8_t value) {
    ((uint8_t *) span.data)[index] = value;
}

static void xrt_c90_span_u8_set(xr_span_t span, int64_t index, uint8_t value) {
    if (index < 0 || index >= span.length || span.data == NULL)
        xrt_index_oob(index, span.length);
    xrt_c90_span_u8_set_unchecked(span, index, value);
}

static void xrt_c90_fixed_u8_set_unchecked(uint8_t *data, int64_t length, int64_t index,
                                            uint8_t value) {
    (void) length;
    data[index] = value;
}

static void xrt_c90_fixed_u8_set(uint8_t *data, int64_t length, int64_t index, uint8_t value) {
    if (index < 0 || index >= length || data == NULL)
        xrt_fixed_index_oob(index, length);
    xrt_c90_fixed_u8_set_unchecked(data, length, index, value);
}

#endif /* XRT_C90_H */
