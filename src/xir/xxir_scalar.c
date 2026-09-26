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

_Static_assert(sizeof(XrXirScalar) == 16, "scalar boundary size");
_Static_assert(_Alignof(XrXirScalar) == 8, "scalar boundary alignment");
_Static_assert(offsetof(XrXirScalar, payload) == 8, "scalar payload offset");

bool xr_xir_scalar_argument(const XrXirScalar *argument, XrXirType type) {
    if (!argument || argument->reserved || argument->type != (uint32_t) type)
        return false;
    return type == XR_XIR_I64 ||
           (type == XR_XIR_BOOL && (argument->payload == 0 || argument->payload == 1));
}

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

XrXirRunStatus xr_xir_scalar_add(int64_t left, int64_t right, int64_t *result) {
    if (!result)
        return XR_XIR_RUN_BAD_ARGUMENT;
    if ((right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right)) {
        *result = 0;
        return XR_XIR_RUN_OVERFLOW;
    }
    *result = left + right;
    return XR_XIR_RUN_OK;
}
