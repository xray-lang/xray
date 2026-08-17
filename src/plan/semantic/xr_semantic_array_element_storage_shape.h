/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_array_element_storage_shape.h - Element storage of an array type
 *
 * KEY CONCEPT:
 *   An array element is stored unboxed only when its type is a plain scalar:
 *   no builtin class, no children, no aggregate extent or alignment, and no
 *   type flags at all. Every other element — anything reference-capable above
 *   all — is stored as a tagged value, which XR_ELEM_ANY names.
 *
 *   Which types qualify is one fact, and answering it in two places is how the
 *   two answers drift apart. They already had: one copy admitted a builtin
 *   class, an aggregate, and a bool or rune carrying a scalar representation
 *   that the other copy refused, so a plan the producer would never emit could
 *   still pass verification. The judgement is stated once here instead.
 *
 *   XR_ELEM_ANY is the fail-closed answer, returned both for "this element is a
 *   reference" and for "this type states no storage". Callers that need those
 *   apart must ask a further question; none may read XR_ELEM_ANY as permission.
 */

#ifndef XR_SEMANTIC_ARRAY_ELEMENT_STORAGE_SHAPE_H
#define XR_SEMANTIC_ARRAY_ELEMENT_STORAGE_SHAPE_H

#include "xr_semantic_plan.h"
#include "../../runtime/value/xtype.h"
#include "../../shared/xr_elem_type.h"
#include <stdint.h>

static inline uint8_t xr_semantic_array_element_storage(const XrSemanticTypeRecord *type) {
    if (!type || type->builtin_type != XR_TID_NULL || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 || type->flags != 0)
        return XR_ELEM_ANY;
    /* A bool or rune is its own storage class and carries no scalar
     * representation; one that does is a different type than this row names. */
    if (type->kind == XR_KIND_BOOL && type->scalar_rep == XR_SCALAR_REP_NONE)
        return XR_ELEM_BOOL;
    if (type->kind == XR_KIND_RUNE && type->scalar_rep == XR_SCALAR_REP_NONE)
        return XR_ELEM_RUNE;
    switch (type->scalar_rep) {
        case XR_NATIVE_I8:
            return XR_ELEM_I8;
        case XR_NATIVE_U8:
            return XR_ELEM_U8;
        case XR_NATIVE_I16:
            return XR_ELEM_I16;
        case XR_NATIVE_U16:
            return XR_ELEM_U16;
        case XR_NATIVE_I32:
            return XR_ELEM_I32;
        case XR_NATIVE_U32:
            return XR_ELEM_U32;
        case XR_NATIVE_I64:
            return XR_ELEM_I64;
        case XR_NATIVE_U64:
            return XR_ELEM_U64;
        case XR_NATIVE_F32:
            return XR_ELEM_F32;
        case XR_NATIVE_F64:
            return XR_ELEM_F64;
        default:
            return XR_ELEM_ANY;
    }
}

#endif  // XR_SEMANTIC_ARRAY_ELEMENT_STORAGE_SHAPE_H
