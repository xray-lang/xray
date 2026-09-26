/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_scalar.c - Scalar boundary validation, overflow, and frame lifetime
 *
 * KEY CONCEPT:
 *   A failed admission allocates nothing, and every admitted frame has one owner.
 */

#include "xxir_scalar.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include <limits.h>

_Static_assert(sizeof(XrXirValue) == 16, "scalar boundary size");
_Static_assert(_Alignof(XrXirValue) == 8, "scalar boundary alignment");
_Static_assert(offsetof(XrXirValue, payload) == 8, "scalar payload offset");

XrXirRunStatus xr_xir_scalar_frame_begin(XrXirRunContext *context, uint32_t bytes,
                                       void **frame) {
    if (!frame)
        return XR_XIR_RUN_BAD_ARGUMENT;
    *frame = NULL;
    if (!context || context->frees > context->allocations ||
        context->peak_bytes < context->live_bytes)
        return XR_XIR_RUN_BAD_ARGUMENT;
#if !defined(XR_ARCH_X86_64)
    return XR_XIR_RUN_BAD_ABI;
#endif
    if (context->live_bytes > context->frame_limit ||
        bytes > context->frame_limit - context->live_bytes)
        return XR_XIR_RUN_FRAME_LIMIT;
    if (!bytes)
        return XR_XIR_RUN_OK;
    if (context->allocations == UINT64_MAX)
        return XR_XIR_RUN_FRAME_LIMIT;
    void *storage = xr_calloc(1, bytes);
    if (!storage)
        return XR_XIR_RUN_OUT_OF_MEMORY;
    context->live_bytes += bytes;
    if (context->live_bytes > context->peak_bytes)
        context->peak_bytes = context->live_bytes;
    ++context->allocations;
    *frame = storage;
    return XR_XIR_RUN_OK;
}

void xr_xir_scalar_frame_end(XrXirRunContext *context, uint32_t bytes, void *frame) {
    if (!frame)
        return;
    XR_CHECK(context && bytes && context->live_bytes >= bytes &&
             context->frees < context->allocations, "invalid scalar frame release");
    context->live_bytes -= bytes;
    ++context->frees;
    xr_free(frame);
}

XrXirRunStatus xr_xir_scalar_arithmetic(XrXirArithmetic operation,
                                        int64_t left, int64_t right, int64_t *result) {
    if (!result) return XR_XIR_RUN_BAD_ARGUMENT;
    *result = 0;
    uint64_t bits;
    switch (operation) {
    case XR_XIR_ARITH_ADD: bits = (uint64_t) left + (uint64_t) right; break;
    case XR_XIR_ARITH_SUB: bits = (uint64_t) left - (uint64_t) right; break;
    case XR_XIR_ARITH_MUL: bits = (uint64_t) left * (uint64_t) right; break;
    case XR_XIR_ARITH_DIV:
    case XR_XIR_ARITH_REM:
        if (!right) return XR_XIR_RUN_DIVIDE_BY_ZERO;
        if (left == INT64_MIN && right == -1)
            *result = operation == XR_XIR_ARITH_DIV ? INT64_MIN : 0;
        else *result = operation == XR_XIR_ARITH_DIV ? left / right : left % right;
        return XR_XIR_RUN_OK;
    default: return XR_XIR_RUN_BAD_ARGUMENT;
    }
    *result = bits <= INT64_MAX ? (int64_t) bits : -1 - (int64_t) (UINT64_MAX - bits);
    return XR_XIR_RUN_OK;
}
