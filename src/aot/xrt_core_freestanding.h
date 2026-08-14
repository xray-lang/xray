/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_core_freestanding.h - no-libc AOT prelude for --profile freestanding
 *
 * This header intentionally stays below hosted xrt.h.  It exposes the value
 * representation and scalar ABI helpers needed by freestanding manifest exports
 * code without pulling in malloc, stdio, pthreads, setjmp, libm, or OS APIs.
 */

#ifndef XRT_CORE_FREESTANDING_H
#define XRT_CORE_FREESTANDING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>
#include <float.h>
#include "../shared/xr_atomic_compat.h"
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include "xrt_callable.h"

#ifdef memcpy
#undef memcpy
#endif
#ifdef memmove
#undef memmove
#endif
#ifdef memset
#undef memset
#endif
#ifdef memcmp
#undef memcmp
#endif

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int value, size_t n);
int memcmp(const void *a, const void *b, size_t n);

#include "../shared/xr_raw_scalar_core.h"
#include "../shared/xr_byte_array_copy_core.h"
#include "../shared/xr_raw_memory_core.h"
#include "../shared/xr_data_pointer_core.h"
#include "../shared/xr_obj_header.h"
#include "../shared/xr_elem_type.h"
#include "../shared/xr_byte_slice_scalar_core.h"
#include "../shared/xr_pod_slice_core.h"
#include "../shared/xr_arith_core.h"
#include "../shared/xr_error_messages.h"
#include "../shared/xr_enum_metadata_core.h"
#include "../shared/xr_int_arith_core.h" /* xr_i64_*_wrap for int wrapping methods (task 153) */
#include "../shared/xr_numeric_conversion_core.h"
/* libc-free int(s)/float(s) decimal grammar, shared with the VM/hosted AOT */
#include "../shared/xr_string_parse_core.h"
#include "../shared/xr_bits_core.h" /* exact-width compiler bit intrinsics */
#include "../shared/xr_range_core.h"
#include "../shared/xr_slice_window_core.h" /* canonical strict contiguous window */
#include "../shared/xr_numeric_core.h"
#include "../shared/xr_null_test_core.h"
#include "../shared/xr_assert_condition_core.h"
#include "../shared/xr_compare_core.h"
#define xrt_compare_route(kind, left_class, right_class)                                          \
    XR_COMPARE_OWNER_ROUTE(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                                     \
                           XR_SEM_OWNER_ID_SHARED_COMPARE_LO,                                     \
                           XR_SEM_CONSUMER_AOT_FREESTANDING, (kind), (left_class), (right_class))
#define xrt_compare_i64(kind, a, b)                                                               \
    XR_COMPARE_OWNER_APPLY_I64(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                                 \
                               XR_SEM_OWNER_ID_SHARED_COMPARE_LO,                                 \
                               XR_SEM_CONSUMER_AOT_FREESTANDING, (kind), (a), (b))
#define xrt_compare_u64(kind, a, b)                                                               \
    XR_COMPARE_OWNER_APPLY_U64(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                                 \
                               XR_SEM_OWNER_ID_SHARED_COMPARE_LO,                                 \
                               XR_SEM_CONSUMER_AOT_FREESTANDING, (kind), (a), (b))
#define xrt_compare_f64(kind, a, b)                                                               \
    XR_COMPARE_OWNER_APPLY_F64(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                                 \
                               XR_SEM_OWNER_ID_SHARED_COMPARE_LO,                                 \
                               XR_SEM_CONSUMER_AOT_FREESTANDING, (kind), (a), (b))
#define xrt_compare_ptr(kind, a, b)                                                               \
    XR_COMPARE_OWNER_APPLY_PTR(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                                 \
                               XR_SEM_OWNER_ID_SHARED_COMPARE_LO,                                 \
                               XR_SEM_CONSUMER_AOT_FREESTANDING, (kind), (a), (b))
#define xrt_compare_ordering(kind, ordering)                                                      \
    XR_COMPARE_OWNER_APPLY_ORDERING(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                            \
                                    XR_SEM_OWNER_ID_SHARED_COMPARE_LO,                            \
                                    XR_SEM_CONSUMER_AOT_FREESTANDING, (kind), (ordering))
#define xrt_compare_equal(kind, equal)                                                            \
    XR_COMPARE_OWNER_APPLY_EQUAL(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                               \
                                 XR_SEM_OWNER_ID_SHARED_COMPARE_LO,                               \
                                 XR_SEM_CONSUMER_AOT_FREESTANDING, (kind), (equal))
/* Spelled into generated C for a proven scalar comparison: the relation comes
 * from the owner while the operand type stays the one the plan chose. */
#define xrt_compare_native(relation, a, b)                                                        \
    XR_COMPARE_OWNER_APPLY_NATIVE(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                              \
                                  XR_SEM_OWNER_ID_SHARED_COMPARE_LO,                              \
                                  XR_SEM_CONSUMER_AOT_FREESTANDING, relation, (a), (b))
#define xrt_bits_exact_eval(kernel, lhs, rhs, native_type)                                        \
    XR_BITS_EXACT_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_BITS_HI,                                     \
                              XR_SEM_OWNER_ID_SHARED_BITS_LO,                                     \
                              XR_SEM_CONSUMER_AOT_FREESTANDING, kernel, lhs, rhs, native_type)
#define xrt_bits_not_eval(value)                                                                  \
    XR_BITS_NOT_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_BITS_NOT_HI,                                  \
                            XR_SEM_OWNER_ID_SHARED_BITS_NOT_LO,                                  \
                            XR_SEM_CONSUMER_AOT_FREESTANDING, value)
#define xrt_bitwise_binary_eval(kind, lhs, rhs)                                                   \
    XR_BITWISE_BINARY_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_BITWISE_BINARY_HI,                       \
                                   XR_SEM_OWNER_ID_SHARED_BITWISE_BINARY_LO,                      \
                                   XR_SEM_CONSUMER_AOT_FREESTANDING, kind, lhs, rhs)
#define xrt_shift_eval(kind, value, count)                                                        \
    XR_SHIFT_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_SHIFT_HI,                                        \
                         XR_SEM_OWNER_ID_SHARED_SHIFT_LO,                                        \
                         XR_SEM_CONSUMER_AOT_FREESTANDING, kind, value, count)
#define xrt_numeric_width_eval(kernel, value)                                                      \
    XR_NUMERIC_WIDTH_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI,                    \
                                 XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO,                    \
                                 XR_SEM_CONSUMER_AOT_FREESTANDING, kernel, value)
#define xrt_range_semantics(start, end, inclusive_end)                                            \
    XR_RANGE_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_RANGE_HI,                                        \
                         XR_SEM_OWNER_ID_SHARED_RANGE_LO, XR_SEM_CONSUMER_AOT_FREESTANDING,       \
                         start, end, inclusive_end)
#define xrt_numeric_neg_eval(kind, i64, f64)                                                       \
    XR_NUMERIC_NEG_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_HI,                             \
                               XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_LO,                             \
                               XR_SEM_CONSUMER_AOT_FREESTANDING, kind, i64, f64)
#define XRT_FREESTANDING_INT_DIV_MOD_CHECKED(kind, lhs, rhs)                                      \
    XR_INT_DIV_MOD_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_HI,                             \
                               XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_LO,                             \
                               XR_SEM_CONSUMER_AOT_FREESTANDING, (kind),                          \
                               XR_INT_DIV_MOD_PROOF_NONE, (lhs), (rhs))
#define xrt_int_div_mod_eval(kind, proof, lhs, rhs)                                               \
    XR_INT_DIV_MOD_OWNER_APPLY_PROVEN(XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_HI,                      \
                                      XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_LO,                      \
                                      XR_SEM_CONSUMER_AOT_FREESTANDING, (kind), (proof), (lhs),   \
                                      (rhs))
/* Generated C asks the window owner through this name; the proof token decides
 * whether the admissibility probe is still the kernel's job. */
#define xrt_slice_window_plan(proof, length, start, count, data, element_size)                     \
    XR_SLICE_WINDOW_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_SLICE_WINDOW_HI,                           \
                                XR_SEM_OWNER_ID_SHARED_SLICE_WINDOW_LO,                           \
                                XR_SEM_CONSUMER_AOT_FREESTANDING, (proof), (length), (start),     \
                                (count), (data), (element_size))
#define xrt_null_test_tagged(tag)                                                                 \
    XR_NULL_TEST_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_NULL_TEST_HI,                                \
                             XR_SEM_OWNER_ID_SHARED_NULL_TEST_LO,                                \
                             XR_SEM_CONSUMER_AOT_FREESTANDING,                                  \
                             xr_null_test_tagged_core((uint8_t) (tag)))
#define xrt_null_test_pointer(pointer)                                                            \
    XR_NULL_TEST_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_NULL_TEST_HI,                                \
                             XR_SEM_OWNER_ID_SHARED_NULL_TEST_LO,                                \
                             XR_SEM_CONSUMER_AOT_FREESTANDING,                                  \
                             xr_null_test_pointer_is_null_core((const void *) (pointer)))
#define xrt_assert_condition_failed(truthy, expected_truthy)                                      \
    XR_ASSERT_CONDITION_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_ASSERT_CONDITION_HI,                                              \
        XR_SEM_OWNER_ID_SHARED_ASSERT_CONDITION_LO,                                              \
        XR_SEM_CONSUMER_AOT_FREESTANDING, (truthy), (expected_truthy))
#define xrt_data_pointer_project(address, lifetime)                                               \
    XR_DATA_POINTER_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_DATA_POINTER_HI,                           \
                                XR_SEM_OWNER_ID_SHARED_DATA_POINTER_LO,                           \
                                XR_SEM_CONSUMER_AOT_FREESTANDING, address, lifetime)
#define xrt_raw_memory_copy_nonoverlap(dst, src, count)                                           \
    XR_RAW_MEMORY_COPY_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_RAW_MEMORY_COPY_HI,                     \
                                   XR_SEM_OWNER_ID_SHARED_RAW_MEMORY_COPY_LO,                     \
                                   XR_SEM_CONSUMER_AOT_FREESTANDING, dst, src, count)
#define xrt_raw_scalar_access_load_i64(ptr, kind, pointer_width, endian)                           \
    XR_RAW_SCALAR_ACCESS_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI,                                              \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO, XR_SEM_CONSUMER_AOT_FREESTANDING,           \
        xr_raw_scalar_load_i64((ptr), (kind), (pointer_width), (endian)))
#define xrt_raw_scalar_access_load_f64(ptr, kind, pointer_width, endian)                           \
    XR_RAW_SCALAR_ACCESS_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI,                                              \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO, XR_SEM_CONSUMER_AOT_FREESTANDING,           \
        xr_raw_scalar_load_f64((ptr), (kind), (pointer_width), (endian)))
#define xrt_raw_scalar_access_load_pointer(ptr, kind, pointer_width, endian)                       \
    XR_RAW_SCALAR_ACCESS_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI,                                              \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO, XR_SEM_CONSUMER_AOT_FREESTANDING,           \
        xr_raw_scalar_load_pointer((ptr), (kind), (pointer_width), (endian)))
#define xrt_raw_scalar_access_store_i64(ptr, kind, pointer_width, endian, value)                   \
    XR_RAW_SCALAR_ACCESS_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI,                                              \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO, XR_SEM_CONSUMER_AOT_FREESTANDING,           \
        xr_raw_scalar_store_i64((ptr), (kind), (pointer_width), (endian), (value)))
#define xrt_raw_scalar_access_store_f64(ptr, kind, pointer_width, endian, value)                   \
    XR_RAW_SCALAR_ACCESS_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI,                                              \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO, XR_SEM_CONSUMER_AOT_FREESTANDING,           \
        xr_raw_scalar_store_f64((ptr), (kind), (pointer_width), (endian), (value)))
#define xrt_raw_scalar_access_store_pointer(ptr, kind, pointer_width, endian, value)               \
    XR_RAW_SCALAR_ACCESS_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI,                                              \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO, XR_SEM_CONSUMER_AOT_FREESTANDING,           \
        xr_raw_scalar_store_pointer((ptr), (kind), (pointer_width), (endian), (value)))
#define xrt_raw_scalar_access(type, ptr)                                                          \
    XR_RAW_SCALAR_ACCESS_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI,                                              \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO, XR_SEM_CONSUMER_AOT_FREESTANDING,           \
        xr_raw_scalar_load_aggregate(type, ptr))
#define xrt_raw_scalar_access_store(type, ptr, value)                                             \
    XR_RAW_SCALAR_ACCESS_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI,                                              \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO, XR_SEM_CONSUMER_AOT_FREESTANDING,           \
        xr_raw_scalar_store_aggregate(type, ptr, value))
#define xrt_byte_slice_scalar_eval(expression)                                                     \
    XR_BYTE_SLICE_SCALAR_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_HI,                 \
                                     XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_LO,                 \
                                     XR_SEM_CONSUMER_AOT_FREESTANDING, expression)
