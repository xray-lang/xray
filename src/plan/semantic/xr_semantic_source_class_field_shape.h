/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_source_class_field_shape.h - Exact source-class field results
 *
 * KEY CONCEPT:
 *   LOAD_FIELD does not choose storage from its selector.  A source-class
 *   field result is accepted only when the serialized operation names an
 *   evidence-backed class-layout field, borrows from an exact source-class
 *   receiver, uniquely defines its result, and the result type belongs to a
 *   closed carrier roster.  Consumers remain orthogonal: a call separately
 *   proves its parameter contract and then consumes the storage established
 *   here.
 */

#ifndef XR_SEMANTIC_SOURCE_CLASS_FIELD_SHAPE_H
#define XR_SEMANTIC_SOURCE_CLASS_FIELD_SHAPE_H

#include "xr_semantic_array_type_shape.h"
#include "xr_semantic_class_shape.h"
#include "xr_semantic_enum_shape.h"
#include "xr_semantic_string_shape.h"

typedef enum XrSemanticSourceClassFieldResultCarrier {
    XR_SEM_SOURCE_CLASS_FIELD_RESULT_NONE = 0,
    XR_SEM_SOURCE_CLASS_FIELD_RESULT_BORROWED_TAGGED,
} XrSemanticSourceClassFieldResultCarrier;

/* Every managed field family uses the same closed carrier roster.  The field
 * producer proves how the value was selected; this type judgement proves only
 * that the selected value has the runtime's single tagged representation. */
static inline bool xr_semantic_managed_field_result_type_is_exact(const XrSemanticPlan *plan,
                                                                  uint32_t semantic_type) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, semantic_type);
    return xr_semantic_tagged_string_type_is_exact(type) ||
           xr_semantic_array_type_row_is_exact(type) ||
           xr_semantic_class_instance_type_source_class(plan, type) != XR_SEMANTIC_INDEX_NONE ||
           xr_semantic_adt_enum_type_is_exact(type);
}

/* An evidence-backed field selection on an exact source-class receiver.  The
 * field id is the pointer-free Xg class-layout identity stamped by lowering;
 * the metadata row preserves the source selector for diagnostics and stable
 * serialization.  Neither is inferred from the other, and neither is replaced
 * with a selector list here. */
static inline bool
xr_semantic_source_class_field_read_is_exact(const XrSemanticPlan *plan,
                                             const XrSemanticOperationRecord *operation,
                                             uint32_t *source_class) {
    const XrSemanticOperandRecord *receiver =
        xr_semantic_class_field_read_receiver_is_exact(plan, operation);
    uint32_t receiver_class = receiver ? xr_semantic_class_instance_type_source_class(
                                             plan, xr_semantic_plan_type(plan, receiver->type))
                                       : XR_SEMANTIC_INDEX_NONE;
    uint32_t metadata_count = 0;
    const char *const *metadata = plan ? xr_semantic_plan_metadata(plan, &metadata_count) : NULL;
    bool evidence_exact = operation && operation->evidence[0] == 0 && operation->evidence[1] == 0 &&
                          operation->evidence[2] == 0 && operation->evidence[3] == 0 &&
                          operation->evidence[4] == 0 && operation->evidence[5] != 0 &&
                          operation->evidence[6] == 0 &&
                          operation->evidence[7] == XR_SEMANTIC_INDEX_NONE;
    bool metadata_exact = operation && metadata && operation->metadata_count == 1 &&
                          operation->metadata_begin < metadata_count &&
                          metadata[operation->metadata_begin] &&
                          metadata[operation->metadata_begin][0] != '\0';
    if (receiver_class == XR_SEMANTIC_INDEX_NONE || !evidence_exact || !metadata_exact ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        !xr_semantic_plan_type(plan, operation->result_type) ||
        xr_semantic_unique_value_definition(plan, operation->result_value) != operation)
        return false;
    if (source_class)
        *source_class = receiver_class;
    return true;
}

/* The closed tagged-result roster is structural and result-type driven.  It
 * admits every currently exact managed XrValue carrier -- String, Array,
 * source-class instance, and source ADT enum -- without depending on a class,
 * selector, field name, or consumer.  Scalar, nullable, and aggregate field
 * results remain owned by their orthogonal type-storage families. */
static inline bool
xr_semantic_source_class_field_result_carrier_is_exact(const XrSemanticPlan *plan,
                                                       const XrSemanticOperationRecord *operation,
                                                       uint8_t *result_carrier) {
    if (!xr_semantic_source_class_field_read_is_exact(plan, operation, NULL))
        return false;
    if (!xr_semantic_managed_field_result_type_is_exact(plan, operation->result_type))
        return false;
    if (result_carrier)
        *result_carrier = XR_SEM_SOURCE_CLASS_FIELD_RESULT_BORROWED_TAGGED;
    return true;
}

#endif /* XR_SEMANTIC_SOURCE_CLASS_FIELD_SHAPE_H */
