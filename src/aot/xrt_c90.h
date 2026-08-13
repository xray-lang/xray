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

/* A hosting libc can publish the fixed-width integer surface even when the
 * dialect is restricted to C90, and it may spell int64_t as long long. The
 * Darwin SDK does exactly that through the <stdlib.h> this header already
 * includes. Restating the types there is a conflicting definition that stops
 * the translation unit, so spell the surface out only when the platform
 * published none of it, and adopt whatever it did publish otherwise. */
#if defined(INT64_MAX) || defined(_INT64_T) || defined(__int64_t_defined)
#define XRT_C90_LIBC_FIXED_WIDTH 1
#endif

#ifndef XRT_C90_LIBC_FIXED_WIDTH
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
#else
#error "restricted C90 output currently requires an LP64 target without long long"
#endif
#endif /* XRT_C90_LIBC_FIXED_WIDTH */

#ifndef INT64_C
#define INT64_C(value) value##L
#endif
#ifndef UINT64_C
#define UINT64_C(value) value##UL
#endif
#ifndef INT32_C
#define INT32_C(value) value
#endif
#ifndef UINT32_C
#define UINT32_C(value) value##U
#endif
#ifndef INT64_MAX
#define INT64_MAX LONG_MAX
#endif

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

#define XR_BYTE_ARRAY_COPY_C90 1
#include "../shared/xr_byte_array_copy_core.h"
#undef XR_BYTE_ARRAY_COPY_C90

#define xrt_byte_array_copy_semantics(kind, dst_data, dst_length, dst_elem_type, src_data,        \
                                      src_length, src_elem_type, src_offset, dst_offset, count)   \
    xr_byte_array_copy_core((kind), (dst_data), (dst_length), (dst_elem_type), (src_data),       \
                            (src_length), (src_elem_type), (src_offset), (dst_offset), (count))

/* Restricted C90 mechanically projects the same address/lifetime pair. */
#define XR_DATA_POINTER_C90 1
#include "../shared/xr_data_pointer_core.h"
#undef XR_DATA_POINTER_C90

static XrDataPointerProjection
xrt_data_pointer_project(const void *address, XrDataPointerLifetime lifetime) {
    return xr_data_pointer_project_core(address, lifetime);
}

/* Restricted C90 is a mechanical ABI adapter over the raw-memory owner core. */
#define XR_RAW_MEMORY_C90 1
#include "../shared/xr_raw_memory_core.h"
#undef XR_RAW_MEMORY_C90

static void *xrt_raw_memory_copy_nonoverlap(void *dst, const void *src, int64_t count) {
    return xr_raw_memory_copy_nonoverlap(dst, src, count);
}

#define XR_BITS_ROTL32(value, count)                                                               \
    ((uint32_t) (((uint32_t) (value) << ((uint32_t) (count) & UINT32_C(31))) |                     \
                 ((uint32_t) (value) >>                                                            \
                  ((UINT32_C(32) - ((uint32_t) (count) & UINT32_C(31))) & UINT32_C(31)))))

typedef struct xrt_closure {
    void *reserved;
} xrt_closure_t;

typedef struct xr_span {
    void *data;
    int64_t length;
} xr_span_t;

/* The restricted runtime is a mechanical adapter over the same byte-slice
 * scalar owner as hosted and freestanding AOT. */
#define XR_BYTE_SLICE_SCALAR_C90 1
#include "../shared/xr_byte_slice_scalar_core.h"
#undef XR_BYTE_SLICE_SCALAR_C90
#define xrt_byte_slice_scalar_eval(expression) (expression)

#define XR_POD_SLICE_C90 1
#include "../shared/xr_pod_slice_core.h"
#undef XR_POD_SLICE_C90

static xr_span_t xrt_byte_slice_fill_checked_raw(xr_span_t span, int64_t value) {
    if (!xr_byte_slice_fill_core(span.data, span.length, XR_ELEM_U8, value))
        abort();
    return span;
}

static xr_span_t xrt_byte_slice_copy_checked_raw(xr_span_t dst, xr_span_t src) {
    if (!xr_byte_slice_copy_core(dst.data, dst.length, XR_ELEM_U8, src.data, src.length,
                                 XR_ELEM_U8))
        abort();
    return dst;
}

static xr_span_t xrt_byte_slice_repeat_from_checked_raw(xr_span_t span, int64_t dst_offset,
                                                        int64_t distance, int64_t count) {
    if (!xr_byte_slice_repeat_core(span.data, span.length, XR_ELEM_U8, dst_offset, distance,
                                   count))
        abort();
    return span;
}

static int64_t xrt_byte_slice_compare_checked_raw(xr_span_t left, xr_span_t right) {
    bool ok = false;
    int64_t ordering;
    ordering = xr_byte_slice_compare_core(left.data, left.length, XR_ELEM_U8, right.data,
                                          right.length, XR_ELEM_U8, &ok);
    if (!ok)
        abort();
    return ordering;
}

