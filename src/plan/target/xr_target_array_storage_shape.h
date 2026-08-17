/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_array_storage_shape.h - Machine storage of an array element
 *
 * KEY CONCEPT:
 *   The plan states an array's element storage as an XR_ELEM_* class; the
 *   target has to name the machine layout that holds it. That translation, and
 *   the question of which element types earn a layout at all, are two facts the
 *   target builder and the target verifier both need. Stated once here, they
 *   cannot drift into a builder that lays out a case the verifier refuses, or
 *   the reverse.
 *
 *   The element roster itself is not restated: it comes from the semantic
 *   shape, so the plan and the target agree by construction on which types are
 *   scalar. The target admits strictly less than that roster, and the one
 *   narrowing is stated below rather than left implicit in a second table.
 */

#ifndef XR_TARGET_ARRAY_STORAGE_SHAPE_H
#define XR_TARGET_ARRAY_STORAGE_SHAPE_H

#include "xr_target_plan.h"
#include "../semantic/xr_semantic_array_element_storage_shape.h"
#include <stdbool.h>
#include <stdint.h>

/* Translate the plan's frozen element class into the machine storage that
 * holds it. A class with no machine layout — XR_ELEM_ANY above all, which the
 * plan uses for every reference element — is refused rather than given one. */
static inline bool xr_target_array_storage_from_semantic(uint8_t storage, uint8_t *out) {
    if (!out)
        return false;
    switch (storage) {
        case XR_ELEM_I8:
            *out = XR_TARGET_ARRAY_STORAGE_I8;
            return true;
        case XR_ELEM_U8:
            *out = XR_TARGET_ARRAY_STORAGE_U8;
            return true;
        case XR_ELEM_I16:
            *out = XR_TARGET_ARRAY_STORAGE_I16;
            return true;
        case XR_ELEM_U16:
            *out = XR_TARGET_ARRAY_STORAGE_U16;
            return true;
        case XR_ELEM_I32:
            *out = XR_TARGET_ARRAY_STORAGE_I32;
            return true;
        case XR_ELEM_U32:
            *out = XR_TARGET_ARRAY_STORAGE_U32;
            return true;
        case XR_ELEM_I64:
            *out = XR_TARGET_ARRAY_STORAGE_I64;
            return true;
        case XR_ELEM_U64:
            *out = XR_TARGET_ARRAY_STORAGE_U64;
            return true;
        case XR_ELEM_F32:
            *out = XR_TARGET_ARRAY_STORAGE_F32;
            return true;
        case XR_ELEM_F64:
            *out = XR_TARGET_ARRAY_STORAGE_F64;
            return true;
        case XR_ELEM_BOOL:
            *out = XR_TARGET_ARRAY_STORAGE_BOOL;
            return true;
        case XR_ELEM_RUNE:
            *out = XR_TARGET_ARRAY_STORAGE_RUNE;
            return true;
        default:
            return false;
    }
}

/* The machine storage an element type earns, or a refusal.
 *
 * The target narrows the semantic roster by one rule: a scalar representation
 * only names a layout when the kind carrying it is numeric. A bool or rune is
 * its own layout and states no representation, so one that carries an integer
 * representation anyway describes a type this target will not lay out, and
 * neither will any other kind that borrows a numeric representation. */
static inline bool xr_target_array_storage_from_type(const XrSemanticTypeRecord *type,
                                                     uint8_t *out) {
    uint8_t element = xr_semantic_array_element_storage(type);
    if (element != XR_ELEM_BOOL && element != XR_ELEM_RUNE && type && type->kind != XR_KIND_INT &&
        type->kind != XR_KIND_FLOAT)
        return false;
    return xr_target_array_storage_from_semantic(element, out);
}

#endif  // XR_TARGET_ARRAY_STORAGE_SHAPE_H
