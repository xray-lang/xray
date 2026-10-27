/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_intrinsic.h - Checked projections of canonical library call facts
 *
 * KEY CONCEPT:
 *   A call ID selects immutable declarations. Unsupported representations and
 *   implementations stay closed before any executable artifact is published.
 */
#ifndef XXIR_INTRINSIC_H
#define XXIR_INTRINSIC_H
#include "xxir.h"
#include "../shared/xnative_declaration.h"
#include <string.h>

static inline bool xr_xir_intrinsic_type(XrNativeTypeTerm term, XrXirType *type) {
    if (!type) return false;
    switch (term) {
    case XR_NATIVE_TERM_BOOL: *type = XR_XIR_BOOL; return true;
    case XR_NATIVE_TERM_I64: *type = XR_XIR_I64; return true;
    case XR_NATIVE_TERM_STRING: *type = XR_XIR_STRING; return true;
    default: return false;
    }
}
static inline const XrNativeIntrinsicDeclaration *xr_xir_intrinsic(int64_t id) {
    if (id <= 0 || (uint64_t)id > UINT32_MAX) return NULL;
    const XrNativeIntrinsicDeclaration *call = xr_native_intrinsic_by_id((uint32_t)id);
    if (!call || call->effects != XR_NATIVE_INTRINSIC_READ ||
        call->owner->id != XR_NATIVE_DECLARATION_STRING ||
        call->member->receiver != XR_NATIVE_RECEIVER_READ ||
        call->member->ownership != XR_NATIVE_RESULT_OWNERSHIP_OWNED ||
        call->member->is_static || !call->member->is_method)
        return NULL;
    switch (call->implementation) {
    case XR_NATIVE_INTRINSIC_IMPLEMENTATION_STRING_CONTAINS:
    case XR_NATIVE_INTRINSIC_IMPLEMENTATION_STRING_STARTS_WITH:
    case XR_NATIVE_INTRINSIC_IMPLEMENTATION_STRING_ENDS_WITH:
        if (call->member->allocation != XR_NATIVE_ALLOCATION_NO_HEAP ||
            strcmp(call->member->failures, "none")) return NULL;
        break;
    case XR_NATIVE_INTRINSIC_IMPLEMENTATION_STRING_SLICE:
        if (call->member->operation != XR_NATIVE_OPERATION_STRING_SLICE ||
            call->member->allocation != XR_NATIVE_ALLOCATION_MAY_HEAP ||
            strcmp(call->member->failures, "bounds,allocation,retain,limit") ||
            call->member->result != XR_NATIVE_TERM_STRING || call->member->parameter_count != 2 ||
            !call->member->parameters ||
            call->member->parameters[0].type != XR_NATIVE_TERM_I64 ||
            call->member->parameters[1].type != XR_NATIVE_TERM_I64 ||
            call->member->parameters[0].optional || !call->member->parameters[1].optional ||
            call->member->parameters[0].variadic || call->member->parameters[1].variadic) return NULL;
        break;
    default: return NULL;
    }
    return call;
}
static inline bool xr_xir_intrinsic_parameter(const XrNativeIntrinsicDeclaration *call,
    uint32_t ordinal, XrXirType *type) {
    if (!call || !type) return false;
    if (!ordinal) {
        if (call->owner->id != XR_NATIVE_DECLARATION_STRING) return false;
        *type = XR_XIR_STRING;
        return true;
    }
    if (ordinal > call->member->parameter_count) return false;
    const XrNativeParameter *parameter = &call->member->parameters[ordinal - 1];
    bool expanded_end = call->implementation == XR_NATIVE_INTRINSIC_IMPLEMENTATION_STRING_SLICE && ordinal == 2;
    return (!parameter->optional || expanded_end) && !parameter->variadic && xr_xir_intrinsic_type(parameter->type, type);
}
#endif // XXIR_INTRINSIC_H
