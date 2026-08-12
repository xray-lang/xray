/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt.h - Umbrella header for AOT runtime
 *
 * KEY CONCEPT:
 *   Includes all layered sub-headers in dependency order:
 *     L0  xr_raw_scalar_core.h - unchecked unaligned scalar memory access
 *     L0  xrt_value.h   - tags, boxing/unboxing, source-level aliases, XrtContext
 *     L1  xrt_arc.h     - execution-local ARC arena, str_alloc/str_concat
 *     L1  xrt_net.h     - hosted TCP handle helpers
 *     L1  xrt_range.h   - lazy Range value
 *     L2  xrt_coll.h    - Array, Map, Json, StringBuilder, Closure, index ops
 *     L2  xrt_compress.h - freestanding checksum helpers
 *     L2  xrt_crypto.h  - freestanding crypto utilities
 *     L2  xrt_regex.h   - freestanding regex utilities
 *     L2  xrt_math.h    - freestanding math system helpers
 *     L2  xrt_time.h    - freestanding time query helpers
 *     L2  xrt_os.h      - freestanding OS query helpers
 *     L2  xrt_io.h      - freestanding sync filesystem helpers
 *     L2  xrt_arith.h   - arithmetic, comparison, print
 *     L3  xrt_method.h  - method dispatch, property access, toString
 *
 *   All runtime primitives are fully self-contained (no extern VM dependency).
 *   AOT-generated code includes only this header.
 *
 * RELATED MODULES:
 *   - xi_cgen.c: generates C code that includes this header
 */

#ifndef XRT_H
#define XRT_H

#include "../shared/xr_raw_scalar_core.h"  // L0: unsafe raw scalar load/store
#include "../shared/xr_raw_memory_core.h"  // L0: unsafe non-overlapping memory copy
#include "../shared/xr_bits_core.h"        // L0: exact-width compiler bit intrinsics
#include "../shared/xr_range_core.h"       // L0: canonical Range semantics
#include "../shared/xr_numeric_core.h"     // L0: canonical numeric negation
#include "../shared/xr_numeric_conversion_core.h"  // L0: deterministic scalar narrowing
#define xrt_bits_exact_eval(kernel, lhs, rhs, native_type)                                        \
    XR_BITS_EXACT_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_BITS_HI,                                     \
                              XR_SEM_OWNER_ID_SHARED_BITS_LO,                                     \
                              XR_SEM_CONSUMER_AOT_HOSTED, kernel, lhs, rhs, native_type)
#define xrt_bits_not_eval(value)                                                                  \
    XR_BITS_NOT_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_BITS_NOT_HI,                                  \
                            XR_SEM_OWNER_ID_SHARED_BITS_NOT_LO,                                  \
                            XR_SEM_CONSUMER_AOT_HOSTED, value)
#define xrt_bitwise_binary_eval(kind, lhs, rhs)                                                   \
    XR_BITWISE_BINARY_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_BITWISE_BINARY_HI,                       \
                                   XR_SEM_OWNER_ID_SHARED_BITWISE_BINARY_LO,                      \
                                   XR_SEM_CONSUMER_AOT_HOSTED, kind, lhs, rhs)
#define xrt_shift_eval(kind, value, count)                                                        \
    XR_SHIFT_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_SHIFT_HI,                                        \
                         XR_SEM_OWNER_ID_SHARED_SHIFT_LO, XR_SEM_CONSUMER_AOT_HOSTED, kind,       \
                         value, count)
#define xrt_numeric_width_eval(kernel, value)                                                      \
    XR_NUMERIC_WIDTH_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI,                    \
                                 XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO,                    \
                                 XR_SEM_CONSUMER_AOT_HOSTED, kernel, value)
#define xrt_range_semantics(start, end, inclusive_end)                                            \
    XR_RANGE_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_RANGE_HI,                                        \
                         XR_SEM_OWNER_ID_SHARED_RANGE_LO, XR_SEM_CONSUMER_AOT_HOSTED, start, end, \
                         inclusive_end)
#define xrt_numeric_neg_eval(kind, i64, f64)                                                       \
    XR_NUMERIC_NEG_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_HI,                             \
                               XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_LO,                             \
                               XR_SEM_CONSUMER_AOT_HOSTED, kind, i64, f64)
#define xrt_raw_memory_copy_nonoverlap(dst, src, count)                                           \
    XR_RAW_MEMORY_COPY_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_RAW_MEMORY_COPY_HI,                     \
                                   XR_SEM_OWNER_ID_SHARED_RAW_MEMORY_COPY_LO,                     \
                                   XR_SEM_CONSUMER_AOT_HOSTED, dst, src, count)
#define xrt_raw_scalar_access_load_i64(ptr, kind, pointer_width, endian)                           \
    XR_RAW_SCALAR_ACCESS_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI,                                              \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO, XR_SEM_CONSUMER_AOT_HOSTED,                 \
        xr_raw_scalar_load_i64((ptr), (kind), (pointer_width), (endian)))
#define xrt_raw_scalar_access_load_f64(ptr, kind, pointer_width, endian)                           \
    XR_RAW_SCALAR_ACCESS_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI,                                              \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO, XR_SEM_CONSUMER_AOT_HOSTED,                 \
        xr_raw_scalar_load_f64((ptr), (kind), (pointer_width), (endian)))
#define xrt_raw_scalar_access_load_pointer(ptr, kind, pointer_width, endian)                       \
    XR_RAW_SCALAR_ACCESS_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI,                                              \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO, XR_SEM_CONSUMER_AOT_HOSTED,                 \
        xr_raw_scalar_load_pointer((ptr), (kind), (pointer_width), (endian)))