#define xrt_byte_slice_compare_semantics(left_data, left_length, right_data, right_length, ok)     \
    XR_BYTE_SLICE_COMPARE_OWNER_APPLY(                                                            \
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMPARE_HI,                                             \
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMPARE_LO, XR_SEM_CONSUMER_AOT_FREESTANDING,           \
        xr_byte_slice_compare_core((left_data), (left_length), XR_ELEM_U8, (right_data),          \
                                   (right_length), XR_ELEM_U8, (ok)))
#define xrt_byte_slice_fill_semantics(data, length, elem_type, value)                              \
    XR_BYTE_SLICE_FILL_OWNER_APPLY(                                                               \
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_FILL_HI, XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_FILL_LO,    \
        XR_SEM_CONSUMER_AOT_FREESTANDING,                                                         \
        xr_byte_slice_fill_core((data), (length), (elem_type), (value)))
#define xrt_byte_slice_copy_semantics(dst_data, dst_length, src_data, src_length)                  \
    XR_BYTE_SLICE_COPY_OWNER_APPLY(                                                               \
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COPY_HI, XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COPY_LO,    \
        XR_SEM_CONSUMER_AOT_FREESTANDING,                                                         \
        xr_byte_slice_copy_core((dst_data), (dst_length), XR_ELEM_U8, (src_data), (src_length),  \
                                XR_ELEM_U8))
#define xrt_byte_array_copy_semantics(kind, dst_data, dst_length, dst_elem_type, src_data,        \
                                      src_length, src_elem_type, src_offset, dst_offset, count)   \
    XR_BYTE_ARRAY_COPY_OWNER_APPLY(                                                              \
        XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_COPY_HI,                                               \
        XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_COPY_LO, XR_SEM_CONSUMER_AOT_FREESTANDING,            \
        xr_byte_array_copy_core((kind), (dst_data), (dst_length), (dst_elem_type), (src_data),   \
                                (src_length), (src_elem_type), (src_offset), (dst_offset),       \
                                (count)))
#define xrt_byte_slice_repeat_semantics(data, length, dst_offset, distance, count)                 \
    XR_BYTE_SLICE_REPEAT_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_REPEAT_HI,                                             \
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_REPEAT_LO, XR_SEM_CONSUMER_AOT_FREESTANDING,           \
        xr_byte_slice_repeat_core((data), (length), XR_ELEM_U8, (dst_offset), (distance),        \
                                  (count)))
#define xrt_pod_slice_copy_semantics(dst_data, dst_length, dst_elem_size, src_data, src_length,   \
                                     src_elem_size)                                               \
    XR_POD_SLICE_COPY_OWNER_APPLY(                                                               \
        XR_SEM_OWNER_ID_SHARED_POD_SLICE_COPY_HI, XR_SEM_OWNER_ID_SHARED_POD_SLICE_COPY_LO,     \
        XR_SEM_CONSUMER_AOT_FREESTANDING,                                                        \
        xr_pod_slice_copy_core((dst_data), (dst_length), (dst_elem_size), (src_data),            \
                               (src_length), (src_elem_size)))
#define xrt_pod_slice_fill_semantics(data, length, elem_size, kind, value)                        \
    XR_POD_SLICE_FILL_OWNER_APPLY(                                                               \
        XR_SEM_OWNER_ID_SHARED_POD_SLICE_FILL_HI, XR_SEM_OWNER_ID_SHARED_POD_SLICE_FILL_LO,     \
        XR_SEM_CONSUMER_AOT_FREESTANDING,                                                       \
        xr_pod_slice_fill_core((data), (length), (elem_size), (kind), (value)))
#define xrt_pod_slice_compare_semantics(left_data, left_length, left_elem_size, right_data,       \
                                        right_length, right_elem_size)                            \
    XR_POD_SLICE_COMPARE_OWNER_APPLY(                                                            \
        XR_SEM_OWNER_ID_SHARED_POD_SLICE_COMPARE_HI,                                             \
        XR_SEM_OWNER_ID_SHARED_POD_SLICE_COMPARE_LO, XR_SEM_CONSUMER_AOT_FREESTANDING,          \
        xr_pod_slice_compare_core((left_data), (left_length), (left_elem_size), (right_data),    \
                                  (right_length), (right_elem_size)))
#define xrt_pod_slice_view_semantics(kind, data, length, source_elem_size, source_has_layout,     \
                                     target_elem_size, target_expected_elem_size,                 \
                                     target_alignment, target_layout_valid, target_is_aggregate) \
    XR_POD_SLICE_VIEW_OWNER_APPLY(                                                               \
        XR_SEM_OWNER_ID_SHARED_POD_SLICE_VIEW_HI, XR_SEM_OWNER_ID_SHARED_POD_SLICE_VIEW_LO,     \
        XR_SEM_CONSUMER_AOT_FREESTANDING,                                                        \
        xr_pod_slice_view_core((kind), (data), (length), (source_elem_size),                    \
                               (source_has_layout), (target_elem_size),                          \
                               (target_expected_elem_size), (target_alignment),                 \
                               (target_layout_valid), (target_is_aggregate)))
#define xrt_byte_slice_common_prefix_semantics(left_data, left_length, right_data, right_length,   \
                                               ok)                                                \
    XR_BYTE_SLICE_COMMON_PREFIX_OWNER_APPLY(                                                      \
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMMON_PREFIX_HI,                                       \
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMMON_PREFIX_LO,                                       \
        XR_SEM_CONSUMER_AOT_FREESTANDING,                                                         \
        xr_byte_slice_common_prefix_core((left_data), (left_length), XR_ELEM_U8, (right_data),    \
                                         (right_length), XR_ELEM_U8, (ok)))
#include "../shared/xr_sync_core.h"
#include "../shared/xr_truthy_core.h"
#include "../shared/xr_type_identity_core.h"
/* Code-shape controls are pure compiler barriers over <stdint.h>: no runtime,
 * no libc, no allocation.  A freestanding target is exactly where an opaque
 * value and a compiler fence are needed most, so they belong in this core
 * rather than only in the hosted xrt.h. */
#include "xrt_codegen.h"
#include "xrt_method_symbols.h"

#if defined(__GNUC__) || defined(__clang__)
typedef uint8_t xr_v16u8 __attribute__((vector_size(16)));
typedef uint32_t xr_v4u32 __attribute__((vector_size(16)));
typedef uint64_t xr_v2u64 __attribute__((vector_size(16)));
#endif

#ifndef XR_FUNC
#define XR_FUNC extern
#endif
#ifndef XR_AINLINE
#if defined(__GNUC__) || defined(__clang__)
#define XR_AINLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define XR_AINLINE __forceinline
#else
#define XR_AINLINE inline
#endif
#endif
#ifndef XR_FORCEINLINE
#if defined(__GNUC__) || defined(__clang__)
#define XR_FORCEINLINE __attribute__((always_inline))
#elif defined(_MSC_VER)
#define XR_FORCEINLINE __forceinline
#else
#define XR_FORCEINLINE
#endif
#endif
#ifndef XR_NOINLINE
#if defined(__GNUC__) || defined(__clang__)
#define XR_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define XR_NOINLINE __declspec(noinline)
#else
#define XR_NOINLINE
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#define XR_LIKELY(x) __builtin_expect(!!(x), 1)
#define XR_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define XRT_INTERNAL __attribute__((visibility("hidden")))
#define XRT_COLD __attribute__((cold))
#define XRT_NORETURN __attribute__((noreturn))
#define XR_ASSUME_ALIGNED(p, n) __builtin_assume_aligned((p), (n))
#define XR_ASSUME(x)                                                                               \
    do {                                                                                           \
        if (!(x))                                                                                  \
            __builtin_unreachable();                                                               \
    } while (0)
#define XRT_FN_CONST __attribute__((const))
#define XRT_FN_PURE __attribute__((pure))
#define XRT_ATTR_SECTION(name) __attribute__((section(name)))
#define XRT_ATTR_WEAK __attribute__((weak))
#define XRT_ATTR_USED __attribute__((used))
#define XRT_ATTR_NAKED __attribute__((naked))
#if defined(__x86_64__) || defined(__i386__)
#if defined(__clang__)
#define XRT_TARGET_AVX2 __attribute__((target("avx2"), __min_vector_width__(256), flatten))
#else
#define XRT_TARGET_AVX2 __attribute__((target("avx2"), flatten))
#endif
/* Clang before 19 rejects the newer evex512 feature name and ignores the
 * entire target attribute, so retain the AVX-512F island with the portable
 * feature spelling on those providers. */
#if defined(__clang__) && __clang_major__ >= 19
#define XRT_TARGET_AVX512                                                                          \
    __attribute__((target("avx512f,evex512"), __min_vector_width__(512), flatten))
#elif defined(__clang__)
#define XRT_TARGET_AVX512 __attribute__((target("avx512f"), __min_vector_width__(512), flatten))
#else
#define XRT_TARGET_AVX512 __attribute__((target("avx512f"), flatten))
#endif
#else
#define XRT_TARGET_AVX2
#define XRT_TARGET_AVX512
#endif
#if defined(__arm__) || defined(__thumb__)
#define XRT_ATTR_INTERRUPT(abi) __attribute__((interrupt(abi)))
#else
#define XRT_ATTR_INTERRUPT(abi) __attribute__((interrupt))
#endif
#define XRT_RESTRICT __restrict__
#elif defined(_MSC_VER)
#define XR_LIKELY(x) (x)
#define XR_UNLIKELY(x) (x)
#define XRT_INTERNAL
#define XRT_COLD
#define XRT_NORETURN __declspec(noreturn)
#define XR_ASSUME_ALIGNED(p, n) (p)
#define XR_ASSUME(x) __assume(x)
#define XRT_FN_CONST
#define XRT_FN_PURE
#define XRT_ATTR_SECTION(name)
#define XRT_ATTR_WEAK
#define XRT_ATTR_USED
#define XRT_ATTR_NAKED
#define XRT_TARGET_AVX2
#define XRT_TARGET_AVX512
#define XRT_ATTR_INTERRUPT(abi)
#define XRT_RESTRICT __restrict
#else
#define XR_LIKELY(x) (x)
#define XR_UNLIKELY(x) (x)
#define XRT_INTERNAL
#define XRT_COLD
#define XRT_NORETURN
#define XR_ASSUME_ALIGNED(p, n) (p)
#define XR_ASSUME(x) ((void) 0)
#define XRT_FN_CONST
#define XRT_FN_PURE
#define XRT_ATTR_SECTION(name)
#define XRT_ATTR_WEAK
#define XRT_ATTR_USED
#define XRT_ATTR_NAKED
#define XRT_TARGET_AVX2
#define XRT_TARGET_AVX512
#define XRT_ATTR_INTERRUPT(abi)
#define XRT_RESTRICT
#endif

static inline int xrt_target_runtime_simd_bytes(void) {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    static _Atomic(int) cached_bytes = 0;
    int cached = atomic_load_explicit(&cached_bytes, memory_order_relaxed);
    if (cached != 0)
        return cached;
    unsigned eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0u), "c"(0u));
    if (eax < 7u)
        goto baseline;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1u), "c"(0u));
    if ((ecx & (1u << 27)) == 0 || (ecx & (1u << 28)) == 0)
        goto baseline;
    unsigned xcr0_lo, xcr0_hi;
    __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0u));
    (void) xcr0_hi;
    if ((xcr0_lo & 6u) != 6u)
        goto baseline;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7u), "c"(0u));
    cached = (ebx & (1u << 5)) != 0 && (ebx & (1u << 16)) != 0 && (xcr0_lo & 0xe6u) == 0xe6u ? 64
             : (ebx & (1u << 5)) != 0                                                        ? 32
                                                                                             : 16;
    atomic_store_explicit(&cached_bytes, cached, memory_order_relaxed);
    return cached;
baseline:
    atomic_store_explicit(&cached_bytes, 16, memory_order_relaxed);
    return 16;
#elif (defined(_M_X64) || defined(_M_IX86)) && defined(_MSC_VER)
    int regs[4];
    __cpuid(regs, 1);
    unsigned __int64 xcr0 = _xgetbv(0);
    if ((regs[2] & (1 << 27)) == 0 || (regs[2] & (1 << 28)) == 0 || (xcr0 & 6) != 6)
        return 16;
    __cpuidex(regs, 7, 0);
    return (regs[1] & (1 << 5)) != 0 && (regs[1] & (1 << 16)) != 0 && (xcr0 & 0xe6) == 0xe6 ? 64
           : (regs[1] & (1 << 5)) != 0                                                      ? 32
                                                                                            : 16;
#else
    return 16;
#endif
}

#if defined(__APPLE__)
#define XR_FFI_ASMNAME(s) "_" s
#else
#define XR_FFI_ASMNAME(s) s
#endif

#include "xray_value_abi.h"
#include "../shared/xr_cell_access_core.h"

