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
#include "xxir_types.h"
#include "../shared/xnative_declaration.h"
#include <string.h>

static inline bool xr_xir_intrinsic_type(XrNativeTypeTerm term, XrXirType *type) {
    if (!type) return false;
    switch (term) {
    case XR_NATIVE_TERM_BOOL: *type = XR_XIR_BOOL; return true;
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
        call->member->is_static || !call->member->is_method)
        return NULL;
    if (call->implementation == XR_NATIVE_INTRINSIC_IMPLEMENTATION_STRING_BYTES)
        return call->id == 4 && call->member->id == 18 && call->member->is_public &&
            call->member->lowered && !call->member->parameter_count &&
            call->member->result == XR_NATIVE_TERM_SLICE_U8 && !call->ctfe &&
            call->member->operation == XR_NATIVE_OPERATION_STRING_BYTES &&
            call->member->allocation == XR_NATIVE_ALLOCATION_MAY_HEAP &&
            call->member->ownership == XR_NATIVE_RESULT_OWNERSHIP_BORROWED &&
            !strcmp(call->member->failures, "allocation,retain,limit") ? call : NULL;
    if (call->member->allocation != XR_NATIVE_ALLOCATION_NO_HEAP ||
        call->member->ownership != XR_NATIVE_RESULT_OWNERSHIP_OWNED ||
        strcmp(call->member->failures, "none") ||
        call->member->is_static || !call->member->is_method)
        return NULL;
    switch (call->implementation) {
    case XR_NATIVE_INTRINSIC_IMPLEMENTATION_STRING_CONTAINS:
    case XR_NATIVE_INTRINSIC_IMPLEMENTATION_STRING_STARTS_WITH:
    case XR_NATIVE_INTRINSIC_IMPLEMENTATION_STRING_ENDS_WITH: break;
    default: return NULL;
    }
    return call;
}
static inline bool xr_xir_intrinsic_parameter(const XrNativeIntrinsicDeclaration *call,
    uint32_t ordinal, XrXirType *type) {
    if (!call || !type || call->implementation == XR_NATIVE_INTRINSIC_IMPLEMENTATION_STRING_BYTES) return false;
    if (!ordinal) {
        if (call->owner->id != XR_NATIVE_DECLARATION_STRING) return false;
        *type = XR_XIR_STRING;
        return true;
    }
    if (ordinal > call->member->parameter_count) return false;
    const XrNativeParameter *parameter = &call->member->parameters[ordinal - 1];
    return !parameter->optional && !parameter->variadic && xr_xir_intrinsic_type(parameter->type, type);
}
/* Only this governed call creates a shared view from a physical string Cell.
 * Every analysis still requires ordinary module admission before using it. */
static inline bool xr_xir_intrinsic_string_bytes(const XrXirInstruction *op) {
    if (!op || op->op != XR_XIR_CALL_INTRINSIC) return false;
    const XrNativeIntrinsicDeclaration *call = xr_xir_intrinsic(op->immediate);
    return call && call->implementation == XR_NATIVE_INTRINSIC_IMPLEMENTATION_STRING_BYTES;
}
static inline bool xr_xir_intrinsic_string_bytes_types(const XrXirTypes *types,
    XrXirType root, XrXirType result) {
    const XrXirTypeNode *view = xr_xir_type_node(types, result);
    return xr_xir_type_is_cell(types, root) && xr_xir_cell_element(types, root) == XR_XIR_STRING &&
        view && view->kind == XR_XIR_TYPE_SLICE && view->element == XR_XIR_U8;
}
#endif // XXIR_INTRINSIC_H