static int64_t xrt_byte_slice_common_prefix_checked_raw(xr_span_t left, xr_span_t right) {
    bool ok = false;
    int64_t prefix;
    prefix = xr_byte_slice_common_prefix_core(left.data, left.length, XR_ELEM_U8, right.data,
                                              right.length, XR_ELEM_U8, &ok);
    if (!ok)
        abort();
    return prefix;
}

static xr_span_t xrt_span_copy_checked_raw(xr_span_t dst, xr_span_t src, uint16_t elem_size) {
    if (xr_pod_slice_copy_core(dst.data, dst.length, elem_size, src.data, src.length, elem_size) !=
        XR_POD_SLICE_OK)
        abort();
    return dst;
}

static xr_span_t xrt_span_fill_checked_raw(xr_span_t span, uint16_t elem_size,
                                           XrPodSliceFillKind kind,
                                           XrPodSliceFillValue value) {
    XrPodSliceStatus status = xr_pod_slice_fill_core(span.data, span.length, elem_size, kind, value);
    if (status != XR_POD_SLICE_OK)
        abort();
    return span;
}

static int64_t xrt_span_compare_checked_raw(xr_span_t left, xr_span_t right,
                                            uint16_t elem_size) {
    XrPodSliceCompareResult result;
    result = xr_pod_slice_compare_core(left.data, left.length, elem_size, right.data, right.length,
                                       elem_size);
    if (result.status != XR_POD_SLICE_OK)
        abort();
    return result.ordering;
}

static xr_span_t xrt_pod_slice_view_checked_raw(
    xr_span_t span, XrPodSliceViewKind kind, uint16_t source_elem_size, bool source_has_layout,
    uint16_t target_elem_size, uint16_t target_expected_elem_size, uint16_t target_alignment,
    bool target_layout_valid, bool target_is_aggregate) {
    XrPodSliceViewResult result;
    result = xr_pod_slice_view_core(kind, span.data, span.length, source_elem_size,
                                    source_has_layout, target_elem_size,
                                    target_expected_elem_size, target_alignment,
                                    target_layout_valid, target_is_aggregate);
    if (result.status != XR_POD_SLICE_VIEW_OK)
        abort();
    span.data = result.data;
    span.length = result.length;
    return span;
}

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

#define XR_NULL_TEST_C90 1
#include "../shared/xr_null_test_core.h"
#undef XR_NULL_TEST_C90
#define xrt_null_test_tagged(tag) xr_null_test_tagged_core((uint8_t) (tag))
#define xrt_null_test_pointer(pointer) xr_null_test_pointer_is_null_core((const void *) (pointer))

#define XR_ASSERT_CONDITION_C90 1
#include "../shared/xr_assert_condition_core.h"
#undef XR_ASSERT_CONDITION_C90
#define xrt_assert_condition_failed(truthy, expected_truthy)                                     \
    xr_assert_condition_failed_core((truthy), (expected_truthy))

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

static void *xr_raw_mut_ptr_offset(void *ptr, intptr_t offset, int subtract) {
    uint8_t *base = (uint8_t *) ptr;
    XR_ASSUME(base != NULL);
    return subtract ? (void *) (base - offset) : (void *) (base + offset);
}

static const void *xr_raw_const_ptr_offset(const void *ptr, intptr_t offset, int subtract) {
    const uint8_t *base = (const uint8_t *) ptr;
    XR_ASSUME(base != NULL);
    return subtract ? (const void *) (base - offset) : (const void *) (base + offset);
}

static uint8_t xr_raw_load_u8_unaligned(const void *ptr) {
    uint8_t value;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

static void xr_raw_store_u8_unaligned(void *ptr, uint8_t value) {
    memcpy(ptr, &value, sizeof(value));
}

static int64_t xrt_byte_slice_load_u32_unchecked_raw(xr_span_t span, int64_t offset,
                                                     int64_t endian) {
    return (int64_t) xrt_byte_slice_scalar_eval(
        xr_byte_slice_scalar_load_u32_unchecked(span.data, offset, endian));
}

static int64_t xrt_byte_slice_load_u32_le_unchecked_raw(xr_span_t span, int64_t offset) {
    return xrt_byte_slice_load_u32_unchecked_raw(span, offset, XR_ENDIAN_LE);
}

static int64_t xrt_byte_slice_load_u64_unchecked_raw(xr_span_t span, int64_t offset,
                                                     int64_t endian) {
    return (int64_t) xrt_byte_slice_scalar_eval(
        xr_byte_slice_scalar_load_u64_unchecked(span.data, offset, endian));
}

static void xrt_byte_slice_store_u32_unchecked_raw(xr_span_t span, int64_t offset, uint32_t value,
                                                   int64_t endian) {
    xrt_byte_slice_scalar_eval(
        xr_byte_slice_scalar_store_u32_unchecked(span.data, offset, value, endian));
}

static void xrt_byte_slice_store_u64_unchecked_raw(xr_span_t span, int64_t offset, uint64_t value,
                                                   int64_t endian) {
    xrt_byte_slice_scalar_eval(
        xr_byte_slice_scalar_store_u64_unchecked(span.data, offset, value, endian));
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

static xr_span_t xrt_c90_span_window_unchecked(xr_span_t source, int64_t start, int64_t count,
                                               size_t element_size) {
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