#define XR_TAG_NULL 0
#define XR_TAG_BOOL 1
#define XR_TAG_RUNE 2
#define XR_TAG_I64 3
#define XR_TAG_F64 4
#define XR_TAG_PTR 5
#define XR_TAG_AGG_REF 6
#define XR_TAG_NOTFOUND 7
#define XR_TAG_STR 14
#define XR_TAG_ARRAY 15
#define XR_TAG_MAP 16
#define XR_TAG_STRBUF 17
#define XR_TAG_CLOSURE 18
#define XR_TAG_STR_ARC 19
#define XR_TAG_CELL 20
#define XR_TAG_TUPLE 21
#define XR_TAG_SET 22
#define XR_TAG_RANGE 23
#define XR_TAG_ENUM 24
#define XR_TAG_ITERATOR 25
#define XR_TAG_REGEX 26
#define XR_TAG_SYS_MUTEX 28
#define XR_TAG_SYS_RWLOCK 29
#define XR_TAG_SYS_CONDVAR 30
#define XR_TAG_SYS_BARRIER 31
#define XR_TAG_SYS_ONCE 32
#define XR_TAG_THREAD 33
#define XR_TAG_BUFFER 34
#define XR_TAG_BIGINT 35

#define XR_NATIVE_I64 0
#define XR_NATIVE_F64 1
#define XR_NATIVE_BOOL 2
#define XR_NATIVE_I8 3
#define XR_NATIVE_I16 4
#define XR_NATIVE_I32 5
#define XR_NATIVE_U8 6
#define XR_NATIVE_U16 7
#define XR_NATIVE_U32 8
#define XR_NATIVE_U64 9
#define XR_NATIVE_F32 10
#define XR_NATIVE_NESTED_AGGREGATE 11
#define XR_NATIVE_ARRAY 12
#define XR_NATIVE_STRING 13
#define XR_NATIVE_ARRAY_REF 14
#define XR_NATIVE_MAP_REF 15
#define XR_NATIVE_SET_REF 16
#define XR_NATIVE_VALUE 17
#define XR_NATIVE_ISIZE 18
#define XR_NATIVE_USIZE 19
#define XR_NATIVE_POINTER 20

#define XR_FROM_INT(x) ((XrValue) {.tag = XR_TAG_I64, .i = (int64_t) (x)})
#define XR_FROM_FLOAT(x) ((XrValue) {.tag = XR_TAG_F64, .f = (double) (x)})
#define XR_FROM_BOOL(x) ((XrValue) {.tag = XR_TAG_BOOL, .i = (x) ? 1 : 0})
#define XR_FROM_RUNE(cp) ((XrValue) {.tag = XR_TAG_RUNE, .i = (int64_t) (uint32_t) (cp)})
#define XR_NULL_VAL ((XrValue) {.tag = XR_TAG_NULL})
#define XR_TRUE_VAL ((XrValue) {.tag = XR_TAG_BOOL, .i = 1})
#define XR_FALSE_VAL ((XrValue) {.tag = XR_TAG_BOOL, .i = 0})

#define XR_TO_INT(v) ((v).i)
#define XR_TO_FLOAT(v) ((v).f)
#define XR_TO_BOOL(v) ((int) (v).i)
#define XR_TO_RUNE(v) ((uint32_t) (v).i)
#define XR_TO_BOOL(v) ((int) (v).i)

#define XR_IS_NULL(v) ((v).tag == XR_TAG_NULL)
#define XR_IS_BOOL(v) ((v).tag == XR_TAG_BOOL)
#define XR_IS_RUNE(v) ((v).tag == XR_TAG_RUNE)
#define XR_IS_INT(v) ((v).tag == XR_TAG_I64)
#define XR_IS_FLOAT(v) ((v).tag == XR_TAG_F64)
#define XR_IS_FALSE(v) ((v).tag == XR_TAG_BOOL && (v).i == 0)
#define XR_IS_NUM(v) (XR_IS_INT(v) || XR_IS_FLOAT(v))
#define XR_IS_STR(v) ((v).tag == XR_TAG_STR || (v).tag == XR_TAG_STR_ARC)
#define XR_IS_ARRAY(v) ((v).tag == XR_TAG_PTR && (v).heap_type == XR_TARRAY)
#define XR_IS_MAP(v) ((v).tag == XR_TAG_PTR && (v).heap_type == XR_TMAP)
#define XR_IS_SET(v) ((v).tag == XR_TAG_PTR && (v).heap_type == XR_TSET)
#define XR_IS_ARRAY_REF(v) ((v).tag == XR_TAG_AGG_REF && (v).ext != 0)
#define XR_ARRAY_REF_ELEM_TYPE(v) ((uint8_t) ((v).ext & 0xFF))
#define XR_ARRAY_REF_MAX_COUNT UINT32_C(0x00FFFFFF)
#define XR_ARRAY_REF_ELEM_COUNT(v) ((uint32_t) ((v).ext >> 8))

#define XRT_STR_LITERAL 0x1u

typedef struct {
    int64_t len;
    int64_t rune_len;
    uint32_t hash;
    uint32_t flags;
    char *data;
} xrt_str_t;

static inline xrt_str_t *xr_str_hdr(XrValue v) {
    return (xrt_str_t *) v.ptr;
}

static inline const char *xr_str_data(XrValue v) {
    return ((const xrt_str_t *) v.ptr)->data;
}

static inline char *xr_str_buf(XrValue v) {
    return ((xrt_str_t *) v.ptr)->data;
}

static inline int64_t xr_str_len(XrValue v) {
    return ((const xrt_str_t *) v.ptr)->len;
}

static inline int64_t xr_str_rune_len(XrValue v) {
    xrt_str_t *h = (xrt_str_t *) v.ptr;
    if (!h || !h->data || h->len <= 0)
        return 0;
    if (h->rune_len >= 0)
        return h->rune_len;
    const unsigned char *p = (const unsigned char *) h->data;
    const unsigned char *end = p + h->len;
    int64_t count = 0;
    while (p < end) {
        unsigned char b = *p;
        int64_t width = (b < 0x80u)              ? 1
                        : ((b & 0xE0u) == 0xC0u) ? 2
                        : ((b & 0xF0u) == 0xE0u) ? 3
                        : ((b & 0xF8u) == 0xF0u) ? 4
                                                 : 1;
        p += width <= end - p ? width : 1;
        count++;
    }
    if (!(h->flags & XRT_STR_LITERAL))
        h->rune_len = count;
    return count;
}

static inline XrValue xr_str_lit(const xrt_str_t *hdr) {
    XrValue r = {0};
    r.tag = XR_TAG_STR;
    r.ptr = (void *) hdr;
    return r;
}

static inline XrValue xr_str_value_from_ptr(void *ptr) {
    if (!ptr)
        return XR_NULL_VAL;
    const xrt_str_t *hdr = (const xrt_str_t *) ptr;
    XrValue r = {0};
    r.tag = (hdr->flags & XRT_STR_LITERAL) ? XR_TAG_STR : XR_TAG_STR_ARC;
    r.ptr = ptr;
    return r;
}

#define XRT_STR_LIT_DEF(name, s)                                                                   \
    static const xrt_str_t name = {(int64_t) sizeof(s) - 1, (int64_t) sizeof(s) - 1, 0,            \
                                   XRT_STR_LITERAL, (char *) (s)}

static inline XrValue xr_mkheap(void *p, uint16_t heap_type) {
    XrValue r = {0};
    r.tag = XR_TAG_PTR;
    r.heap_type = heap_type;
    r.ptr = p;
    return r;
}

static inline XrValue xr_mkptr(void *p, uint8_t tag) {
    switch (tag) {
        case XR_TAG_ARRAY:
            return xr_mkheap(p, XR_TARRAY);
        case XR_TAG_MAP:
            return xr_mkheap(p, XR_TMAP);
        case XR_TAG_SET:
            return xr_mkheap(p, XR_TSET);
        default:
            break;
    }
    XrValue r = {0};
    r.tag = tag;
    r.ptr = p;
    return r;
}

static inline XrValue xr_aggregate_ref(void *p, uint16_t storage_size) {
    XrValue r = {0};
    r.tag = XR_TAG_AGG_REF;
    r.heap_type = storage_size;
    r.ptr = p;
    return r;
}

static inline XrValue xr_array_ref(void *ptr, uint8_t elem_native_type, uint32_t elem_count) {
    XrValue r = {0};
    r.tag = XR_TAG_AGG_REF;
    r.ext = ((uint32_t) elem_count << 8) | elem_native_type;
    r.ptr = ptr;
    return r;
}

typedef struct XrAotEnumBox {
    XrObjHeader hdr;
    void *klass;
    const char *enum_name;
    const char *member_name;
    uint32_t member_index;
    uint32_t payload_count;
    uint32_t layout_id;
    XrValue payloads[];
} XrAotEnumBox;

static inline uint32_t xrt_enum_value_layout_id(XrValue v) {
    if (v.tag != XR_TAG_ENUM || !v.ptr)
        return 0;
    return ((const XrAotEnumBox *) v.ptr)->layout_id;
}

#define XR_AOT_ENUM_AGG_PAYLOAD_CAP 16u

typedef struct XrAotEnumAggregate {
    const char *enum_name;
    const char *member_name;
    int64_t tag;
    uint32_t payload_count;
    uint32_t layout_id;
    XrValue payloads[XR_AOT_ENUM_AGG_PAYLOAD_CAP];
} XrAotEnumAggregate;

static inline XrAotEnumAggregate xrt_enum_aggregate_zero(void) {
    XrAotEnumAggregate out;
    out.enum_name = NULL;
    out.member_name = NULL;
    out.tag = 0;
    out.payload_count = 0;
    out.layout_id = 0;
    out.payloads[0] = XR_NULL_VAL;
    out.payloads[1] = XR_NULL_VAL;
    out.payloads[2] = XR_NULL_VAL;
    out.payloads[3] = XR_NULL_VAL;
    out.payloads[4] = XR_NULL_VAL;
    out.payloads[5] = XR_NULL_VAL;
    out.payloads[6] = XR_NULL_VAL;
    out.payloads[7] = XR_NULL_VAL;
    out.payloads[8] = XR_NULL_VAL;
    out.payloads[9] = XR_NULL_VAL;
    out.payloads[10] = XR_NULL_VAL;
    out.payloads[11] = XR_NULL_VAL;
    out.payloads[12] = XR_NULL_VAL;
    out.payloads[13] = XR_NULL_VAL;
    out.payloads[14] = XR_NULL_VAL;
    out.payloads[15] = XR_NULL_VAL;
    return out;
}

static inline XrAotEnumAggregate
xrt_enum_aggregate_make(uint32_t layout_id, int64_t tag, uint32_t payload_count,
                        const char *enum_name, const char *member_name, const XrValue *payloads) {
    XrAotEnumAggregate out = xrt_enum_aggregate_zero();
    out.enum_name = enum_name;
    out.member_name = member_name;
    out.tag = tag;
    out.payload_count = payload_count;
    out.layout_id = layout_id;
    uint32_t limit =
        payload_count < XR_AOT_ENUM_AGG_PAYLOAD_CAP ? payload_count : XR_AOT_ENUM_AGG_PAYLOAD_CAP;
    for (uint32_t i = 0; i < limit; i++)
        out.payloads[i] = payloads ? payloads[i] : XR_NULL_VAL;
    return out;
}

/* C compound-literal arrays and C++ temporary arrays have different address
 * rules.  Keep the payload alive for the complete make call in either mode. */
#if defined(__cplusplus)
#define XRT_ENUM_AGGREGATE_MAKE(layout_id, tag, payload_count, enum_name, member_name, ...)        \
    ([&]() {                                                                                       \
        const XrValue _xrt_enum_payloads[(payload_count)] = {__VA_ARGS__};                         \
        return xrt_enum_aggregate_make((layout_id), (tag), (payload_count), (enum_name),           \
                                       (member_name), _xrt_enum_payloads);                         \
    }())
#else
#define XRT_ENUM_AGGREGATE_MAKE(layout_id, tag, payload_count, enum_name, member_name, ...)        \
    xrt_enum_aggregate_make((layout_id), (tag), (payload_count), (enum_name), (member_name),       \
                            (const XrValue[(payload_count)]) {__VA_ARGS__})
#endif

typedef struct xrt_closure {
    const XrAotCallableDesc *callable;
    int nupvals;
    XrValue upvals[];
} xrt_closure_t;

typedef void *XrtContext;
typedef struct XrAotRuntime XrAotRuntime;
struct XrCoroutine;

typedef enum {
    XR_AOT_RUN_DONE = 0,
    XR_AOT_RUN_BLOCKED,
    XR_AOT_RUN_YIELD,
    XR_AOT_RUN_SPAWN_CHILD,
    XR_AOT_RUN_ERROR,
    XR_AOT_RUN_CANCELLED,
    XR_AOT_RUN_GEN_YIELD
} XrAotRunKind;

typedef struct XrAotResult {
    XrAotRunKind kind;
    XrValue value;
    XrValue error;
    struct XrCoroutine *child;
    bool error_is_value;
} XrAotResult;

typedef struct XrAotVmHostOps {
    XrValue (*get_builtin)(void *host, int32_t index);
    void *reserved[15];
} XrAotVmHostOps;

