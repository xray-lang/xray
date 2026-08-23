/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_number_parse_error_shape.h - Exact NumberParseError builtin authority
 */

#ifndef XR_SEMANTIC_NUMBER_PARSE_ERROR_SHAPE_H
#define XR_SEMANTIC_NUMBER_PARSE_ERROR_SHAPE_H

#include "../../base/xnumber_parse_error.h"
#include "xr_semantic_enum_shape.h"
#include "xr_semantic_ids.h"
#include "xr_semantic_panic_catch_shape.h"
#include <stdio.h>
#include <string.h>

static inline bool xr_semantic_number_parse_error_type_is_exact(
    const XrSemanticPlan *plan, uint32_t type_index) {
    const XrNumberParseErrorRegistryRow *row =
        xr_number_parse_error_registry_row(XR_GLOBAL_VAR_NUMBER_PARSE_ERROR);
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, type_index);
    char expected_key[256];
    XrStableId expected_identity = {{0}};
    XrFingerprint digest = {{0}};
    int written =
        row ? snprintf(expected_key, sizeof(expected_key),
                       "source-enum-v1:schema=%u:owner=%u:%s:name=%u:%s:members=2:"
                       "m0=%u:%s:payloads=0:m1=%u:%s:payloads=0",
                       (unsigned) XR_SEMANTIC_SCHEMA_VERSION,
                       (unsigned) strlen(row->nominal_owner), row->nominal_owner,
                       (unsigned) strlen(row->enum_name), row->enum_name,
                       (unsigned) strlen(row->members[XR_NUMBER_PARSE_ERROR_INVALID_SYNTAX]),
                       row->members[XR_NUMBER_PARSE_ERROR_INVALID_SYNTAX],
                       (unsigned) strlen(row->members[XR_NUMBER_PARSE_ERROR_OUT_OF_RANGE]),
                       row->members[XR_NUMBER_PARSE_ERROR_OUT_OF_RANGE])
            : -1;
    return plan && row && type && written > 0 && (size_t) written < sizeof(expected_key) &&
           xr_semantic_unit_enum_type_is_exact(type) &&
           type->enum_layout_id == row->enum_layout_id &&
           type->enum_member_count == XR_NUMBER_PARSE_ERROR_MEMBER_COUNT &&
           strcmp(type->source_enum_key, expected_key) == 0 &&
           xr_stable_id_from_key(expected_key, &expected_identity, &digest) &&
           xr_stable_id_equal(type->source_enum_identity, expected_identity);
}

/* The namespace load is exact in both directions: id 30 selects the sole row,
 * while metadata and the class type must independently identify that same row.
 * A wrong id, wrong type, or missing metadata therefore refuses authority. */
static inline bool xr_semantic_number_parse_error_namespace_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    const XrNumberParseErrorRegistryRow *row =
        operation && operation->semantic_immediate >= 0 &&
                operation->semantic_immediate <= UINT32_MAX
            ? xr_number_parse_error_registry_row(
                  (uint32_t) operation->semantic_immediate)
            : NULL;
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    char expected_type_key[192];
    int written =
        row ? snprintf(expected_type_key, sizeof(expected_type_key),
                       "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;named:%u:%s[0]",
                       (unsigned) XR_KIND_CLASS, (unsigned) XR_TID_NULL,
                       (unsigned) XR_SCALAR_REP_NONE, (unsigned) strlen(row->enum_name),
                       row->enum_name)
            : -1;
    XrStableId zero = {{0}};
    return plan && operation && row && type && metadata && written > 0 &&
           (size_t) written < sizeof(expected_type_key) && operation->opcode == XI_GET_BUILTIN &&
           operation->operand_count == 0 && operation->metadata_count == 1 &&
           operation->metadata_begin < metadata_count && metadata[operation->metadata_begin] &&
           strcmp(metadata[operation->metadata_begin], row->enum_name) == 0 &&
           operation->auxiliary_kind == XI_AUX_KIND_NONE &&
           operation->semantic_immediate == (int64_t) row->global_index &&
           operation->constant == XR_SEMANTIC_INDEX_NONE &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
           operation->effects == xi_generated_op_effects(XI_GET_BUILTIN) &&
           operation->flags == xi_generated_op_default_flags(XI_GET_BUILTIN) &&
           operation->result_alias_operand == -1 && type->kind == XR_KIND_CLASS &&
           type->builtin_type == XR_TID_NULL && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) && type->canonical_key &&
           strcmp(type->canonical_key, expected_type_key) == 0;
}

