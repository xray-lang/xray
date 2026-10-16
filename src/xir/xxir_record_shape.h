/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_record_shape.h - Owned canonical fields with independent source ordering
 *
 * KEY CONCEPT:
 *   Sorting field identities never changes expression evaluation order.
 */
#ifndef XXIR_RECORD_SHAPE_H
#define XXIR_RECORD_SHAPE_H
#include "xxir.h"
typedef struct XrXirRecordField {
    XrXirLiteral name;
    XrXirType type;
} XrXirRecordField;
typedef struct XrXirRecordShapeInput {
    const XrXirTypes *types;
    const XrXirRecordField *fields;
    uint32_t count, parameter_count;
} XrXirRecordShapeInput;
typedef struct XrXirRecordShape {
    const XrXirRecordField *fields;
    const uint32_t *source_to_field;
    uint32_t count;
} XrXirRecordShape;
/* Inputs borrow a previously verified, unchanged type pool. The result owns all
 * names and ordinals, but its type IDs still belong to that pool. No execution,
 * structural-constraint, equality, Sendable or interface authority is granted.
 * Names are strict UTF-8 identifier spellings, ordered by unsigned UTF-8 bytes.
 * Duplicate names fail regardless of field type. Failure preserves output and
 * charges all completed work to the original ledger without refunds. */
XR_FUNC XrXirStatus xr_xir_compile_record_shape_new(const XrXirCompileContext *context,
    const XrXirRecordShapeInput *input, XrXirRecordShape **output);
static inline void xr_xir_compile_record_shape_free(XrXirRecordShape *shape) {
    xr_compile_resources_free(shape);
}
#endif // XXIR_RECORD_SHAPE_H