#define xrt_raw_scalar_access_store_i64(ptr, kind, pointer_width, endian, value)                   \
    XR_RAW_SCALAR_ACCESS_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI,                                              \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO, XR_SEM_CONSUMER_AOT_HOSTED,                 \
        xr_raw_scalar_store_i64((ptr), (kind), (pointer_width), (endian), (value)))
#define xrt_raw_scalar_access_store_f64(ptr, kind, pointer_width, endian, value)                   \
    XR_RAW_SCALAR_ACCESS_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI,                                              \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO, XR_SEM_CONSUMER_AOT_HOSTED,                 \
        xr_raw_scalar_store_f64((ptr), (kind), (pointer_width), (endian), (value)))
#define xrt_raw_scalar_access_store_pointer(ptr, kind, pointer_width, endian, value)               \
    XR_RAW_SCALAR_ACCESS_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI,                                              \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO, XR_SEM_CONSUMER_AOT_HOSTED,                 \
        xr_raw_scalar_store_pointer((ptr), (kind), (pointer_width), (endian), (value)))
#define xrt_raw_scalar_access(type, ptr)                                                          \
    XR_RAW_SCALAR_ACCESS_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI,                                              \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO, XR_SEM_CONSUMER_AOT_HOSTED,                 \
        xr_raw_scalar_load_aggregate(type, ptr))
#define xrt_raw_scalar_access_store(type, ptr, value)                                             \
    XR_RAW_SCALAR_ACCESS_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI,                                              \
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO, XR_SEM_CONSUMER_AOT_HOSTED,                 \
        xr_raw_scalar_store_aggregate(type, ptr, value))
#ifndef xrt_byte_slice_scalar_eval
#define xrt_byte_slice_scalar_eval(expression)                                                     \
    XR_BYTE_SLICE_SCALAR_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_HI,                 \
                                     XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_LO,                 \
                                     XR_SEM_CONSUMER_AOT_HOSTED, expression)
#endif
#define xrt_byte_slice_compare_semantics(left_data, left_length, right_data, right_length, ok)     \
    XR_BYTE_SLICE_COMPARE_OWNER_APPLY(                                                            \
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMPARE_HI,                                             \
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMPARE_LO, XR_SEM_CONSUMER_AOT_HOSTED,                 \
        xr_byte_slice_compare_core((left_data), (left_length), XR_ELEM_U8, (right_data),          \
                                   (right_length), XR_ELEM_U8, (ok)))
#define xrt_byte_slice_fill_semantics(data, length, elem_type, value)                              \
    XR_BYTE_SLICE_FILL_OWNER_APPLY(                                                               \
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_FILL_HI, XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_FILL_LO,    \
        XR_SEM_CONSUMER_AOT_HOSTED,                                                               \
        xr_byte_slice_fill_core((data), (length), (elem_type), (value)))
#define xrt_byte_slice_copy_semantics(dst_data, dst_length, src_data, src_length)                  \
    XR_BYTE_SLICE_COPY_OWNER_APPLY(                                                               \
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COPY_HI, XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COPY_LO,    \
        XR_SEM_CONSUMER_AOT_HOSTED,                                                               \
        xr_byte_slice_copy_core((dst_data), (dst_length), XR_ELEM_U8, (src_data), (src_length),  \
                                XR_ELEM_U8))
#define xrt_byte_slice_repeat_semantics(data, length, dst_offset, distance, count)                 \
    XR_BYTE_SLICE_REPEAT_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_REPEAT_HI,                                             \
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_REPEAT_LO, XR_SEM_CONSUMER_AOT_HOSTED,                 \
        xr_byte_slice_repeat_core((data), (length), XR_ELEM_U8, (dst_offset), (distance),        \
                                  (count)))
#include "xrt_value.h"      // L0: tags, boxing, unboxing, source-level aliases, XrtContext
#include "xrt_arc.h"        // L1: execution arena, xrt_str_alloc, xrt_str_concat
#include "xrt_net.h"        // L1: hosted TCP handle helpers
#include "xrt_http.h"       // L1: HTTP backend capability boundary
#include "xrt_range.h"      // L1: lazy Range value
#include "xrt_coll.h"       // L2: Array, Map, Json, StringBuilder, Closure, index ops
#include "xrt_arith.h"      // L2: add/sub/mul/div/mod/neg, eq/lt/le, print
#include "xrt_compress.h"   // L2: freestanding checksum helpers
#include "xrt_crypto.h"     // L2: freestanding crypto helpers
#include "xrt_regex.h"      // L2: freestanding regex helpers
#include "xrt_math.h"       // L2: freestanding math helpers
#include "xrt_codegen.h"    // L2: semantic-neutral native code-shape controls
#include "xrt_mem.h"        // L2: freestanding mem bit intrinsics
#include "xrt_cluster.h"    // L2: standalone AOT cluster transport boundary
#include "xrt_sys.h"        // L2: sys.* OS-domain primitives
#include "xrt_time.h"       // L2: freestanding time query helpers
#include "xrt_os.h"         // L2: freestanding OS query helpers
#include "xrt_io.h"         // L2: freestanding sync filesystem helpers
#include "xrt_method.h"     // L3: method_0/1/2/3, getprop, tostring, symbol IDs
#include "xrt_exception.h"  // L4: setjmp/longjmp exception handling
#include "xrt_class.h"      // L5: ObjHeader, TypeInfo, type table, obj_alloc

#endif  // XRT_H