static inline const XrSemanticOperationRecord *
xr_semantic_number_parse_error_value_definition(const XrSemanticPlan *plan,
                                                 uint32_t semantic_value) {
    const XrSemanticOperationRecord *definition = NULL;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->result_value != semantic_value)
            continue;
        if (definition)
            return NULL;
        definition = candidate;
    }
    return definition;
}

/* Member selection carries a frozen integer ordinal. The source spelling was
 * consumed while binding; neither SemanticPlan nor AOT recovers the selected
 * variant from a field-name string. */
static inline bool xr_semantic_number_parse_error_member_access_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *namespace_value, uint32_t *member_index_value, uint32_t *member_index) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (namespace_value)
        *namespace_value = XR_SEMANTIC_INDEX_NONE;
    if (member_index_value)
        *member_index_value = XR_SEMANTIC_INDEX_NONE;
    if (member_index)
        *member_index = UINT32_MAX;
    if (!plan || !operation || !operands || operation->opcode != XI_INDEX_GET ||
        operation->operand_count != 2 || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 0 || operation->auxiliary_kind != XI_AUX_KIND_ENUM_CASE ||
        operation->semantic_immediate != 0 || operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_INDEX_GET) ||
        operation->flags != xi_generated_op_default_flags(XI_INDEX_GET) ||
        operation->ownership_use != xi_generated_op_own_use(XI_INDEX_GET) ||
        operation->result_alias_operand != -1 ||
        !xr_semantic_number_parse_error_type_is_exact(plan, operation->result_type))
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *index = receiver + 1;
    const XrSemanticOperationRecord *receiver_definition =
        xr_semantic_number_parse_error_value_definition(plan, receiver->value);
    const XrSemanticOperationRecord *index_definition =
        xr_semantic_number_parse_error_value_definition(plan, index->value);
    const XrSemanticConstantRecord *constant =
        index_definition && index_definition->constant != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_constant(plan, index_definition->constant)
            : NULL;
    const XrSemanticTypeRecord *index_type = xr_semantic_plan_type(plan, index->type);
    if (!receiver_definition || !index_definition || !constant || !index_type ||
        receiver_definition->function != operation->function ||
        index_definition->function != operation->function ||
        !xr_semantic_number_parse_error_namespace_is_exact(plan, receiver_definition) ||
        index_definition->opcode != XI_CONST || constant->kind != XR_SEM_CONST_INT ||
        constant->integer < 0 || constant->integer >= XR_NUMBER_PARSE_ERROR_MEMBER_COUNT ||
        index_type->kind != XR_KIND_INT || index_type->scalar_rep != XR_NATIVE_I64 ||
        receiver->role != XR_SEM_OPERAND_VALUE || receiver->parameter != -1 ||
        receiver->flags != 0 || index->role != XR_SEM_OPERAND_VALUE || index->parameter != -1 ||
        index->flags != 0)
        return false;
    if (namespace_value)
        *namespace_value = receiver->value;
    if (member_index_value)
        *member_index_value = index->value;
    if (member_index)
        *member_index = (uint32_t) constant->integer;
    return true;
}

/* Typed error clauses lower as ERR_CATCH(any) -> IS(namespace) -> AS(enum).
 * This predicate accepts only the NumberParseError narrowing form and proves
 * its result type from the source-enum identity rather than from the AS name. */
