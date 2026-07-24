/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu
 * Licensed under the MIT License
 */

#include "xi_semantic_intrinsic.h"
#include "xi_effect.h"
#include "../runtime/value/xtype.h"

#include <stdarg.h>
#include <stdio.h>

XiOp xi_semantic_intrinsic_op(const XaIntrinsicDesc *desc) {
    if (!desc)
        return XI_OP_COUNT;
    switch (desc->lowering) {
        case XA_INTRINSIC_LOWERING_VEC_LOAD:
            return XI_VEC_LOAD;
        case XA_INTRINSIC_LOWERING_VEC_STORE:
            return XI_VEC_STORE;
        case XA_INTRINSIC_LOWERING_VEC_SPLAT:
            return XI_VEC_SPLAT;
        case XA_INTRINSIC_LOWERING_VEC_EXTRACT:
            return XI_VEC_EXTRACT;
        case XA_INTRINSIC_LOWERING_VEC_REPLACE:
            return XI_VEC_REPLACE;
        case XA_INTRINSIC_LOWERING_VEC_ADD:
            return XI_VEC_ADD;
        case XA_INTRINSIC_LOWERING_VEC_SUB:
            return XI_VEC_SUB;
        case XA_INTRINSIC_LOWERING_VEC_MUL:
            return XI_VEC_MUL;
        case XA_INTRINSIC_LOWERING_VEC_BIT_AND:
            return XI_VEC_BIT_AND;
        case XA_INTRINSIC_LOWERING_VEC_BIT_OR:
            return XI_VEC_BIT_OR;
        case XA_INTRINSIC_LOWERING_VEC_BIT_XOR:
            return XI_VEC_BIT_XOR;
        case XA_INTRINSIC_LOWERING_VEC_BIT_NOT:
            return XI_VEC_BIT_NOT;
        case XA_INTRINSIC_LOWERING_VEC_SHL:
            return XI_VEC_SHL;
        case XA_INTRINSIC_LOWERING_VEC_SHR:
            return XI_VEC_SHR;
        case XA_INTRINSIC_LOWERING_VEC_REINTERPRET:
            return XI_VEC_REINTERPRET;
        case XA_INTRINSIC_LOWERING_VEC_SHUFFLE:
            return XI_VEC_SHUFFLE;
        case XA_INTRINSIC_LOWERING_VEC_WIDEN_MUL:
            return XI_VEC_WIDEN_MUL;
        case XA_INTRINSIC_LOWERING_VEC_REDUCE_ADD:
            return XI_VEC_REDUCE_ADD;
        case XA_INTRINSIC_LOWERING_TARGET_SIMD_BYTES:
            return XI_TARGET_SIMD_BYTES;
        case XA_INTRINSIC_LOWERING_TARGET_SIMD_ACCELERATED:
            return XI_TARGET_SIMD_ACCELERATED;
        case XA_INTRINSIC_LOWERING_BIT_ROTL:
            return XI_BIT_ROTL;
        case XA_INTRINSIC_LOWERING_BIT_ROTR:
            return XI_BIT_ROTR;
        case XA_INTRINSIC_LOWERING_BIT_MUL_HIGH:
            return XI_BIT_MUL_HIGH;
        case XA_INTRINSIC_LOWERING_BIT_BSWAP:
            return XI_BIT_BSWAP;
        case XA_INTRINSIC_LOWERING_BIT_POPCOUNT:
            return XI_BIT_POPCOUNT;
        case XA_INTRINSIC_LOWERING_BIT_CLZ:
            return XI_BIT_CLZ;
        case XA_INTRINSIC_LOWERING_BIT_CTZ:
            return XI_BIT_CTZ;
        case XA_INTRINSIC_LOWERING_BYTE_SLICE_TYPED_LOAD:
        case XA_INTRINSIC_LOWERING_BYTE_SLICE_TYPED_STORE:
            return XI_OP_COUNT;
        case XA_INTRINSIC_LOWERING_BYTE_SLICE_FILL:
            return XI_BYTE_SLICE_FILL;
        case XA_INTRINSIC_LOWERING_BYTE_SLICE_COPY:
            return XI_BYTE_SLICE_COPY;
        case XA_INTRINSIC_LOWERING_BYTE_SLICE_COMPARE:
            return XI_BYTE_SLICE_COMPARE;
        case XA_INTRINSIC_LOWERING_BYTE_SLICE_COMMON_PREFIX:
            return XI_BYTE_SLICE_COMMON_PREFIX;
        case XA_INTRINSIC_LOWERING_BYTE_SLICE_REPEAT:
            return XI_BYTE_SLICE_REPEAT;
        case XA_INTRINSIC_LOWERING_SLICE_REINTERPRET:
            return XI_SLICE_REINTERPRET;
        case XA_INTRINSIC_LOWERING_SLICE_DATA_PTR:
            return XI_ARRAY_DATA_PTR;
        case XA_INTRINSIC_LOWERING_SLICE_WINDOW:
            return XI_SLICE_WINDOW;
        case XA_INTRINSIC_LOWERING_SLICE_AS_BYTES:
            return XI_SLICE_AS_BYTES;
        case XA_INTRINSIC_LOWERING_SLICE_FILL:
            return XI_SLICE_FILL;
        case XA_INTRINSIC_LOWERING_SLICE_COPY:
            return XI_SLICE_COPY;
        case XA_INTRINSIC_LOWERING_SLICE_COMPARE:
            return XI_SLICE_COMPARE;
        case XA_INTRINSIC_LOWERING_SLICE_GET:
            return XI_INDEX_GET;
        case XA_INTRINSIC_LOWERING_ATOMIC_LOAD:
            return XI_ATOMIC_LOAD;
        case XA_INTRINSIC_LOWERING_ATOMIC_STORE:
            return XI_ATOMIC_STORE;
        case XA_INTRINSIC_LOWERING_ATOMIC_RMW:
            return XI_ATOMIC_RMW;
        case XA_INTRINSIC_LOWERING_ATOMIC_TO_STRING:
            return XI_ATOMIC_TO_STRING;
        case XA_INTRINSIC_LOWERING_PAR_FOR:
            return XI_PAR_FOR;
        case XA_INTRINSIC_LOWERING_PAR_MAP:
        case XA_INTRINSIC_LOWERING_PAR_MAP_INTO:
            return XI_PAR_MAP;
        case XA_INTRINSIC_LOWERING_PAR_REDUCE:
            return XI_PAR_REDUCE;
        case XA_INTRINSIC_LOWERING_NONE:
            return XI_OP_COUNT;
    }
    return XI_OP_COUNT;
}

