/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_mem.h - Freestanding AOT wrappers for the mem.* bit intrinsics.
 *
 * These mirror the VM bindings in stdlib/mem/mem.c one-for-one and call
 * the same shared core (src/shared/xr_bits_core.h), so AOT-compiled code
 * and the VM produce identical results. AOT direct-call metadata for
 * these lives in stdlib/defs/core.def (aot: "xrt_mem_*", arg_spec).
 */

#ifndef XRT_MEM_H
#define XRT_MEM_H

#include "xrt_value.h"
#include "../shared/xr_bits_core.h"

static inline int64_t xrt_mem_int_arg(XrValue v) {
    return XR_IS_INT(v) ? XR_TO_INT(v) : 0;
}

static inline XrValue xrt_mem_popcount(XrValue x) {
    return XR_FROM_INT(xr_bits_core_popcount(xrt_mem_int_arg(x)));
}

static inline XrValue xrt_mem_leading_zeros(XrValue x) {
    return XR_FROM_INT(xr_bits_core_leading_zeros(xrt_mem_int_arg(x)));
}

static inline XrValue xrt_mem_trailing_zeros(XrValue x) {
    return XR_FROM_INT(xr_bits_core_trailing_zeros(xrt_mem_int_arg(x)));
}

static inline XrValue xrt_mem_byteswap(XrValue x) {
    return XR_FROM_INT(xr_bits_core_byteswap(xrt_mem_int_arg(x)));
}

static inline XrValue xrt_mem_rotate_left(XrValue x, XrValue n) {
    return XR_FROM_INT(xr_bits_core_rotate_left(xrt_mem_int_arg(x), xrt_mem_int_arg(n)));
}

static inline XrValue xrt_mem_rotate_right(XrValue x, XrValue n) {
    return XR_FROM_INT(xr_bits_core_rotate_right(xrt_mem_int_arg(x), xrt_mem_int_arg(n)));
}

#endif  // XRT_MEM_H