static inline bool xr_semantic_number_parse_error_catch_narrow_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *caught_value) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    const XrNumberParseErrorRegistryRow *row =
        xr_number_parse_error_registry_row(XR_GLOBAL_VAR_NUMBER_PARSE_ERROR);
    if (caught_value)
        *caught_value = XR_SEMANTIC_INDEX_NONE;
    if (!plan || !operation || !operands || !metadata || !row || operation->opcode != XI_AS ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        !metadata[operation->metadata_begin] ||
        strcmp(metadata[operation->metadata_begin], row->enum_name) != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->semantic_immediate != ((int64_t) UINT32_MAX << 1) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_AS) ||
        operation->flags != xi_generated_op_default_flags(XI_AS) ||
        operation->ownership_use != xi_generated_op_own_use(XI_AS) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->result_alias_operand != -1 ||
        !xr_semantic_number_parse_error_type_is_exact(plan, operation->result_type))
        return false;
    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    const XrSemanticOperationRecord *definition = NULL;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->result_value != source->value)
            continue;
        if (definition)
            return false;
        definition = candidate;
    }
    if (!definition || definition->function != operation->function ||
        definition->opcode != XI_ERR_CATCH || source->type != definition->result_type ||
        source->role != XR_SEM_OPERAND_VALUE || source->parameter != -1 || source->flags != 0 ||
        !xr_semantic_panic_catch_is_exact(plan, definition))
        return false;
    if (caught_value)
        *caught_value = source->value;
    return true;
}

/* Catch dispatch compares one exact typed error with one stable builtin case.
 * Both Xi values carry the compact declaration ordinal after their own typed
 * operations have consumed the tagged namespace/error carriers. */
static inline bool xr_semantic_number_parse_error_equality_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const XrSemanticTypeRecord *result_type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (!plan || !operation || !operands || !result_type || operation->opcode != XI_EQ ||
        operation->operand_count != 2 || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 0 || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->semantic_immediate != 0 || operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->effects != xi_generated_op_effects(XI_EQ) ||
        operation->flags != xi_generated_op_default_flags(XI_EQ) ||
        operation->result_alias_operand != -1 || result_type->kind != XR_KIND_BOOL ||
        result_type->child_count != 0)
        return false;
    const XrSemanticOperandRecord *left = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *right = left + 1;
    const XrSemanticOperationRecord *left_definition =
        xr_semantic_number_parse_error_value_definition(plan, left->value);
    const XrSemanticOperationRecord *right_definition =
        xr_semantic_number_parse_error_value_definition(plan, right->value);
    bool left_caught = left_definition &&
                       xr_semantic_number_parse_error_catch_narrow_is_exact(
                           plan, left_definition, NULL);
    bool right_caught = right_definition &&
                        xr_semantic_number_parse_error_catch_narrow_is_exact(
                            plan, right_definition, NULL);
    bool left_member = left_definition &&
                       xr_semantic_number_parse_error_member_access_is_exact(
                           plan, left_definition, NULL, NULL, NULL);
    bool right_member = right_definition &&
                        xr_semantic_number_parse_error_member_access_is_exact(
                            plan, right_definition, NULL, NULL, NULL);
    return left_definition && right_definition &&
           left_definition->function == operation->function &&
           right_definition->function == operation->function &&
           left_definition->result_type == left->type &&
           right_definition->result_type == right->type && left->type == right->type &&
           xr_semantic_number_parse_error_type_is_exact(plan, left->type) &&
           left->role == XR_SEM_OPERAND_VALUE && left->parameter == -1 && left->flags == 0 &&
           right->role == XR_SEM_OPERAND_VALUE && right->parameter == -1 && right->flags == 0 &&
           ((left_caught && right_member) || (right_caught && left_member));
}

#endif /* XR_SEMANTIC_NUMBER_PARSE_ERROR_SHAPE_H */