typedef struct XrAotContext {
    XrAotRuntime *runtime;
    struct XrCoroutine *coro;
    const XrAotVmHostOps *vm_host_ops;
    void *vm_host;
    void *worker;
} XrAotContext;

/* Native value structs still receive stable, non-zero type identities during
 * module initialization.  Freestanding code has no dynamic class table: the
 * verified native path resolves constructors and methods statically, so only
 * monotonic identity assignment is required here and no allocator is pulled
 * into an otherwise no-heap binary. */
typedef void (*XrtDestructor)(void *obj);
typedef void (*XrtStoragePromoter)(void *obj, uint8_t storage_mode);
typedef XrValue (*XrtMethodFn)(void);

#ifdef XRT_IMPL
XRT_INTERNAL uint16_t xrt_freestanding_type_count = 1;
#else
extern XRT_INTERNAL uint16_t xrt_freestanding_type_count;
#endif

static inline uint16_t xrt_type_register_hot(uint16_t parent_id, XrtMethodFn *vtable,
                                             int vtable_size, XrtDestructor dtor,
                                             XrtStoragePromoter promote_storage,
                                             uint32_t inst_size) {
    (void) parent_id;
    (void) vtable;
    (void) vtable_size;
    (void) dtor;
    (void) promote_storage;
    (void) inst_size;
    if (XR_UNLIKELY(xrt_freestanding_type_count == UINT16_MAX)) {
        XR_ASSUME(0);
        return 0;
    }
    return xrt_freestanding_type_count++;
}

#if defined(XRAY_TARGET_RUNTIME_PROVIDER)
#include "xrt_provider_abi.h"
#endif

#ifdef XRT_IMPL
XrValue xrt_pending_error = {.tag = XR_TAG_NULL};
XrAotEnumAggregate xrt_pending_enum_error = {0};
int xrt_pending_enum_error_active = 0;
#else
extern XrValue xrt_pending_error;
extern XrAotEnumAggregate xrt_pending_enum_error;
extern int xrt_pending_enum_error_active;
#endif

static inline int xrt_has_pending_error(void) {
    return !XR_IS_NULL(xrt_pending_error) || xrt_pending_enum_error_active;
}

XRT_COLD XRT_NORETURN void xr_hook_panic(const char *message, size_t len);
void xr_hook_write(const char *bytes, size_t len);
void *xr_hook_alloc(size_t size, size_t align);
void xr_hook_free(void *ptr);
void *xr_hook_page_alloc(size_t size, int64_t prot);
bool xr_hook_page_protect(void *ptr, size_t size, int64_t prot);
bool xr_hook_page_free(void *ptr, size_t size);

typedef struct xrt_buffer_object {
    void *data;
    int64_t length;
    size_t align;
} xrt_buffer_object_t;

#define XRT_FREESTANDING_OBJECT_ALIGN 16u
#define XRT_FREESTANDING_DEFAULT_ALLOC_ALIGN ((size_t) sizeof(void *))
#define XRT_ARC_KIND_BUFFER 10u
#define XRT_ARC_HDR(p) ((XrObjHeader *) ((char *) (p) - sizeof(XrObjHeader)))

#define XRT_MEM_PROT_NONE 0
#define XRT_MEM_PROT_READ 1
#define XRT_MEM_PROT_WRITE 2
#define XRT_MEM_PROT_EXEC 4

static inline bool xrt_freestanding_is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

static inline size_t xrt_freestanding_align_up(size_t value, size_t align) {
    if (align == 0)
        return value;
    return (value + align - 1u) & ~(align - 1u);
}

static inline void *xrt_freestanding_alloc_bytes(size_t size, size_t align) {
    if (size == 0)
        return NULL;
    if (align == 0)
        align = XRT_FREESTANDING_DEFAULT_ALLOC_ALIGN;
    if (!xrt_freestanding_is_power_of_two(align))
        return NULL;
    return xr_hook_alloc(size, align);
}

static inline XrValue xrt_buffer_box(xrt_buffer_object_t *buf) {
    return buf ? xr_mkptr(buf, XR_TAG_BUFFER) : XR_NULL_VAL;
}

static inline int xrt_buffer_is(XrValue value) {
    return value.tag == XR_TAG_BUFFER && value.ptr != NULL;
}

static inline xrt_buffer_object_t *xrt_buffer_obj_ptr(XrValue value) {
    return xrt_buffer_is(value) ? (xrt_buffer_object_t *) value.ptr : NULL;
}

static inline int64_t xrt_buffer_length(XrValue value) {
    xrt_buffer_object_t *buf = xrt_buffer_obj_ptr(value);
    return buf ? buf->length : 0;
}

static inline void xrt_buffer_free_data(void *data) {
    if (data)
        xr_hook_free(data);
}

static inline void xrt_buffer_destroy_builtin(void *obj) {
    xrt_buffer_object_t *buf = (xrt_buffer_object_t *) obj;
    if (!buf)
        return;
    xrt_buffer_free_data(buf->data);
    buf->data = NULL;
    buf->length = 0;
    buf->align = 0;
}

static inline void *xrt_freestanding_arc_alloc(size_t obj_size) {
    size_t rounded = xrt_freestanding_align_up(obj_size, XRT_FREESTANDING_OBJECT_ALIGN);
    size_t total = sizeof(XrObjHeader) + rounded;
    XrObjHeader *hdr =
        (XrObjHeader *) xrt_freestanding_alloc_bytes(total, XRT_FREESTANDING_OBJECT_ALIGN);
    if (!hdr)
        return NULL;
    memset(hdr, 0, total);
    hdr->extra = XR_OBJ_HAS_DTOR;
    atomic_store_explicit(&hdr->refcount, XR_RC_INIT, memory_order_relaxed);
    hdr->objsize = total > UINT32_MAX ? UINT32_MAX : (uint32_t) total;
    hdr->_rsv = XRT_ARC_KIND_BUFFER;
    return (char *) hdr + sizeof(XrObjHeader);
}

static inline void xrt_retain(XrValue v) {
    if (!xrt_buffer_is(v))
        return;
    XrObjHeader *hdr = XRT_ARC_HDR(v.ptr);
    int32_t rc = atomic_load_explicit(&hdr->refcount, memory_order_relaxed);
    if (rc == XR_RC_STICKY)
        return;
    atomic_fetch_add_explicit(&hdr->refcount, 1, memory_order_relaxed);
}

static inline XrValue xrt_retain_identity(XrValue value) {
    xrt_retain(value);
    return value;
}

static inline bool xrt_rc_claim_release_last(XrObjHeader *hdr) {
    if (!hdr)
        return false;
    for (;;) {
        int32_t rc = atomic_load_explicit(&hdr->refcount, memory_order_acquire);
        if (rc == XR_RC_STICKY)
            return false;
        if (rc > 0) {
            int32_t next = rc - 1;
            if (atomic_compare_exchange_weak_explicit(&hdr->refcount, &rc, next,
                                                      memory_order_acq_rel, memory_order_acquire))
                return false;
            continue;
        }
        if (rc == 0) {
            int32_t next = XR_RC_STICKY;
            if (atomic_compare_exchange_weak_explicit(&hdr->refcount, &rc, next,
                                                      memory_order_acq_rel, memory_order_acquire))
                return true;
            continue;
        }
        return false;
    }
}

static inline void xrt_release(XrValue v) {
    if (!xrt_buffer_is(v))
        return;
    XrObjHeader *hdr = XRT_ARC_HDR(v.ptr);
    if (!xrt_rc_claim_release_last(hdr))
        return;
    xrt_buffer_destroy_builtin(v.ptr);
    xr_hook_free(hdr);
}

typedef struct xrt_cell {
    XR_CELL_ABI_FIELDS;
} xrt_cell_t;

static inline XrValue xrt_cell_access_get(XrValue cell_value) {
    if (cell_value.tag != XR_TAG_CELL || !cell_value.ptr)
        return cell_value;
    xrt_cell_t *cell = (xrt_cell_t *) cell_value.ptr;
    return XR_CELL_ACCESS_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_CELL_ACCESS_HI, XR_SEM_OWNER_ID_SHARED_CELL_ACCESS_LO,
        XR_SEM_CONSUMER_AOT_FREESTANDING, xr_cell_access_load_core(&cell->value));
}

static inline void xrt_cell_access_set(XrValue cell_value, XrValue value) {
    if (cell_value.tag != XR_TAG_CELL || !cell_value.ptr)
        return;
    xrt_cell_t *cell = (xrt_cell_t *) cell_value.ptr;
    XrValue old = XR_CELL_ACCESS_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_CELL_ACCESS_HI, XR_SEM_OWNER_ID_SHARED_CELL_ACCESS_LO,
        XR_SEM_CONSUMER_AOT_FREESTANDING,
        xr_cell_access_replace_core(&cell->value, value));
    xrt_release(old);
}

static inline void xrt_enum_aggregate_retain(XrAotEnumAggregate value) {
    uint32_t limit = value.payload_count < XR_AOT_ENUM_AGG_PAYLOAD_CAP
                         ? value.payload_count
                         : XR_AOT_ENUM_AGG_PAYLOAD_CAP;
    for (uint32_t i = 0; i < limit; i++)
        xrt_retain(value.payloads[i]);
}

static inline void xrt_enum_aggregate_release(XrAotEnumAggregate value) {
    uint32_t limit = value.payload_count < XR_AOT_ENUM_AGG_PAYLOAD_CAP
                         ? value.payload_count
                         : XR_AOT_ENUM_AGG_PAYLOAD_CAP;
    for (uint32_t i = 0; i < limit; i++)
        xrt_release(value.payloads[i]);
}

static inline size_t xrt_freestanding_strlen(const char *s) {
    size_t len = 0;
    if (!s)
        return 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

static inline XRT_COLD XRT_NORETURN void xrt_freestanding_trap(const char *message) {
    if (!message)
        message = "panic";
    xr_hook_panic(message, xrt_freestanding_strlen(message));
#if defined(__GNUC__) || defined(__clang__)
    __builtin_unreachable();
#else
    for (;;) {
    }
#endif
}

#ifndef XR_ERR_CMP_CONST_ASSIGN
#define XR_ERR_CMP_CONST_ASSIGN 303
#endif
#ifndef XR_ERR_TYPE_MISMATCH
#define XR_ERR_TYPE_MISMATCH 404
#endif
#ifndef XR_ERR_INDEX_OUT_OF_BOUNDS
#define XR_ERR_INDEX_OUT_OF_BOUNDS 430
#endif
#ifndef XR_ERR_OVERFLOW
#define XR_ERR_OVERFLOW 422
#endif
#ifndef XR_ERR_OUT_OF_MEMORY
#define XR_ERR_OUT_OF_MEMORY 441
#endif
#ifndef XR_ERR_INVALID_ARG_TYPE
#define XR_ERR_INVALID_ARG_TYPE 451
#endif

static inline XRT_COLD XRT_NORETURN void xrt_throw_error(int code, const char *message) {
    (void) code;
    xrt_freestanding_trap(message && message[0] ? message : "freestanding runtime error");
}

static inline int64_t xrt_enum_metadata_access_variant_at(int64_t count, int64_t index) {
    XrEnumMetadataResult result = XR_ENUM_METADATA_ACCESS_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_HI,
        XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_LO, XR_SEM_CONSUMER_AOT_FREESTANDING,
        xr_enum_metadata_variant_at_core(count, index));
    if (result.status != XR_ENUM_METADATA_OK)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_ENUM_VARIANT_INDEX_OOB_MSG);
    return result.value;
}

static inline int64_t xrt_enum_metadata_access_payload_at(uint64_t view, int64_t index) {
    XrEnumMetadataResult result = XR_ENUM_METADATA_ACCESS_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_HI,
        XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_LO, XR_SEM_CONSUMER_AOT_FREESTANDING,
        xr_enum_metadata_payload_at_core(view, index));
    if (result.status != XR_ENUM_METADATA_OK)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_ENUM_PAYLOAD_INDEX_OOB_MSG);
    return result.value;
}

static inline int64_t xrt_numeric_float_to_int_or_throw(double source, uint8_t target_rep,
                                                        uint8_t pointer_bits) {
    int64_t result = 0;
    if (!xr_numeric_float_to_int(source, target_rep, pointer_bits, &result))
        xrt_throw_error(XR_ERR_OVERFLOW, XR_ERROR_CORE_NUMERIC_CONVERSION_RANGE_MSG);
    return result;
}

static inline XRT_COLD XRT_NORETURN void xrt_enum_aggregate_shape_fail(const char *what,
                                                                       const char *enum_name) {
    (void) enum_name;
    xrt_freestanding_trap(what && what[0] ? what : "AOT enum aggregate shape mismatch");
}

static inline void xrt_enum_aggregate_check_layout(uint32_t actual_layout_id,
                                                   uint32_t expected_layout_id,
                                                   const char *enum_name) {
    if (actual_layout_id != 0 && expected_layout_id != 0 && actual_layout_id != expected_layout_id)
        xrt_enum_aggregate_shape_fail("enum layout id mismatch", enum_name);
}

