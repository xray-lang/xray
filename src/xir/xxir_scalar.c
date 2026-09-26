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

static bool integer_format_valid(XrXirIntegerFormat format) {
    return format.bits == 8 || format.bits == 16 || format.bits == 32 || format.bits == 64;
}
static int64_t integer_payload(XrXirIntegerFormat format, uint64_t bits) {
    uint64_t mask = UINT64_MAX >> (64 - format.bits);
    bits &= mask;
    if (format.is_signed && (bits & (UINT64_C(1) << (format.bits - 1)))) bits |= ~mask;
    return bits <= INT64_MAX ? (int64_t) bits : -1 - (int64_t) (UINT64_MAX - bits);
}
static bool integer_valid(XrXirIntegerFormat format, int64_t payload) {
    return integer_format_valid(format) && integer_payload(format, (uint64_t) payload) == payload;
}
XrXirRunStatus xr_xir_integer_convert(XrXirIntegerFormat source,
    XrXirIntegerFormat target, int64_t value, int64_t *result) {
    if (!result) return XR_XIR_RUN_BAD_ARGUMENT;
    *result = 0;
    if (!integer_valid(source, value) || !integer_format_valid(target)) return XR_XIR_RUN_BAD_ARGUMENT;
    *result = integer_payload(target, (uint64_t) value);
    return XR_XIR_RUN_OK;
}
XrXirRunStatus xr_xir_integer_compare(XrXirIntegerFormat format,
    int64_t left, int64_t right, int *result) {
    if (!result) return XR_XIR_RUN_BAD_ARGUMENT;
    *result = 0;
    if (!integer_valid(format, left) || !integer_valid(format, right)) return XR_XIR_RUN_BAD_ARGUMENT;
    *result = format.is_signed ? (left > right) - (left < right) :
        ((uint64_t) left > (uint64_t) right) - ((uint64_t) left < (uint64_t) right);
    return XR_XIR_RUN_OK;
}
XrXirRunStatus xr_xir_integer_arithmetic(XrXirIntegerFormat format,
    XrXirArithmetic operation, int64_t left, int64_t right, int64_t *result) {
    if (!result) return XR_XIR_RUN_BAD_ARGUMENT;
    *result = 0;
    bool shift = operation == XR_XIR_ARITH_SHL || operation == XR_XIR_ARITH_SHR;
    if (!integer_valid(format, left) || (!shift && !integer_valid(format, right))) return XR_XIR_RUN_BAD_ARGUMENT;
    uint64_t bits;
    switch (operation) {
    case XR_XIR_ARITH_ADD: bits = (uint64_t) left + (uint64_t) right; break;
    case XR_XIR_ARITH_SUB: bits = (uint64_t) left - (uint64_t) right; break;
    case XR_XIR_ARITH_MUL: bits = (uint64_t) left * (uint64_t) right; break;
    case XR_XIR_ARITH_AND: bits = (uint64_t) left & (uint64_t) right; break;
    case XR_XIR_ARITH_OR: bits = (uint64_t) left | (uint64_t) right; break;
    case XR_XIR_ARITH_XOR: bits = (uint64_t) left ^ (uint64_t) right; break;
    case XR_XIR_ARITH_SHL: bits = (uint64_t) left << ((uint64_t) right & 63); break;
    case XR_XIR_ARITH_SHR:
        bits = format.is_signed && left < 0 ? ~((~(uint64_t) left) >> ((uint64_t) right & 63)) :
            (uint64_t) left >> ((uint64_t) right & 63);
        break;
    case XR_XIR_ARITH_DIV:
    case XR_XIR_ARITH_REM:
        if (!right) return XR_XIR_RUN_DIVIDE_BY_ZERO;
        if (!format.is_signed)
            bits = operation == XR_XIR_ARITH_DIV ? (uint64_t) left / (uint64_t) right : (uint64_t) left % (uint64_t) right;
        else if (left == INT64_MIN && right == -1)
            bits = operation == XR_XIR_ARITH_DIV ? (uint64_t) INT64_MIN : 0;
        else bits = (uint64_t) (operation == XR_XIR_ARITH_DIV ? left / right : left % right);
        break;
    default: return XR_XIR_RUN_BAD_ARGUMENT;
    }
    *result = integer_payload(format, bits);
    return XR_XIR_RUN_OK;
}
