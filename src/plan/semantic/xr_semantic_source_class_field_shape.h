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
#include "xr_semantic_dynamic_value_shape.h"
#include "xr_semantic_channel_type_shape.h"
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
    return xr_semantic_atomic_type_is_exact(plan, type) ||
           xr_semantic_tagged_string_type_is_exact(type) ||
           xr_semantic_nullable_tagged_string_type_is_exact(type) ||
           xr_semantic_array_type_row_is_exact(type) ||
           xr_semantic_channel_type_row_is_exact(plan, type) ||
           xr_semantic_class_instance_type_source_class(plan, type) != XR_SEMANTIC_INDEX_NONE ||
           xr_semantic_external_class_instance_type_is_exact(type) ||
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

/* Join the serialized selector to the owning class declaration.
 * Imported reads use the dependency's declaration and the caller's selector. */
static inline bool xr_semantic_source_field_selector_is_exact(
    const XrSemanticPlan *caller, const XrSemanticPlan *owner,
    const XrSemanticOperationRecord *read, const XrSemanticSourceClassRecord *source) {
    uint32_t caller_count = 0u, owner_count = 0u;
    const char *const *caller_metadata = xr_semantic_plan_metadata(caller, &caller_count);
    const char *const *owner_metadata = xr_semantic_plan_metadata(owner, &owner_count);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(owner);
    if (!source || !read || read->semantic_immediate < 0 || !caller_metadata ||
        !owner_metadata || read->metadata_count != 1u ||
        read->metadata_begin >= caller_count || !caller_metadata[read->metadata_begin])
        return false;
    const XrSemanticOperationRecord *match = NULL;
    for (uint32_t i = 0u; i < operation_count; ++i) {
        const XrSemanticOperationRecord *declaration = xr_semantic_plan_operation(owner, i);
        if (declaration->opcode != XI_CLASS_CREATE ||
            xr_semantic_class_object_source_class(owner, declaration) != source->ordinal)
            continue;
        if (match || declaration->metadata_begin > owner_count ||
            declaration->metadata_count > owner_count - declaration->metadata_begin ||
            declaration->metadata_count < 4u + source->method_count)
            return false;
        uint32_t fields = declaration->metadata_count - 4u - source->method_count;
        uint32_t matches = 0u;
        for (uint32_t field = 0u; field < fields; ++field) {
            const char *name = owner_metadata[declaration->metadata_begin + 4u + field];
            if (name && strcmp(name, caller_metadata[read->metadata_begin]) == 0)
                ++matches;
        }
        if (matches != 1u)
            return false;
        match = declaration;
    }
    return match != NULL;
}

/* A native field borrows its declared storage owner. The field layout is
 * proved by the caller; the owning stdlib namespace and unique generated
 * native declaration prove the carrier, independently of wrapper count. */
static inline bool xr_semantic_source_native_storage_field_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    const XrSemanticSourceClassRecord *source) {
    const char *module = NULL;
    size_t module_length = 0u;
    if (!operation || !source || !source->module_path ||
        !xr_module_identity_stdlib_namespace(source->module_path, &module, &module_length))
        return false;
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, operation->result_type);
    const XrStdlibNativeClassDefEntry *match = NULL;
    for (uint32_t i = 0; i < XR_STDLIB_NATIVE_CLASS_DEF_ENTRY_COUNT; i++) {
        const XrStdlibNativeClassDefEntry *entry = &xr_stdlib_native_class_def_entries[i];
        if (!entry->module || !entry->name || strlen(entry->module) != module_length ||
            memcmp(entry->module, module, module_length) != 0 ||
            !xr_semantic_native_direct_storage_type_matches(
                entry->module, entry->name, strlen(entry->name), type, false, NULL))
            continue;
        if (match)
            return false;
        match = entry;
    }
    return match != NULL;
}

/* Ordinary managed results use their structural carrier judgement. Private
 * native storage additionally requires its owning module and generated declaration.
 * Scalar and other nullable or aggregate field results retain their own storage
 * families; a consuming native call still proves its separate signature. */
static inline bool
xr_semantic_source_class_field_result_carrier_is_exact(const XrSemanticPlan *plan,
                                                       const XrSemanticOperationRecord *operation,
                                                       uint8_t *result_carrier) {
    uint32_t source_class = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_source_class_field_read_is_exact(plan, operation, &source_class))
        return false;
    if (!xr_semantic_managed_field_result_type_is_exact(plan, operation->result_type) &&
        !(xr_semantic_source_field_selector_is_exact(
              plan, plan, operation, xr_semantic_plan_source_class(plan, source_class)) &&
          xr_semantic_source_native_storage_field_is_exact(
              plan, operation, xr_semantic_plan_source_class(plan, source_class))))
        return false;
    if (result_carrier)
        *result_carrier = XR_SEM_SOURCE_CLASS_FIELD_RESULT_BORROWED_TAGGED;
    return true;
}

/* A dependency field read keeps the receiver's stable declaration identity.
 * Resolve it against the actual dependency, never the caller's local indexes. */
static inline bool xr_semantic_imported_class_field_result_is_exact(
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
    return match &&
           (xr_semantic_managed_field_result_type_is_exact(caller, operation->result_type) ||
            (xr_semantic_source_field_selector_is_exact(caller, dependency, operation, match) &&
             xr_semantic_source_native_storage_field_is_exact(caller, operation, match)));
}

#endif /* XR_SEMANTIC_SOURCE_CLASS_FIELD_SHAPE_H */