static inline void xrt_enum_aggregate_check_payload_count(uint32_t layout_id, uint32_t actual,
                                                          uint32_t expected,
                                                          const char *enum_name) {
    if (layout_id != 0 && actual != expected)
        xrt_enum_aggregate_shape_fail("enum payload count mismatch", enum_name);
}

static inline void xrt_enum_aggregate_check_payload_type(uint32_t layout_id, int ok,
                                                         const char *enum_name) {
    if (layout_id != 0 && !ok)
        xrt_enum_aggregate_shape_fail("enum payload type mismatch", enum_name);
}

static inline void xrt_enum_aggregate_check_known_tag(uint32_t layout_id, const char *enum_name) {
    if (layout_id != 0)
        xrt_enum_aggregate_shape_fail("enum tag mismatch", enum_name);
}

static inline XRT_COLD XRT_NORETURN void xrt_index_oob(int64_t idx, int64_t length) {
    (void) idx;
    (void) length;
    xrt_freestanding_trap("array index out of range");
}

static inline XRT_COLD XRT_NORETURN void xrt_fixed_index_oob(int64_t idx, int64_t length) {
    (void) idx;
    (void) length;
    xrt_freestanding_trap("fixed array index out of range");
}

static inline size_t xrt_value_native_type_size(uint8_t native_type) {
    switch (native_type) {
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
        case XR_NATIVE_BOOL:
            return 1;
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
            return 2;
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
        case XR_NATIVE_F32:
            return 4;
        case XR_NATIVE_ISIZE:
            return sizeof(ptrdiff_t);
        case XR_NATIVE_USIZE:
            return sizeof(size_t);
        case XR_NATIVE_POINTER:
            return sizeof(void *);
        case XR_NATIVE_VALUE:
            return sizeof(XrValue);
        default:
            return 8;
    }
}

#if defined(_MSC_VER)
#define XRT_SPAN_ALIGN __declspec(align(8))
#else
#define XRT_SPAN_ALIGN __attribute__((aligned(8)))
#endif
typedef struct XRT_SPAN_ALIGN {
    void *data;
#if UINTPTR_MAX == UINT32_MAX
    uint32_t _abi_padding;
#endif
    int64_t length;
} xr_span_t;
#undef XRT_SPAN_ALIGN

_Static_assert(sizeof(xr_span_t) == 16, "release Slice ABI must be data + length");
#if !defined(_MSC_VER)
_Static_assert(_Alignof(xr_span_t) == 8, "release Slice ABI must remain 8-byte aligned");
#endif

static inline int64_t xrt_expect_int_arg(XrValue value) {
    if (XR_UNLIKELY(!XR_IS_INT(value)))
        xrt_freestanding_trap("E0404: expected int argument");
    return XR_TO_INT(value);
}

static inline int64_t xrt_expect_bool_arg(XrValue value) {
    if (XR_UNLIKELY(!XR_IS_BOOL(value)))
        xrt_freestanding_trap("E0404: expected bool argument");
    return XR_TO_INT(value);
}

static inline int64_t xrt_expect_rune_arg(XrValue value) {
    if (XR_UNLIKELY(!XR_IS_RUNE(value)))
        xrt_freestanding_trap("E0404: expected rune argument");
    return (int64_t) XR_TO_RUNE(value);
}

static inline XrValue xrt_span_to_value_ref(xr_span_t *span) {
    XrValue out = {0};
    out.tag = XR_TAG_AGG_REF;
    out.heap_type = UINT16_MAX;
    out.ptr = span;
    return out;
}

static inline XrValue xrt_span_box_value(xr_span_t span) {
    static _Thread_local xr_span_t slots[8];
    static _Thread_local unsigned cursor;
    xr_span_t *slot = &slots[cursor++ & 7u];
    *slot = span;
    return xrt_span_to_value_ref(slot);
}

static inline xr_span_t xrt_span_empty(void) {
    return (xr_span_t) {.data = NULL, .length = 0};
}

static inline xr_span_t xrt_span_from_value_ref(XrValue value) {
    if (value.tag == XR_TAG_AGG_REF && value.ext == 0 && value.heap_type == UINT16_MAX && value.ptr)
        return *(const xr_span_t *) value.ptr;
    xrt_throw_error(XR_ERR_TYPE_MISMATCH, "expected Slice value");
    return xrt_span_empty();
}

static inline void xrt_array_normalize_slice(int64_t len, int64_t *start, int64_t *end) {
    if (*start < 0)
        *start += len;
    if (*end < 0)
        *end += len;
    if (*start < 0)
        *start = 0;
    if (*end < 0)
        *end = 0;
    if (*start > len)
        *start = len;
    if (*end > len)
        *end = len;
    if (*start > *end)
        *start = *end;
}

static inline xr_span_t xrt_span_from_array_slice(XrValue arr, int64_t start, int64_t end) {
    if (!XR_IS_ARRAY_REF(arr))
        xrt_freestanding_trap("freestanding span slice supports only fixed arrays");
    uint8_t native_type = XR_ARRAY_REF_ELEM_TYPE(arr);
    int64_t len = XR_ARRAY_REF_ELEM_COUNT(arr);
    xrt_array_normalize_slice(len, &start, &end);
    int64_t count = end - start;
    if (count < 0)
        count = 0;
    uint8_t elem_size = (uint8_t) xrt_value_native_type_size(native_type);
    xr_span_t out = {0};
    out.data = (count > 0 && arr.ptr)
                   ? (void *) ((uint8_t *) arr.ptr + (size_t) start * (size_t) elem_size)
                   : arr.ptr;
    out.length = count;
    return out;
}

static inline xr_span_t xrt_span_from_span_slice(xr_span_t src, int64_t start, int64_t end,
                                                 uint16_t elem_size) {
    xrt_array_normalize_slice(src.length, &start, &end);
    int64_t count = end - start;
    if (count < 0)
        count = 0;
    xr_span_t out = src;
    out.data = (count > 0 && src.data)
                   ? (void *) ((uint8_t *) src.data + (size_t) start * (size_t) elem_size)
                   : src.data;
    out.length = count;
    return out;
}

static inline int64_t xrt_byte_slice_compare_checked_raw(xr_span_t left, xr_span_t right) {
    bool ok = false;
    int64_t ordering = xrt_byte_slice_compare_semantics(
        left.data, left.length, right.data, right.length, &ok);
    if (!ok)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_COMPARE_NO_DATA_MSG);
    return ordering;
}

static inline xr_span_t xrt_byte_slice_fill_checked_raw(xr_span_t span, int64_t value) {
    if (!xrt_byte_slice_fill_semantics(span.data, span.length, XR_ELEM_U8, value))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_FILL_OOB_MSG);
    return span;
}

static inline xr_span_t xrt_byte_slice_copy_checked_raw(xr_span_t dst, xr_span_t src) {
    if (!xrt_byte_slice_copy_semantics(dst.data, dst.length, src.data, src.length))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_COPY_OOB_MSG);
    return dst;
}

static inline xr_span_t xrt_byte_slice_repeat_from_checked_raw(xr_span_t span, int64_t dst_offset,
                                                               int64_t distance, int64_t count) {
    if (!xrt_byte_slice_repeat_semantics(span.data, span.length, dst_offset, distance, count))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_REPEAT_OOB_MSG);
    return span;
}

static inline int64_t xrt_byte_slice_common_prefix_checked_raw(xr_span_t left, xr_span_t right) {
    bool ok = false;
    int64_t prefix = xrt_byte_slice_common_prefix_semantics(
        left.data, left.length, right.data, right.length, &ok);
    if (!ok)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_COMMON_PREFIX_NO_DATA_MSG);
    return prefix;
}

static inline xr_span_t xrt_span_copy_checked_raw(xr_span_t dst, xr_span_t src,
                                                  uint16_t elem_size) {
    XrPodSliceStatus status = xrt_pod_slice_copy_semantics(
        dst.data, dst.length, elem_size, src.data, src.length, elem_size);
    if (status == XR_POD_SLICE_INVALID_LAYOUT)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        "Slice.copyFrom(src) requires static element layout");
    if (status == XR_POD_SLICE_BYTE_LENGTH_OVERFLOW)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice.copyFrom(src) byte length overflow");
    if (status != XR_POD_SLICE_OK)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice.copyFrom(src) range out of bounds");
    return dst;
}

static inline xr_span_t xrt_span_fill_checked_raw(xr_span_t span, uint16_t elem_size,
                                                  XrPodSliceFillKind kind,
                                                  XrPodSliceFillValue value) {
    XrPodSliceStatus status =
        xrt_pod_slice_fill_semantics(span.data, span.length, elem_size, kind, value);
    if (status == XR_POD_SLICE_INVALID_LAYOUT)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, "Slice.fill(value) element layout mismatch");
    if (status == XR_POD_SLICE_BYTE_LENGTH_OVERFLOW)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice.fill(value) byte length overflow");
    if (status != XR_POD_SLICE_OK)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice.fill(value) range out of bounds");
    return span;
}

static inline int64_t xrt_span_compare_checked_raw(xr_span_t left, xr_span_t right,
                                                   uint16_t elem_size) {
    XrPodSliceCompareResult result = xrt_pod_slice_compare_semantics(
        left.data, left.length, elem_size, right.data, right.length, elem_size);
    if (result.status == XR_POD_SLICE_INVALID_LAYOUT)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        "Slice.compare(other) requires static element layout");
    if (result.status == XR_POD_SLICE_BYTE_LENGTH_OVERFLOW)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice.compare(other) byte length overflow");
    if (result.status != XR_POD_SLICE_OK)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, "Slice.compare(other) span has no data");
    return result.ordering;
}

static inline xr_span_t xrt_pod_slice_view_checked_raw(
    xr_span_t span, XrPodSliceViewKind kind, uint16_t source_elem_size, bool source_has_layout,
    uint16_t target_elem_size, uint16_t target_expected_elem_size, uint16_t target_alignment,
    bool target_layout_valid, bool target_is_aggregate) {
    XrPodSliceViewResult result = xrt_pod_slice_view_semantics(
        kind, span.data, span.length, source_elem_size, source_has_layout, target_elem_size,
        target_expected_elem_size, target_alignment, target_layout_valid, target_is_aggregate);
    if (kind == XR_POD_SLICE_VIEW_AS_BYTES &&
        result.status == XR_POD_SLICE_VIEW_INVALID_SOURCE_LAYOUT)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, "Slice.asBytes() requires POD Slice element type");
    if (kind == XR_POD_SLICE_VIEW_AS_BYTES && result.status != XR_POD_SLICE_VIEW_OK)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice.asBytes() byte length overflow");
    if (result.status == XR_POD_SLICE_VIEW_INVALID_TARGET_LAYOUT)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_REQUIRES_POD_MSG);
    if (result.status == XR_POD_SLICE_VIEW_TARGET_SIZE_MISMATCH)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_METADATA_MISMATCH_MSG);
    if (result.status == XR_POD_SLICE_VIEW_BYTE_LENGTH_OVERFLOW)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS,
                        XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_OVERFLOW_MSG);
    if (result.status == XR_POD_SLICE_VIEW_LENGTH_NOT_DIVISIBLE)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS,
                        XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_DIVISIBLE_MSG);
    if (result.status != XR_POD_SLICE_VIEW_OK)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_MISALIGNED_MSG);
    return (xr_span_t) {.data = result.data, .length = result.length};
}

typedef struct XrArrayCoreRange {
    int64_t start;
    int64_t end;
    int64_t count;
} XrArrayCoreRange;

static inline XrArrayCoreRange xr_array_core_slice_range(int64_t length, int64_t start,
                                                         int64_t end) {
    if (length < 0)
        length = 0;
    if (start < 0)
        start += length;
    if (end < 0)
        end += length;
    if (start < 0)
        start = 0;
    if (start > length)
        start = length;
    if (end < 0)
        end = 0;
    if (end > length)
        end = length;
    if (start > end)
        start = end;
    return (XrArrayCoreRange) {start, end, end - start};
}

static inline bool xr_array_core_memory_ranges_overlap(const void *a, int64_t a_len, const void *b,
                                                       int64_t b_len) {
    if (!a || !b || a_len <= 0 || b_len <= 0)
        return false;
    uintptr_t a_begin = (uintptr_t) a;
    uintptr_t b_begin = (uintptr_t) b;
    uintptr_t a_end = a_begin + (uintptr_t) a_len;
    uintptr_t b_end = b_begin + (uintptr_t) b_len;
    if (a_end < a_begin || b_end < b_begin)
        return true;
    return a_begin < b_end && b_begin < a_end;
}

static inline void xr_array_core_copy_or_move_bytes(void *dst, const void *src, int64_t count) {
    if (count <= 0)
        return;
    if (xr_array_core_memory_ranges_overlap(dst, count, src, count))
        memmove(dst, src, (size_t) count);
    else
        xr_raw_memory_copy_nonoverlap(dst, src, count);
}

