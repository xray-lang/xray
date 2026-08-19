/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_array_type_shape.h - The exact member-Array type row
 *
 * KEY CONCEPT:
 *   Array allocation, Array members and the higher-order forms all rest on the
 *   same question: is this type row exactly the compiler-owned Array shape?
 *   The builder and the verifier each used to spell it out, and the two
 *   spellings must agree -- a row one accepts and the other refuses fails as a
 *   plan-level refusal with nothing in it pointing at the disagreement.
 */

#ifndef XR_SEMANTIC_ARRAY_TYPE_SHAPE_H
#define XR_SEMANTIC_ARRAY_TYPE_SHAPE_H

#include "xr_semantic_plan.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>

/* Nullability is a fact about the reference, not about what the array holds:
 * a non-null `Array<T>?` lays its elements out exactly as `Array<T>` does, and
 * a null one has no elements at all. Every answer these rows carry -- element
 * storage above all -- reads the same either way, so the flag is admitted.
 * The canonical key encodes it as a field, so it is matched in both spellings
 * rather than by skipping past it. */
static inline bool xr_semantic_array_type_row_is_exact(const XrSemanticTypeRecord *type) {
    char expected[96];
    char expected_nullable[96];
    int length = snprintf(expected, sizeof(expected),
                          "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;element:", (unsigned) XR_KIND_ARRAY,
                          (unsigned) XR_TID_NULL, (unsigned) XR_SCALAR_REP_NONE);
    int nullable_length =
        snprintf(expected_nullable, sizeof(expected_nullable),
                 "type-v3:%u:0:%u:1:0:0:0:0:0:%u:0:;element:", (unsigned) XR_KIND_ARRAY,
                 (unsigned) XR_TID_NULL, (unsigned) XR_SCALAR_REP_NONE);
    XrStableId zero = {{0}};
    const uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
    if (!type || length <= 0 || (size_t) length >= sizeof(expected) || nullable_length <= 0 ||
        (size_t) nullable_length >= sizeof(expected_nullable))
        return false;
    if (type->kind != XR_KIND_ARRAY || type->builtin_type != XR_TID_NULL ||
        type->child_count != 1 || type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->scalar_rep != XR_SCALAR_REP_NONE ||
        (type->flags & ~(uint8_t) XR_SEM_TYPE_NULLABLE) != required ||
        type->source_class != XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(type->source_class_identity, zero) || !type->canonical_key)
        return false;
    return (type->flags & XR_SEM_TYPE_NULLABLE)
               ? strncmp(type->canonical_key, expected_nullable, (size_t) nullable_length) == 0
               : strncmp(type->canonical_key, expected, (size_t) length) == 0;
}

#endif /* XR_SEMANTIC_ARRAY_TYPE_SHAPE_H */
