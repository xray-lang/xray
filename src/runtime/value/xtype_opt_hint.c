/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_opt_hint.c - Type-derived optimization hints
 *
 * Holds the type-classification optimisation hints. Living in the
 * runtime/value layer lets the analyzer's JIT-metadata pass call into
 * it without pulling a downward analyzer->codegen include. The
 * constant-folding helpers (xr_opt_fold_*) stay in xoptimize.c — those
 * genuinely belong to the codegen layer.
 */

#include "xtype_opt_hint.h"
#include "xtype.h"

XrOptHint xr_opt_get_hint(struct XrType *type) {
    if (!type) return XR_OPT_NONE;

    // Known primitive types - can skip runtime type checks
    if (type->kind == XR_KIND_INT) return XR_OPT_KNOWN_INT;
    if (type->kind == XR_KIND_FLOAT) return XR_OPT_KNOWN_FLOAT;
    if (type->kind == XR_KIND_BOOL) return XR_OPT_KNOWN_BOOL;
    if (type->kind == XR_KIND_STRING) return XR_OPT_KNOWN_STRING;
    if (type->kind == XR_KIND_NULL) return XR_OPT_KNOWN_NULL;

    // Container types - future inline cache candidates
    if (type->kind == XR_KIND_ARRAY) return XR_OPT_INLINE_ARRAY;
    if (type->kind == XR_KIND_MAP) return XR_OPT_INLINE_MAP;
    if (type->kind == XR_KIND_INSTANCE) return XR_OPT_INLINE_FIELD;

    // Non-nullable types can skip null checks
    if (!type->is_nullable && !(type->kind == XR_KIND_NULL)) {
        return XR_OPT_ELIM_NULL_CHECK;
    }

    return XR_OPT_NONE;
}

bool xr_opt_can_unbox_arith(struct XrType *left, struct XrType *right) {
    if (!left || !right) return false;

    bool left_numeric =
        (left->kind == XR_KIND_INT || left->kind == XR_KIND_FLOAT);
    bool right_numeric =
        (right->kind == XR_KIND_INT || right->kind == XR_KIND_FLOAT);

    return left_numeric && right_numeric;
}

bool xr_opt_can_devirt(struct XrType *receiver_type, const char *method) {
    if (!receiver_type || !method) return false;

    // Can devirtualize if receiver is a known class instance
    if (receiver_type->kind == XR_KIND_INSTANCE) {
        return true;
    }

    return false;
}