static inline void xr_array_core_bytes_repeat_copy(void *data, int64_t dst_offset, int64_t distance,
                                                   int64_t count) {
    xr_byte_slice_repeat_unchecked(data, dst_offset, distance, count);
}

static inline int64_t xrt_byte_slice_load_u16_unchecked_raw(xr_span_t span, int64_t off,
                                                            int64_t endian) {
    return (int64_t) xrt_byte_slice_scalar_eval(
        xr_byte_slice_scalar_load_u16_unchecked(span.data, off, endian));
}

static inline int64_t xrt_byte_slice_load_u32_unchecked_raw(xr_span_t span, int64_t off,
                                                            int64_t endian) {
    return (int64_t) xrt_byte_slice_scalar_eval(
        xr_byte_slice_scalar_load_u32_unchecked(span.data, off, endian));
}

static inline int64_t xrt_byte_slice_load_u64_unchecked_raw(xr_span_t span, int64_t off,
                                                            int64_t endian) {
    return (int64_t) xrt_byte_slice_scalar_eval(
        xr_byte_slice_scalar_load_u64_unchecked(span.data, off, endian));
}

static inline void xrt_byte_slice_store_u16_unchecked_raw(xr_span_t span, int64_t off,
                                                          uint16_t value, int64_t endian) {
    xrt_byte_slice_scalar_eval(
        xr_byte_slice_scalar_store_u16_unchecked(span.data, off, value, endian));
}

static inline void xrt_byte_slice_store_u32_unchecked_raw(xr_span_t span, int64_t off,
                                                          uint32_t value, int64_t endian) {
    xrt_byte_slice_scalar_eval(
        xr_byte_slice_scalar_store_u32_unchecked(span.data, off, value, endian));
}

static inline void xrt_byte_slice_store_u64_unchecked_raw(xr_span_t span, int64_t off,
                                                          uint64_t value, int64_t endian) {
    xrt_byte_slice_scalar_eval(
        xr_byte_slice_scalar_store_u64_unchecked(span.data, off, value, endian));
}

static inline int64_t xrt_byte_slice_load_u16_le_unchecked_raw(xr_span_t span, int64_t off) {
    return (int64_t) xrt_byte_slice_scalar_eval(
        xr_byte_slice_scalar_load_u16_unchecked(span.data, off, XR_ENDIAN_LE));
}

static inline int64_t xrt_byte_slice_load_u32_le_unchecked_raw(xr_span_t span, int64_t off) {
    return (int64_t) xrt_byte_slice_scalar_eval(
        xr_byte_slice_scalar_load_u32_unchecked(span.data, off, XR_ENDIAN_LE));
}

static inline int64_t xrt_byte_slice_load_u64_le_unchecked_raw(xr_span_t span, int64_t off) {
    return (int64_t) xrt_byte_slice_scalar_eval(
        xr_byte_slice_scalar_load_u64_unchecked(span.data, off, XR_ENDIAN_LE));
}

static inline int64_t xr_value_to_int64_coerce(XrValue v) {
    if (XR_IS_INT(v) || XR_IS_RUNE(v) || XR_IS_BOOL(v))
        return v.i;
    if (XR_IS_FLOAT(v))
        return (int64_t) v.f;
    return 0;
}

static inline double xr_value_to_f64_coerce(XrValue v) {
    if (XR_IS_FLOAT(v))
        return v.f;
    if (XR_IS_INT(v) || XR_IS_RUNE(v) || XR_IS_BOOL(v))
        return (double) v.i;
    return 0.0;
}

static inline XrValue xrt_fixed_array_get(void *base, uint8_t native_type, int64_t idx) {
    uint8_t *p = (uint8_t *) base + (size_t) idx * xrt_value_native_type_size(native_type);
    switch (native_type) {
        case XR_NATIVE_F32:
            return XR_FROM_FLOAT((double) *(float *) p);
        case XR_NATIVE_F64:
            return XR_FROM_FLOAT(*(double *) p);
        case XR_NATIVE_BOOL:
            return *(uint8_t *) p ? XR_TRUE_VAL : XR_FALSE_VAL;
        case XR_NATIVE_VALUE:
            return *(XrValue *) p;
        case XR_NATIVE_I8:
            return XR_FROM_INT((int64_t) *(int8_t *) p);
        case XR_NATIVE_I16:
            return XR_FROM_INT((int64_t) *(int16_t *) p);
        case XR_NATIVE_I32:
            return XR_FROM_INT((int64_t) *(int32_t *) p);
        case XR_NATIVE_U8:
            return XR_FROM_INT((int64_t) *(uint8_t *) p);
        case XR_NATIVE_U16:
            return XR_FROM_INT((int64_t) *(uint16_t *) p);
        case XR_NATIVE_U32:
            return XR_FROM_INT((int64_t) *(uint32_t *) p);
        case XR_NATIVE_U64:
            return XR_FROM_INT((int64_t) *(uint64_t *) p);
        case XR_NATIVE_ISIZE:
            return XR_FROM_INT((int64_t) *(ptrdiff_t *) p);
        case XR_NATIVE_USIZE:
            return XR_FROM_INT((int64_t) *(size_t *) p);
        default:
            return XR_FROM_INT(*(int64_t *) p);
    }
}