static bool semantic_intrinsic_op_matches(const XaIntrinsicDesc *desc, XiOp op) {
    if (!desc)
        return false;
    if (desc->lowering == XA_INTRINSIC_LOWERING_BYTE_SLICE_TYPED_LOAD) {
        return op == XI_BYTE_SLICE_LOAD_U16 || op == XI_BYTE_SLICE_LOAD_U32 ||
               op == XI_BYTE_SLICE_LOAD_U64 || op == XI_BYTE_SLICE_LOAD_F32 ||
               op == XI_BYTE_SLICE_LOAD_F64;
    }
    if (desc->lowering == XA_INTRINSIC_LOWERING_BYTE_SLICE_TYPED_STORE) {
        return op == XI_BYTE_SLICE_STORE_U16 || op == XI_BYTE_SLICE_STORE_U32 ||
               op == XI_BYTE_SLICE_STORE_U64 || op == XI_BYTE_SLICE_STORE_F32 ||
               op == XI_BYTE_SLICE_STORE_F64;
    }
    XiOp expected = xi_semantic_intrinsic_op(desc);
    return expected != XI_OP_COUNT && op == expected;
}

static bool set_error(char *error, size_t error_size, const char *format, ...) {
    if (error && error_size) {
        va_list args;
        va_start(args, format);
        vsnprintf(error, error_size, format, args);
        va_end(args);
    }
    return false;
}

