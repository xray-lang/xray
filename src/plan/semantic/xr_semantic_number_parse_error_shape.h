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

static inline bool xr_semantic_number_parse_error_namespace_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
    return operation && operation->semantic_immediate == XR_GLOBAL_VAR_NUMBER_PARSE_ERROR &&
           xr_semantic_builtin_enum_namespace_is_exact(plan, operation);
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

#endif /* XR_SEMANTIC_NUMBER_PARSE_ERROR_SHAPE_H */