static inline void xrt_fixed_array_set(void *base, uint8_t native_type, int64_t idx,
                                       XrValue value) {
    uint8_t *p = (uint8_t *) base + (size_t) idx * xrt_value_native_type_size(native_type);
    switch (native_type) {
        case XR_NATIVE_F32:
            *(float *) p = (float) xr_value_to_f64_coerce(value);
            break;
        case XR_NATIVE_F64:
            *(double *) p = xr_value_to_f64_coerce(value);
            break;
        case XR_NATIVE_BOOL:
            *(uint8_t *) p = (uint8_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_VALUE:
            *(XrValue *) p = value;
            break;
        case XR_NATIVE_I8:
            *(int8_t *) p = (int8_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_I16:
            *(int16_t *) p = (int16_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_I32:
            *(int32_t *) p = (int32_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_U8:
            *(uint8_t *) p = (uint8_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_U16:
            *(uint16_t *) p = (uint16_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_U32:
            *(uint32_t *) p = (uint32_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_U64:
            *(uint64_t *) p = (uint64_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_ISIZE:
            *(ptrdiff_t *) p = (ptrdiff_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_USIZE:
            *(size_t *) p = (size_t) xr_value_to_int64_coerce(value);
            break;
        default:
            *(int64_t *) p = xr_value_to_int64_coerce(value);
            break;
    }
}

static inline XrValue xrt_index_get(XrValue obj, XrValue key) {
    if (XR_IS_ARRAY_REF(obj) && XR_IS_INT(key)) {
        int64_t idx = key.i;
        uint32_t count = XR_ARRAY_REF_ELEM_COUNT(obj);
        if (XR_LIKELY(idx >= 0 && idx < count))
            return xrt_fixed_array_get(obj.ptr, XR_ARRAY_REF_ELEM_TYPE(obj), idx);
        xrt_fixed_index_oob(idx, count);
    }
    if (obj.tag == XR_TAG_ENUM && XR_IS_INT(key)) {
        const XrAotEnumBox *ev = (const XrAotEnumBox *) obj.ptr;
        if (!ev)
            return XR_NULL_VAL;
        if (key.i == 0)
            return XR_FROM_INT(ev->member_index);
        if (key.i > 0 && (uint32_t) key.i <= ev->payload_count)
            return ev->payloads[key.i - 1];
    }
    xrt_freestanding_trap("freestanding index get supports only fixed arrays");
    return XR_NULL_VAL;
}

static inline XrValue xrt_enum_field_get(XrValue boxed, int64_t index) {
    if (boxed.tag != XR_TAG_ENUM || !boxed.ptr)
        return XR_NULL_VAL;
    const XrAotEnumBox *ev = (const XrAotEnumBox *) boxed.ptr;
    if (index == 0)
        return XR_FROM_INT(ev->member_index);
    if (index > 0 && (uint32_t) index <= ev->payload_count)
        return ev->payloads[index - 1];
    return XR_NULL_VAL;
}

static inline XrValue xrt_enum_box_ordinal(XrValue obj) {
    if (XR_IS_INT(obj))
        return obj;
    if (obj.tag != XR_TAG_ENUM || !obj.ptr)
        return XR_FROM_INT(-1);
    const XrAotEnumBox *ev = (const XrAotEnumBox *) obj.ptr;
    return XR_FROM_INT(ev->member_index);
}

static inline void xrt_index_set(XrValue obj, XrValue key, XrValue val) {
    if (XR_IS_ARRAY_REF(obj) && XR_IS_INT(key)) {
        int64_t idx = key.i;
        uint32_t count = XR_ARRAY_REF_ELEM_COUNT(obj);
        if (XR_LIKELY(idx >= 0 && idx < count)) {
            xrt_fixed_array_set(obj.ptr, XR_ARRAY_REF_ELEM_TYPE(obj), idx, val);
            return;
        }
        xrt_fixed_index_oob(idx, count);
    }
    xrt_freestanding_trap("freestanding index set supports only fixed arrays");
}

static inline void xrt_write_bytes(const char *bytes, size_t len) {
    if (bytes && len > 0)
        xr_hook_write(bytes, len);
}

static inline void xrt_write_char(char c) {
    xr_hook_write(&c, 1);
}

static inline void xrt_write_cstr(const char *s) {
    xrt_write_bytes(s, xrt_freestanding_strlen(s));
}

static inline void xrt_print_u64(uint64_t value) {
    char buf[20];
    size_t pos = sizeof(buf);
    do {
        buf[--pos] = (char) ('0' + (value % 10u));
        value /= 10u;
    } while (value != 0);
    xrt_write_bytes(buf + pos, sizeof(buf) - pos);
}

static inline void xrt_print_i64(int64_t value) {
    if (value < 0) {
        xrt_write_char('-');
        xrt_print_u64((uint64_t) (-(value + 1)) + 1u);
    } else {
        xrt_print_u64((uint64_t) value);
    }
}

static inline void xrt_print_char(uint32_t cp) {
    char buf[4];
    size_t len = 0;
    if (cp <= 0x7Fu) {
        buf[len++] = (char) cp;
    } else if (cp <= 0x7FFu) {
        buf[len++] = (char) (0xC0u | (cp >> 6));
        buf[len++] = (char) (0x80u | (cp & 0x3Fu));
    } else if (cp <= 0xFFFFu) {
        buf[len++] = (char) (0xE0u | (cp >> 12));
        buf[len++] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
        buf[len++] = (char) (0x80u | (cp & 0x3Fu));
    } else {
        buf[len++] = (char) (0xF0u | (cp >> 18));
        buf[len++] = (char) (0x80u | ((cp >> 12) & 0x3Fu));
        buf[len++] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
        buf[len++] = (char) (0x80u | (cp & 0x3Fu));
    }
    xrt_write_bytes(buf, len);
}

static inline void xrt_print(XrValue v) {
    if (XR_IS_INT(v)) {
        xrt_print_i64(v.i);
    } else if (XR_IS_BOOL(v)) {
        xrt_write_cstr(v.i ? "true" : "false");
    } else if (XR_IS_NULL(v)) {
        xrt_write_cstr("null");
    } else if (XR_IS_RUNE(v)) {
        xrt_print_char((uint32_t) v.i);
    } else if (XR_IS_STR(v)) {
        xrt_write_bytes(xr_str_data(v), (size_t) xr_str_len(v));
    } else {
        xrt_freestanding_trap("freestanding print supports only scalar/string values");
    }
}

static inline void xrt_println(XrValue v) {
    xrt_print(v);
    xrt_write_char('\n');
}

static inline int64_t xrt_i64_add(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a + (uint64_t) b);
}

static inline int64_t xrt_i64_sub(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a - (uint64_t) b);
}

static inline int64_t xrt_i64_mul(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a * (uint64_t) b);
}

static inline int64_t xrt_i64_neg(int64_t a) {
    return (int64_t) (-(uint64_t) a);
}

/* Freestanding adapters for the shared xi.div/xi.mod owner. The value rule —
 * including INT64_MIN / -1 and the unsigned kinds — is the same owner the VM
 * and the hosted profile evaluate; only the divide-by-zero publication is
 * profile-specific, and here it is the no-libc trap. */
static inline int64_t xrt_int_div(int64_t a, int64_t b) {
    XrIntDivModResult quotient = XRT_FREESTANDING_INT_DIV_MOD_CHECKED(XR_INT_DIV_MOD_DIV, a, b);
    if (XR_UNLIKELY(quotient.divisor_is_zero))
        xrt_freestanding_trap("division by zero");
    return quotient.value;
}

static inline int64_t xrt_int_mod(int64_t a, int64_t b) {
    XrIntDivModResult remainder = XRT_FREESTANDING_INT_DIV_MOD_CHECKED(XR_INT_DIV_MOD_MOD, a, b);
    if (XR_UNLIKELY(remainder.divisor_is_zero))
        xrt_freestanding_trap("modulo by zero");
    return remainder.value;
}

/* Unsigned division / modulo for statically-unsigned operands (mirrors VM
 * OP_DIV_U / OP_MOD_U). uint64_t covers every unsigned width: narrower payloads
 * are zero-extended in the i64 value model. */
static inline int64_t xrt_uint_div(int64_t a, int64_t b) {
    XrIntDivModResult quotient = XRT_FREESTANDING_INT_DIV_MOD_CHECKED(XR_INT_DIV_MOD_DIV_U, a, b);
    if (XR_UNLIKELY(quotient.divisor_is_zero))
        xrt_freestanding_trap("division by zero");
    return quotient.value;
}

static inline int64_t xrt_uint_mod(int64_t a, int64_t b) {
    XrIntDivModResult remainder = XRT_FREESTANDING_INT_DIV_MOD_CHECKED(XR_INT_DIV_MOD_MOD_U, a, b);
    if (XR_UNLIKELY(remainder.divisor_is_zero))
        xrt_freestanding_trap("modulo by zero");
    return remainder.value;
}

static inline double xrt_math_number(XrValue v) {
    if (XR_IS_FLOAT(v))
        return v.f;
    if (XR_IS_INT(v) || XR_IS_BOOL(v) || XR_IS_RUNE(v))
        return (double) v.i;
    return 0.0;
}

static inline XrValue xrt_to_int(XrValue v) {
    if (XR_IS_INT(v))
        return v;
    if (XR_IS_FLOAT(v))
        return XR_FROM_INT((int64_t) v.f);
    if (XR_IS_BOOL(v) || XR_IS_RUNE(v))
        return XR_FROM_INT(v.i);
    if (XR_IS_STR(v)) {
        /* Spec 13.2, same strict decimal grammar and same message as the VM
         * and the hosted runtime; this profile reports it through the panic
         * hook because it has no unwinder to longjmp to. */
        XrStringParseIntResult parsed =
            xr_string_parse_int64(xr_str_data(v), (size_t) xr_str_len(v));
        if (!parsed.ok)
            xrt_throw_error(XR_ERR_INVALID_ARG_TYPE, XR_ERROR_CORE_INT_PARSE_MSG);
        return XR_FROM_INT(parsed.value);
    }
    return XR_FROM_INT(0);
}

static inline XrValue xrt_to_float(XrValue v) {
    if (XR_IS_FLOAT(v))
        return v;
    if (XR_IS_STR(v)) {
        XrStringParseFloatResult parsed =
            xr_string_parse_float64(xr_str_data(v), (size_t) xr_str_len(v));
        if (!parsed.ok)
            xrt_throw_error(XR_ERR_INVALID_ARG_TYPE, XR_ERROR_CORE_FLOAT_PARSE_MSG);
        return XR_FROM_FLOAT(parsed.value);
    }
    return XR_FROM_FLOAT(xrt_math_number(v));
}

static inline XrValue xrt_to_bool(XrValue v) {
    XrTruthyCoreKind kind = XR_TRUTHY_CORE_OBJECT;
    int64_t integer = 0;
    double floating = 0.0;
    int64_t size = 0;
    if (XR_IS_BOOL(v)) {
        kind = XR_TRUTHY_CORE_BOOL;
        integer = XR_TO_BOOL(v);
    } else if (XR_IS_NULL(v)) {
        kind = XR_TRUTHY_CORE_NULL;
    } else if (XR_IS_INT(v) || XR_IS_RUNE(v)) {
        kind = XR_TRUTHY_CORE_INT;
        integer = v.i;
    } else if (XR_IS_FLOAT(v)) {
        kind = XR_TRUTHY_CORE_FLOAT;
        floating = v.f;
    } else if (XR_IS_STR(v)) {
        kind = XR_TRUTHY_CORE_SIZED;
        size = xr_str_len(v);
    }
    return XR_FROM_BOOL(xr_truthy_core_eval(XR_SEM_OWNER_ID_SHARED_TRUTHINESS_HI,
                                             XR_SEM_OWNER_ID_SHARED_TRUTHINESS_LO,
                                             XR_SEM_CONSUMER_AOT_FREESTANDING, kind, integer,
                                             floating, size));
}

static inline int xr_truthy(XrValue v) {
    XR_TRUTHY_CORE_OWNER_GUARD(XR_SEM_OWNER_ID_SHARED_TRUTHINESS_HI,
                               XR_SEM_OWNER_ID_SHARED_TRUTHINESS_LO);
    XR_TRUTHY_CORE_CONSUMER_GUARD(XR_SEM_CONSUMER_AOT_FREESTANDING);
    return XR_TO_INT(xrt_to_bool(v)) != 0;
}

static inline uint8_t xrt_freestanding_value_kind(XrValue v) {
    if (v.tag == XR_TAG_PTR) {
        switch (v.heap_type) {
            case XR_TARRAY:
                return XR_TAG_ARRAY;
            case XR_TMAP:
                return XR_TAG_MAP;
            case XR_TSET:
                return XR_TAG_SET;
            default:
                return XR_TAG_PTR;
        }
    }
    return v.tag;
}

static inline XrTypeIdentityCoreKind xrt_freestanding_type_identity_kind(XrValue v) {
    switch (xrt_freestanding_value_kind(v)) {
        case XR_TAG_I64:
            return XR_TYPE_IDENTITY_CORE_INT;
        case XR_TAG_F64:
            return XR_TYPE_IDENTITY_CORE_FLOAT;
        case XR_TAG_BOOL:
            return XR_TYPE_IDENTITY_CORE_BOOL;
        case XR_TAG_RUNE:
            return XR_TYPE_IDENTITY_CORE_RUNE;
        case XR_TAG_NULL:
            return XR_TYPE_IDENTITY_CORE_NULL;
        case XR_TAG_STR:
        case XR_TAG_STR_ARC:
            return XR_TYPE_IDENTITY_CORE_STRING;
        case XR_TAG_ARRAY:
            return XR_TYPE_IDENTITY_CORE_ARRAY;
        case XR_TAG_SET:
            return XR_TYPE_IDENTITY_CORE_SET;
        case XR_TAG_MAP:
            return XR_TYPE_IDENTITY_CORE_MAP;
        case XR_TAG_PTR:
            if (v.ptr && v.heap_type == 0)
                return XR_TYPE_IDENTITY_CORE_OBJECT;
            return XR_TYPE_IDENTITY_CORE_INSTANCE;
        case XR_TAG_CLOSURE:
            return XR_TYPE_IDENTITY_CORE_FUNCTION;
        case XR_TAG_STRBUF:
            return XR_TYPE_IDENTITY_CORE_STRINGBUILDER;
        case XR_TAG_RANGE:
            return XR_TYPE_IDENTITY_CORE_RANGE;
        case XR_TAG_ENUM:
            return XR_TYPE_IDENTITY_CORE_ENUM_VALUE;
        case XR_TAG_BIGINT:
            return XR_TYPE_IDENTITY_CORE_BIGINT;
        default:
            return XR_TYPE_IDENTITY_CORE_INSTANCE;
    }
}

static inline int64_t xrt_typeof_id(XrValue v) {
    return (int64_t) xr_type_identity_core_eval(
        XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_HI,
        XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_LO, XR_SEM_CONSUMER_AOT_FREESTANDING,
        xrt_freestanding_type_identity_kind(v));
}

/* The freestanding carrier rule mirrors the hosted one: a tagged pair compares
 * only within one tag class, STR_ARC normalized to STR. The relation over each
 * class comes from the shared owner. */
static inline int64_t xrt_compare_tagged_equal(XrCompareKind kind, XrValue a, XrValue b) {
    uint32_t ta = (a.tag == XR_TAG_STR_ARC) ? XR_TAG_STR : a.tag;
    uint32_t tb = (b.tag == XR_TAG_STR_ARC) ? XR_TAG_STR : b.tag;
    if (ta != tb)
        return xrt_compare_equal(kind, false);
    if (ta == XR_TAG_I64 || ta == XR_TAG_BOOL || ta == XR_TAG_RUNE)
        return xrt_compare_i64(kind, a.i, b.i);
    if (ta == XR_TAG_F64)
        return xrt_compare_f64(kind, a.f, b.f);
    if (ta == XR_TAG_NULL)
        return xrt_compare_equal(kind, true);
    if (ta == XR_TAG_ENUM) {
        const XrAotEnumBox *ea = (const XrAotEnumBox *) a.ptr;
        const XrAotEnumBox *eb = (const XrAotEnumBox *) b.ptr;
        if (!ea || !eb)
            return xrt_compare_ptr(kind, ea, eb);
        bool same = ea->layout_id == eb->layout_id && ea->member_index == eb->member_index &&
                    ea->payload_count == eb->payload_count;
        for (uint32_t i = 0; same && i < ea->payload_count; i++)
            same = xrt_compare_tagged_equal(XR_COMPARE_EQ, ea->payloads[i], eb->payloads[i]) != 0;
        return xrt_compare_equal(kind, same);
    }
    return xrt_compare_equal(kind, a.ptr == b.ptr && a.ext == b.ext);
}

static inline int64_t xrt_eq(XrValue a, XrValue b) {
    return xrt_compare_tagged_equal(XR_COMPARE_EQ, a, b);
}

/* Freestanding ordering: the integer lane when both operands are integers, the
 * double lane otherwise, with the numeric projection the profile already owns. */
static inline int64_t xrt_compare_tagged_order(XrCompareKind kind, XrValue a, XrValue b) {
    if (XR_IS_INT(a) && XR_IS_INT(b))
        return xrt_compare_i64(kind, a.i, b.i);
    return xrt_compare_f64(kind, xrt_math_number(a), xrt_math_number(b));
}

static inline int64_t xrt_lt(XrValue a, XrValue b) {
    return xrt_compare_tagged_order(XR_COMPARE_LT, a, b);
}

static inline int64_t xrt_le(XrValue a, XrValue b) {
    return xrt_compare_tagged_order(XR_COMPARE_LE, a, b);
}

static inline XrValue xrt_add(XrValue a, XrValue b) {
    if (XR_IS_INT(a) && XR_IS_INT(b))
        return XR_FROM_INT(xrt_i64_add(a.i, b.i));
    return XR_FROM_FLOAT(xrt_math_number(a) + xrt_math_number(b));
}

static inline XrValue xrt_sub(XrValue a, XrValue b) {
    if (XR_IS_INT(a) && XR_IS_INT(b))
        return XR_FROM_INT(xrt_i64_sub(a.i, b.i));
    return XR_FROM_FLOAT(xrt_math_number(a) - xrt_math_number(b));
}

static inline XrValue xrt_mul(XrValue a, XrValue b) {
    if (XR_IS_INT(a) && XR_IS_INT(b))
        return XR_FROM_INT(xrt_i64_mul(a.i, b.i));
    return XR_FROM_FLOAT(xrt_math_number(a) * xrt_math_number(b));
}

static inline XrValue xrt_div(XrValue a, XrValue b) {
    if (XR_IS_INT(a) && XR_IS_INT(b))
        return XR_FROM_INT(xrt_int_div(a.i, b.i));
    return XR_FROM_FLOAT(xrt_math_number(a) / xrt_math_number(b));
}

static inline XrValue xrt_mod(XrValue a, XrValue b) {
    if (XR_IS_INT(a) && XR_IS_INT(b))
        return XR_FROM_INT(xrt_int_mod(a.i, b.i));
    xrt_freestanding_trap("modulo requires integer types");
    return XR_NULL_VAL;
}

static inline XrValue xrt_neg(XrValue a) {
    if (XR_IS_INT(a)) {
        XrNumericNegResult result = xrt_numeric_neg_eval(XR_NUMERIC_NEG_I64, a.i, 0.0);
        return XR_FROM_INT(result.i64);
    }
    if (XR_IS_FLOAT(a)) {
        XrNumericNegResult result = xrt_numeric_neg_eval(XR_NUMERIC_NEG_F64, 0, a.f);
        return XR_FROM_FLOAT(result.f64);
    }
    xrt_freestanding_trap("operand must be numeric");
    return XR_NULL_VAL;
}

static inline void xrt_arc_init(void) {
}

static inline void xrt_arc_shutdown(void) {
}

static inline int64_t xrt_mem_int_arg(XrValue v) {
    return XR_IS_INT(v) ? XR_TO_INT(v) : 0;
}

/* Integer bit intrinsics are stable xi.bit.* ops and therefore require no
 * freestanding runtime wrappers. Wrapping/overflow arithmetic is emitted from
 * xr_arith_core.h / xr_i64_* as before. */

static inline XrValue xrt_mem_fence(XrValue ordering) {
    xr_sync_core_fence(xrt_mem_int_arg(ordering));
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_compiler_guard_u64(XrValue value) {
    uint64_t bits = (uint64_t) xrt_mem_int_arg(value);
#if defined(__GNUC__) || defined(__clang__)
    __asm__("" : "+r"(bits) : : "memory");
#endif
    return XR_FROM_INT((int64_t) bits);
}

static inline XrValue xrt_mem_compiler_opaque_u64(XrValue value) {
    uint64_t bits = (uint64_t) xrt_mem_int_arg(value);
#if defined(__GNUC__) || defined(__clang__)
    __asm__("" : "+r"(bits));
#endif
    return XR_FROM_INT((int64_t) bits);
}

static inline XrValue xrt_mem_prefetch(XrValue ptr, XrValue rw) {
#if defined(__GNUC__) || defined(__clang__)
    if (xrt_mem_int_arg(rw) != 0)
        __builtin_prefetch(ptr.ptr, 1, 3);
    else
        __builtin_prefetch(ptr.ptr, 0, 3);
#else
    (void) ptr;
    (void) rw;
#endif
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_cache_line_size(void) {
    return XR_FROM_INT(64);
}

static inline XrValue xrt_buffer_new(int64_t length, int zeroed, size_t align) {
    if (length < 0)
        length = 0;
    if (align == 0)
        align = XRT_FREESTANDING_DEFAULT_ALLOC_ALIGN;
    if (!xrt_freestanding_is_power_of_two(align) || align < sizeof(void *))
        xrt_freestanding_trap("mem.allocAligned: align must be a power of two >= sizeof(void*)");

    xrt_buffer_object_t *buf =
        (xrt_buffer_object_t *) xrt_freestanding_arc_alloc(sizeof(xrt_buffer_object_t));
    if (!buf)
        xrt_freestanding_trap("mem.alloc: out of memory");
    buf->data = NULL;
    buf->length = length;
    buf->align = align;

    if (length > 0) {
        size_t size = (size_t) length;
        buf->data = xrt_freestanding_alloc_bytes(size, align);
        if (!buf->data) {
            xr_hook_free(XRT_ARC_HDR(buf));
            xrt_freestanding_trap("mem.alloc: out of memory");
        }
        if (zeroed)
            memset(buf->data, 0, size);
    }
    return xrt_buffer_box(buf);
}

static inline xr_span_t xrt_buffer_bytes_view(XrValue value, int readonly) {
    (void) readonly;
    xrt_buffer_object_t *buf = xrt_buffer_obj_ptr(value);
    if (!buf)
        return xrt_span_empty();
    xr_span_t out = {0};
    out.data = buf->data;
    out.length = buf->length;
    return out;
}

static inline xr_span_t xrt_buffer_as_bytes(XrValue value) {
    return xrt_buffer_bytes_view(value, 1);
}

static inline xr_span_t xrt_buffer_as_mut_bytes(XrValue value) {
    return xrt_buffer_bytes_view(value, 0);
}

static inline XrValue xrt_buffer_borrow_ptr(XrValue value) {
    xrt_buffer_object_t *buf = xrt_buffer_obj_ptr(value);
    return xr_mkptr(buf ? buf->data : NULL, XR_TAG_PTR);
}

static inline XrValue xrt_buffer_resize(XrValue value, XrValue size_value) {
    xrt_buffer_object_t *buf = xrt_buffer_obj_ptr(value);
    int64_t new_len = xrt_mem_int_arg(size_value);
    if (!buf || new_len < 0)
        return XR_FALSE_VAL;
    if (new_len == 0) {
        xrt_buffer_free_data(buf->data);
        buf->data = NULL;
        buf->length = 0;
        return XR_TRUE_VAL;
    }

    void *new_data = xrt_freestanding_alloc_bytes((size_t) new_len, buf->align);
    if (!new_data)
        return XR_FALSE_VAL;
    size_t copy = (size_t) ((buf->length < new_len) ? buf->length : new_len);
    if (copy > 0 && buf->data)
        memcpy(new_data, buf->data, copy);
    xrt_buffer_free_data(buf->data);
    buf->data = new_data;
    buf->length = new_len;
    return XR_TRUE_VAL;
}

static inline XrValue xrt_buffer_method_0(XrValue recv, int sym) {
    xrt_buffer_object_t *buf = xrt_buffer_obj_ptr(recv);
    if (!buf)
        return XR_NULL_VAL;
    if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
        return XR_FROM_INT(buf->length);
    if (sym == XRT_SYM_BORROW_PTR)
        return xrt_buffer_borrow_ptr(recv);
    return XR_NULL_VAL;
}

static inline XrValue xrt_buffer_method_1(XrValue recv, int sym, XrValue arg0) {
    if (sym == XRT_SYM_RESIZE)
        return xrt_buffer_resize(recv, arg0);
    return XR_NULL_VAL;
}

static inline XrValue xrt_method_0(XrValue recv, int sym) {
    if (recv.tag == XR_TAG_BUFFER)
        return xrt_buffer_method_0(recv, sym);
    return XR_NULL_VAL;
}

static inline XrValue xrt_method_1(XrValue recv, int sym, XrValue arg0) {
    if (recv.tag == XR_TAG_BUFFER)
        return xrt_buffer_method_1(recv, sym, arg0);
    return XR_NULL_VAL;
}

/* Statement-position dispatch, the freestanding counterpart of the hosted
 * xrt_method_discard_N (see xrt_method.h).  Generated C emits these wherever a
 * builtin method call's result has no consumer, so the freestanding profile has
 * to answer the same names.  Nothing is released: the whole dispatch surface
 * here is Buffer, whose arms answer with a length, a raw borrowed pointer, or a
 * resize status — never an owned reference.  An arm that starts returning one
 * needs the hosted gate (xrt_method_result_is_owned) mirrored here. */
static inline XrValue xrt_method_discard_0(XrValue recv, int sym) {
    (void) xrt_method_0(recv, sym);
    return XR_NULL_VAL;
}

static inline XrValue xrt_method_discard_1(XrValue recv, int sym, XrValue arg0) {
    (void) xrt_method_1(recv, sym, arg0);
    return XR_NULL_VAL;
}

static inline XrValue xrt_getprop(XrValue obj, int64_t symbol_id) {
    if (obj.tag == XR_TAG_BUFFER)
        return xrt_buffer_method_0(obj, (int) symbol_id);
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_alloc(XrValue n) {
    return xrt_buffer_new(xrt_mem_int_arg(n), 0, 0);
}

static inline XrValue xrt_mem_alloc_zeroed(XrValue n) {
    return xrt_buffer_new(xrt_mem_int_arg(n), 1, 0);
}

static inline XrValue xrt_mem_alloc_aligned(XrValue n, XrValue align) {
    return xrt_buffer_new(xrt_mem_int_arg(n), 0, (size_t) xrt_mem_int_arg(align));
}

static inline XrValue xrt_mem_page_alloc(XrValue bytes, XrValue prot) {
    int64_t n = xrt_mem_int_arg(bytes);
    if (n <= 0)
        return xr_mkptr(NULL, XR_TAG_PTR);
    return xr_mkptr(xr_hook_page_alloc((size_t) n, xrt_mem_int_arg(prot)), XR_TAG_PTR);
}

static inline XrValue xrt_mem_page_alloc_default(XrValue bytes) {
    return xrt_mem_page_alloc(bytes, XR_FROM_INT(XRT_MEM_PROT_READ | XRT_MEM_PROT_WRITE));
}

static inline XrValue xrt_mem_page_protect(XrValue ptr, XrValue bytes, XrValue prot) {
    int64_t n = xrt_mem_int_arg(bytes);
    if (!ptr.ptr || n <= 0)
        return XR_FALSE_VAL;
    return XR_FROM_BOOL(xr_hook_page_protect(ptr.ptr, (size_t) n, xrt_mem_int_arg(prot)));
}

static inline XrValue xrt_mem_page_free(XrValue ptr, XrValue bytes) {
    int64_t n = xrt_mem_int_arg(bytes);
    if (!ptr.ptr || n <= 0)
        return XR_FALSE_VAL;
    return XR_FROM_BOOL(xr_hook_page_free(ptr.ptr, (size_t) n));
}

static inline XrValue xrt_mem_ptr(XrValue addr) {
    return xr_mkptr((void *) (uintptr_t) (int64_t) xrt_mem_int_arg(addr), XR_TAG_PTR);
}

static inline XrValue xrt_mem_mut_ptr(XrValue addr) {
    return xrt_mem_ptr(addr);
}

static inline XrValue xrt_mem_addr(XrValue ptr) {
    return XR_FROM_INT((int64_t) (intptr_t) ptr.ptr);
}

static inline XrValue xrt_mem_copy(XrValue dst, XrValue src, XrValue n) {
    memcpy(dst.ptr, src.ptr, (size_t) xrt_mem_int_arg(n));
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_move(XrValue dst, XrValue src, XrValue n) {
    memmove(dst.ptr, src.ptr, (size_t) xrt_mem_int_arg(n));
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_set(XrValue dst, XrValue byte, XrValue n) {
    memset(dst.ptr, (int) xrt_mem_int_arg(byte), (size_t) xrt_mem_int_arg(n));
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_compare(XrValue a, XrValue b, XrValue n) {
    return XR_FROM_INT(memcmp(a.ptr, b.ptr, (size_t) xrt_mem_int_arg(n)));
}

static inline void xrt_mem_cache_maintain_range(void *ptr, size_t n) {
    if (!ptr || n == 0)
        return;
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    uintptr_t addr = (uintptr_t) ptr;
    uintptr_t start = addr & ~(uintptr_t) 63u;
    uintptr_t end = addr + n;
    if (end < addr)
        end = UINTPTR_MAX;
    for (uintptr_t p = start; p < end; p += 64u) {
        __asm__ __volatile__("clflush (%0)" : : "r"((const void *) p) : "memory");
        if (UINTPTR_MAX - p < 64u)
            break;
    }
#else
    (void) ptr;
    (void) n;
#endif
    xr_sync_core_fence(4);
}

static inline XrValue xrt_mem_cache_flush(XrValue ptr, XrValue n) {
    int64_t len = xrt_mem_int_arg(n);
    if (len > 0)
        xrt_mem_cache_maintain_range(ptr.ptr, (size_t) len);
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_cache_invalidate(XrValue ptr, XrValue n) {
    int64_t len = xrt_mem_int_arg(n);
    if (len > 0)
        xrt_mem_cache_maintain_range(ptr.ptr, (size_t) len);
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_volatile_load(XrValue ptr, XrValue size) {
    void *p = ptr.ptr;
    uint64_t v = 0;
    switch (xrt_mem_int_arg(size)) {
        case 1:
            v = *(volatile uint8_t *) p;
            break;
        case 2:
            v = *(volatile uint16_t *) p;
            break;
        case 4:
            v = *(volatile uint32_t *) p;
            break;
        case 8:
            v = *(volatile uint64_t *) p;
            break;
        default:
            break;
    }
    return XR_FROM_INT((int64_t) v);
}

static inline XrValue xrt_mem_volatile_store(XrValue ptr, XrValue value, XrValue size) {
    void *p = ptr.ptr;
    int64_t v = xrt_mem_int_arg(value);
    switch (xrt_mem_int_arg(size)) {
        case 1:
            *(volatile uint8_t *) p = (uint8_t) v;
            break;
        case 2:
            *(volatile uint16_t *) p = (uint16_t) v;
            break;
        case 4:
            *(volatile uint32_t *) p = (uint32_t) v;
            break;
        case 8:
            *(volatile uint64_t *) p = (uint64_t) v;
            break;
        default:
            break;
    }
    return XR_NULL_VAL;
}

static inline void xrt_mem_nontemporal_store_raw(void *p, uint64_t value, int64_t size) {
    int streamed = 0;
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    uintptr_t addr = (uintptr_t) p;
    if (size == 4 && (addr & 3u) == 0) {
        uint32_t v32 = (uint32_t) value;
        __asm__ __volatile__("movnti %1, %0" : "=m"(*(uint32_t *) p) : "r"(v32) : "memory");
        streamed = 1;
    }
#if defined(__x86_64__)
    else if (size == 8 && (addr & 7u) == 0) {
        uint64_t v64 = (uint64_t) value;
        __asm__ __volatile__("movnti %1, %0" : "=m"(*(uint64_t *) p) : "r"(v64) : "memory");
        streamed = 1;
    }
#endif
    else
#endif
    {
        switch (size) {
            case 1:
                *(volatile uint8_t *) p = (uint8_t) value;
                break;
            case 2:
                *(volatile uint16_t *) p = (uint16_t) value;
                break;
            case 4:
                *(volatile uint32_t *) p = (uint32_t) value;
                break;
            case 8:
                *(volatile uint64_t *) p = (uint64_t) value;
                break;
            default:
                break;
        }
    }
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    if (streamed)
        __asm__ __volatile__("sfence" ::: "memory");
    else
#endif
        xr_sync_core_fence(4);
}

static inline XrValue xrt_mem_nontemporal_store(XrValue ptr, XrValue value, XrValue size) {
    xrt_mem_nontemporal_store_raw(ptr.ptr, (uint64_t) xrt_mem_int_arg(value),
                                  xrt_mem_int_arg(size));
    return XR_NULL_VAL;
}

#endif  // XRT_CORE_FREESTANDING_H
