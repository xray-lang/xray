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
static inline bool xr_xir_intrinsic_is_string_bytes(const XrNativeIntrinsicDeclaration *call) {
    return call && call->implementation==XR_NATIVE_INTRINSIC_IMPLEMENTATION_STRING_BYTES;
}
static inline const XrNativeIntrinsicDeclaration *xr_xir_intrinsic(int64_t id) {
    if (id <= 0 || (uint64_t)id > UINT32_MAX) return NULL;
    const XrNativeIntrinsicDeclaration *call = xr_native_intrinsic_by_id((uint32_t)id);
    if (!call || call->effects != XR_NATIVE_INTRINSIC_READ ||
        call->owner->id != XR_NATIVE_DECLARATION_STRING ||
        call->member->receiver != XR_NATIVE_RECEIVER_READ ||
        call->member->is_static || !call->member->is_method)
        return NULL;
    if (xr_xir_intrinsic_is_string_bytes(call)) {
        const XrNativeMemberDeclaration *member=call->member;
        return member->operation==XR_NATIVE_OPERATION_STRING_BYTES && !member->lowered &&
            !member->parameter_count && member->result==XR_NATIVE_TERM_SLICE_U8 &&
            member->allocation==XR_NATIVE_ALLOCATION_MAY_HEAP &&
            member->ownership==XR_NATIVE_RESULT_OWNERSHIP_BORROWED &&
            !strcmp(member->failures,"allocation,retain,limit") && !call->ctfe &&
            !strcmp(call->c_symbol,"xr_xir_call_view_operation") ? call : NULL;
    }
    if (call->member->allocation != XR_NATIVE_ALLOCATION_NO_HEAP ||
        call->member->ownership != XR_NATIVE_RESULT_OWNERSHIP_OWNED ||
        strcmp(call->member->failures, "none")) return NULL;
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
    if (!call || !type || xr_xir_intrinsic_is_string_bytes(call)) return false;
    if (!ordinal) {
        if (call->owner->id != XR_NATIVE_DECLARATION_STRING) return false;
        *type = XR_XIR_STRING;
        return true;
    }
    if (ordinal > call->member->parameter_count) return false;
    const XrNativeParameter *parameter = &call->member->parameters[ordinal - 1];
    return !parameter->optional && !parameter->variadic && xr_xir_intrinsic_type(parameter->type, type);
}
/* Constructed descriptors belong to the receiving pool, not a fixed ordinal. */
static inline bool xr_xir_intrinsic_result_matches(const XrNativeIntrinsicDeclaration *call,
    const XrXirTypes *types,XrXirType actual) {
    if (xr_xir_intrinsic_is_string_bytes(call)) {
        const XrXirTypeNode *node=xr_xir_type_node(types,actual);
        return node && node->kind==XR_XIR_TYPE_SLICE && node->element==XR_XIR_U8;
    }
    XrXirType expected;
    return call && xr_xir_intrinsic_type(call->member->result,&expected) && actual==expected;
}
static inline bool xr_xir_intrinsic_argument_matches(const XrNativeIntrinsicDeclaration *call,
    const XrXirTypes *types,uint32_t ordinal,XrXirType actual) {
    if (xr_xir_intrinsic_is_string_bytes(call)) {
        const XrXirTypeNode *node=xr_xir_type_node(types,actual);
        return !ordinal && node && node->kind==XR_XIR_TYPE_CELL && node->element==XR_XIR_STRING;
    }
    XrXirType expected;
    return xr_xir_intrinsic_parameter(call,ordinal,&expected) && actual==expected;
}
static inline bool xr_xir_instruction_is_string_bytes(const XrXirInstruction *op) {
    return op && op->op==XR_XIR_CALL_INTRINSIC &&
        xr_xir_intrinsic_is_string_bytes(xr_xir_intrinsic(op->immediate));
}
#endif // XXIR_INTRINSIC_H
