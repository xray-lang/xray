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
#include "xr_semantic_native_leaf_shape.h"
#include "xr_semantic_string_shape.h"
#include "xr_semantic_value_aggregate_shape.h"

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
           xr_semantic_adt_enum_type_is_exact(type) ||
           xr_semantic_source_structural_shape_is_exact(plan, semantic_type);
}

/* An evidence-backed field selection on an exact source-class receiver.  The
 * field id is the pointer-free Xg class-layout identity stamped by lowering;
 * the metadata row preserves the source selector for diagnostics and stable
 * serialization.  Neither is inferred from the other, and neither is replaced
 * with a selector list here. */
static inline bool
xr_semantic_source_class_field_read_shape_is_exact(const XrSemanticPlan *plan,
                                                   const XrSemanticOperationRecord *operation) {
    const XrSemanticOperandRecord *receiver =
        xr_semantic_class_field_read_receiver_is_exact(plan, operation);
    const XrSemanticTypeRecord *receiver_type =
        receiver ? xr_semantic_plan_type(plan, receiver->type) : NULL;
    bool receiver_exact =
        xr_semantic_class_instance_type_source_class(plan, receiver_type) != XR_SEMANTIC_INDEX_NONE ||
        xr_semantic_external_class_instance_type_is_exact(receiver_type);
    if (!receiver_exact ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        !xr_semantic_plan_type(plan, operation->result_type) ||
        xr_semantic_unique_value_definition(plan, operation->result_value) != operation)
        return false;
    return true;
}

/* Local consumers require a local declaration index; program consumers may
 * resolve an external receiver's stable class identity in its owning module. */
static inline bool
xr_semantic_source_class_field_read_is_exact(const XrSemanticPlan *plan,
                                             const XrSemanticOperationRecord *operation,
                                             uint32_t *source_class) {
    if (!xr_semantic_source_class_field_read_shape_is_exact(plan, operation))
        return false;
    const XrSemanticOperandRecord *receiver =
        xr_semantic_class_field_read_receiver_is_exact(plan, operation);
    uint32_t receiver_class = xr_semantic_class_instance_type_source_class(
        plan, xr_semantic_plan_type(plan, receiver->type));
    if (receiver_class == XR_SEMANTIC_INDEX_NONE)
        return false;
    if (source_class)
        *source_class = receiver_class;
    return true;
}

/* A private storage field borrows the same tagged owner as its source wrapper.
 * The frozen wrapper module and generated bridge declaration select the native
 * storage type; an equal field spelling in an unrelated class grants nothing. */
static inline bool xr_semantic_source_native_storage_field_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    const XrSemanticSourceClassRecord *source) {
    const char *module = NULL;
    size_t module_length = 0u;
    if (!source || !source->name || !source->module_path ||
        !xr_module_identity_stdlib_namespace(source->module_path, &module, &module_length))
        return false;
    const XrStdlibNativeClassDefEntry *storage = xr_stdlib_metadata_unique_source_provider_span(
        module, module_length, source->name, strlen(source->name));
    uint32_t metadata_count = 0u;
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    return storage && storage->source_storage_field && operation && metadata &&
           operation->metadata_count == 1u && operation->metadata_begin < metadata_count &&
           metadata[operation->metadata_begin] &&
           strcmp(metadata[operation->metadata_begin], storage->source_storage_field) == 0 &&
           xr_semantic_native_direct_storage_type_matches(
               storage->module, storage->name, strlen(storage->name),
               xr_semantic_plan_type(plan, operation->result_type), false, NULL);
}

/* Ordinary managed results use their structural carrier judgement. Private
 * native storage additionally requires its exact generated wrapper bridge.
 * Scalar, nullable, and aggregate field results retain their own storage
 * families; a consuming native call still proves its separate signature. */
static inline bool
xr_semantic_source_class_field_result_carrier_is_exact(const XrSemanticPlan *plan,
                                                       const XrSemanticOperationRecord *operation,
                                                       uint8_t *result_carrier) {
    uint32_t source_class = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_source_class_field_read_is_exact(plan, operation, &source_class))
        return false;
    if (!xr_semantic_managed_field_result_type_is_exact(plan, operation->result_type) &&
        !xr_semantic_source_native_storage_field_is_exact(
            plan, operation, xr_semantic_plan_source_class(plan, source_class)))
        return false;
    if (result_carrier)
        *result_carrier = XR_SEM_SOURCE_CLASS_FIELD_RESULT_BORROWED_TAGGED;
    return true;
}

/* An inlined dependency body keeps the receiver's stable declaration identity.
 * Resolve it against the actual dependency, never the caller's local indexes. */
static inline bool xr_semantic_imported_native_storage_field_is_exact(
    const XrSemanticPlan *caller, const XrSemanticPlan *dependency,
    const XrSemanticOperationRecord *operation) {
    if (!dependency || !xr_semantic_source_class_field_read_shape_is_exact(caller, operation))
        return false;
    const XrSemanticOperandRecord *receiver =
        xr_semantic_class_field_read_receiver_is_exact(caller, operation);
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(caller, receiver->type);
    if (!xr_semantic_external_class_instance_type_is_exact(type))
        return false;
    const XrSemanticSourceClassRecord *match = NULL;
    for (uint32_t index = 0u; index < xr_semantic_plan_source_class_count(dependency); ++index) {
        const XrSemanticSourceClassRecord *source = xr_semantic_plan_source_class(dependency, index);
        if (!source || !xr_stable_id_equal(source->id, type->source_class_identity))
            continue;
        if (match || !xr_semantic_class_declaration_is_frozen(dependency, index))
            return false;
        bool exact_type = false;
        for (uint32_t row = 0u; row < xr_semantic_plan_type_count(dependency); ++row) {
            const XrSemanticTypeRecord *declared_type = xr_semantic_plan_type(dependency, row);
            if (declared_type && xr_stable_id_equal(declared_type->id, type->id) &&
                xr_semantic_class_instance_type_source_class(dependency, declared_type) == index)
                exact_type = true;
        }
        if (!exact_type)
            return false;
        match = source;
    }
    return match && xr_semantic_source_native_storage_field_is_exact(caller, operation, match);
}

#endif /* XR_SEMANTIC_SOURCE_CLASS_FIELD_SHAPE_H */