static bool semantic_intrinsic_native_is_integer(int64_t native_type) {
    switch ((XrNativeType) native_type) {
        case XR_NATIVE_I8:
        case XR_NATIVE_I16:
        case XR_NATIVE_I32:
        case XR_NATIVE_I64:
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
            return true;
        default:
            return false;
    }
}

bool xi_semantic_intrinsic_verify_value(const XiValue *value, XiStage stage, char *error,
                                        size_t error_size) {
    if (error && error_size)
        error[0] = '\0';
    if (!value || value->xa_intrinsic_id == XA_INTRINSIC_NONE)
        return set_error(error, error_size, "Xi value has no canonical intrinsic identity");

    const XaIntrinsicDesc *desc = xa_intrinsic_by_id((XaIntrinsicId) value->xa_intrinsic_id);
    if (!desc)
        return set_error(error, error_size, "unknown canonical intrinsic id %u",
                         value->xa_intrinsic_id);

    XiOp expected = xi_semantic_intrinsic_op(desc);
    if (!semantic_intrinsic_op_matches(desc, (XiOp) value->op))
        return set_error(error, error_size, "canonical intrinsic id %u requires Xi op %u",
                         value->xa_intrinsic_id, (unsigned) expected);

    bool has_unchecked_access = (value->aux_int & XI_ACCESS_UNCHECKED) != 0;
    if (has_unchecked_access) {
        bool allowed = value->op == XI_VEC_LOAD || value->op == XI_VEC_STORE ||
                       value->op == XI_BYTE_SLICE_LOAD_U16 || value->op == XI_BYTE_SLICE_LOAD_U32 ||
                       value->op == XI_BYTE_SLICE_LOAD_U64 ||
                       value->op == XI_BYTE_SLICE_STORE_U16 ||
                       value->op == XI_BYTE_SLICE_STORE_U32 ||
                       value->op == XI_BYTE_SLICE_STORE_U64 || value->op == XI_SLICE_WINDOW ||
                       value->op == XI_BYTE_SLICE_COPY || value->op == XI_SLICE_COPY;
        if (!allowed)
            return set_error(error, error_size,
                             "canonical intrinsic id %u has unchecked unsupported op %u",
                             value->xa_intrinsic_id, (unsigned) value->op);
    }

    uint32_t min_nargs = (uint32_t) desc->min_arity + 1u;
    uint32_t max_nargs = (uint32_t) desc->max_arity + 1u;
    bool backend_static_arity = stage >= XI_STAGE_BACKEND &&
                                (desc->flags & XA_INTRINSIC_FLAG_STATIC_RECEIVER) != 0 &&
                                value->nargs >= desc->min_arity && value->nargs <= desc->max_arity;
    bool backend_encoded_shuffle = stage >= XI_STAGE_BACKEND &&
                                   (desc->flags & XA_INTRINSIC_FLAG_EXPLICIT_SHUFFLE) != 0 &&
                                   value->nargs == 1;
    bool lowered_parallel_arity = desc->family == XA_INTRINSIC_FAMILY_PARALLEL &&
                                  value->nargs >= (expected == XI_PAR_FOR ? 4u : 5u);
    if (!backend_static_arity && !backend_encoded_shuffle && !lowered_parallel_arity &&
        (value->nargs < min_nargs || value->nargs > max_nargs))
        return set_error(error, error_size, "canonical intrinsic id %u has invalid Xi arity %u",
                         value->xa_intrinsic_id, value->nargs);

    if (desc->family == XA_INTRINSIC_FAMILY_TARGET) {
        if (value->aux_int != 0)
            return set_error(error, error_size, "canonical intrinsic id %u has unexpected shape %u",
                             value->xa_intrinsic_id, (unsigned) value->aux_int);
    } else if (desc->family == XA_INTRINSIC_FAMILY_BITS) {
        if (!semantic_intrinsic_native_is_integer(value->aux_int))
            return set_error(error, error_size,
                             "canonical intrinsic id %u has invalid integer width %u",
                             value->xa_intrinsic_id, (unsigned) value->aux_int);
    } else if (desc->family == XA_INTRINSIC_FAMILY_SIMD) {
        if (!xi_vec_shape_is_explicit(value->aux_int) ||
            xi_vec_shape_native_type(value->aux_int) != desc->shape_rule.result_native_type ||
            xi_vec_shape_lanes(value->aux_int) != desc->shape_rule.result_lanes)
            return set_error(error, error_size,
                             "canonical intrinsic id %u has invalid vector shape %u",
                             value->xa_intrinsic_id, (unsigned) value->aux_int);

        bool expects_odd = (desc->flags & XA_INTRINSIC_FLAG_ODD_LANES) != 0;
        bool has_odd = (value->aux_int & XI_VEC_SHAPE_ODD_LANES) != 0;
        if (expects_odd != has_odd)
            return set_error(error, error_size,
                             "canonical intrinsic id %u has invalid odd-lane flag %u",
                             value->xa_intrinsic_id, has_odd ? 1u : 0u);
        bool expects_unzip = (desc->flags & XA_INTRINSIC_FLAG_UNZIP) != 0;
        bool has_unzip = (value->aux_int & XI_VEC_SHAPE_UNZIP) != 0;
        if (expects_unzip != has_unzip)
            return set_error(error, error_size,
                             "canonical intrinsic id %u has invalid unzip flag %u",
                             value->xa_intrinsic_id, has_unzip ? 1u : 0u);
        bool expects_half = (desc->flags & XA_INTRINSIC_FLAG_CONTIGUOUS_HALF) != 0;
        bool has_half = (value->aux_int & XI_VEC_SHAPE_CONTIGUOUS_HALF) != 0;
        if (expects_half != has_half)
            return set_error(error, error_size,
                             "canonical intrinsic id %u has invalid contiguous-half flag %u",
                             value->xa_intrinsic_id, has_half ? 1u : 0u);
        if (has_unchecked_access && value->op != XI_VEC_LOAD && value->op != XI_VEC_STORE)
            return set_error(error, error_size,
                             "canonical intrinsic id %u has unchecked non-memory vector op %u",
                             value->xa_intrinsic_id, (unsigned) value->op);
    } else if (desc->family == XA_INTRINSIC_FAMILY_MEMORY) {
        if (desc->lowering == XA_INTRINSIC_LOWERING_SLICE_REINTERPRET && value->aux_int == 0)
            return set_error(error, error_size,
                             "canonical intrinsic id %u lacks reinterpret element metadata",
                             value->xa_intrinsic_id);
    } else if (desc->family == XA_INTRINSIC_FAMILY_ATOMIC) {
        if (!value->args || !value->args[0] || !value->args[0]->type ||
            !xr_type_is_named_class(value->args[0]->type, "Atomic"))
            return set_error(error, error_size,
                             "canonical atomic intrinsic id %u lacks Atomic<T> receiver",
                             value->xa_intrinsic_id);
    } else if (desc->family == XA_INTRINSIC_FAMILY_PARALLEL) {
        XiAuxKind expected_aux = value->op == XI_PAR_FOR      ? XI_AUX_KIND_PAR_FOR
                                 : value->op == XI_PAR_MAP    ? XI_AUX_KIND_PAR_MAP
                                 : value->op == XI_PAR_REDUCE ? XI_AUX_KIND_PAR_REDUCE
                                                              : XI_AUX_KIND_NONE;
        if (expected_aux == XI_AUX_KIND_NONE || value->aux_kind != expected_aux || !value->aux)
            return set_error(error, error_size,
                             "canonical parallel intrinsic id %u has invalid lowering contract",
                             value->xa_intrinsic_id);
    } else {
        return set_error(error, error_size, "canonical intrinsic id %u has unsupported family %u",
                         value->xa_intrinsic_id, (unsigned) desc->family);
    }

    if (value->flags != xi_op_default_effects(value->op))
        return set_error(error, error_size, "canonical intrinsic id %u has invalid effect flags %u",
                         value->xa_intrinsic_id, value->flags);
    return true;
}
