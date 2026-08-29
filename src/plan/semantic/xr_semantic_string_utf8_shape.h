/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_string_utf8_shape.h - Exact String UTF-8 static-call authority
 *
 * `string.fromUtf8` and `string.fromUtf8Lossy` lower through the runtime-owned
 * `String` class token.  The selector text is diagnostic metadata; the frozen
 * Xi method symbol and the exact builtin-class producer are the dispatch
 * identity.  Keeping this judgement shared prevents TargetPlan construction
 * and verification from growing two slightly different name-based tables.
 */

#ifndef XR_SEMANTIC_STRING_UTF8_SHAPE_H
#define XR_SEMANTIC_STRING_UTF8_SHAPE_H

#include "xr_semantic_plan.h"
#include "xr_semantic_string_shape.h"
#include "../../base/xglobal_indices.h"
#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include <stdio.h>
#include <string.h>

static inline bool
xr_semantic_string_class_namespace_type_is_exact(const XrSemanticTypeRecord *type) {
    char expected[160];
    int written =
        snprintf(expected, sizeof(expected), "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;named:6:String[0]",
                 (unsigned) XR_KIND_CLASS, (unsigned) XR_TID_NULL, (unsigned) XR_SCALAR_REP_NONE);
    XrStableId zero = {{0}};
    return type && written > 0 && (size_t) written < sizeof(expected) &&
           type->kind == XR_KIND_CLASS && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) && type->canonical_key &&
           strcmp(type->canonical_key, expected) == 0;
}

static inline bool xr_semantic_string_utf8_source_type_is_exact(const XrSemanticPlan *plan,
                                                                uint32_t type_index) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, type_index);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    uint8_t required = type && type->kind == XR_KIND_ARRAY
                           ? XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT
                           : XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW;
    uint8_t allowed = required | XR_SEM_TYPE_CONST;
    if (!type || !children || (type->kind != XR_KIND_ARRAY && type->kind != XR_KIND_SLICE) ||
        type->builtin_type != XR_TID_NULL || type->scalar_rep != XR_SCALAR_REP_NONE ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 || type->child_count != 1 ||
        type->child_begin >= child_count || (type->flags & required) != required ||
        (type->flags & ~allowed) != 0)
        return false;
    const XrSemanticTypeRecord *element = xr_semantic_plan_type(plan, children[type->child_begin]);
    return element && element->kind == XR_KIND_INT && element->builtin_type == XR_TID_NULL &&
           element->scalar_rep == XR_NATIVE_U8 && element->flags == 0 &&
           element->child_count == 0 && element->aggregate_extent == 0 &&
           element->aggregate_align == 0;
}

static inline const XrSemanticOperationRecord *
xr_semantic_string_utf8_unique_value_definition(const XrSemanticPlan *plan, uint32_t function,
                                                uint32_t value) {
    const XrSemanticOperationRecord *found = NULL;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->function != function || candidate->result_value != value)
            continue;
        if (found)
            return NULL;
        found = candidate;
    }
    return found;
}

/* Prove one runtime-owned UTF-8 decoder call from frozen SemanticPlan rows.
 * The returned method symbol and argument value are discriminants used by the
 * TargetPlan call identity; neither is recovered from selector spelling. */
static inline bool
xr_semantic_string_utf8_static_call_is_exact(const XrSemanticPlan *plan,
                                             const XrSemanticOperationRecord *operation,
                                             uint32_t *method_symbol, uint32_t *argument_value) {
    if (method_symbol)
        *method_symbol = XI_METHOD_SYMBOL_INVALID;
    if (argument_value)
        *argument_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t operand_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation || !operands || !metadata || operation->opcode != XI_CALL_METHOD ||
        operation->operand_count != 2 || operation->operand_begin + 1u >= operand_count ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1 || operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        !xr_semantic_tagged_string_type_is_exact(
            xr_semantic_plan_type(plan, operation->result_type)))
        return false;

    uint32_t symbol = (uint32_t) ((uint64_t) operation->semantic_immediate >> 1);
    const char *selector = metadata[operation->metadata_begin];
    if (operation->semantic_immediate <= 0 || (operation->semantic_immediate & 1) != 0 ||
        !((symbol == XI_METHOD_SYMBOL_FROM_UTF8 && strcmp(selector, "fromUtf8") == 0) ||
          (symbol == XI_METHOD_SYMBOL_FROM_UTF8_LOSSY && strcmp(selector, "fromUtf8Lossy") == 0)))
        return false;

    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *argument = receiver + 1;
    const XrSemanticOperationRecord *producer =
        xr_semantic_string_utf8_unique_value_definition(plan, operation->function, receiver->value);
    uint32_t producer_metadata_count = 0;
    const char *const *producer_metadata =
        xr_semantic_plan_metadata(plan, &producer_metadata_count);
    if (!xr_semantic_string_class_namespace_type_is_exact(
            xr_semantic_plan_type(plan, receiver->type)) ||
        receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW || !producer ||
        producer->opcode != XI_GET_BUILTIN || producer->result_value != receiver->value ||
        producer->result_type != receiver->type ||
        producer->semantic_immediate != XR_GLOBAL_VAR_STRING || producer->operand_count != 0 ||
        producer->metadata_count != 1 || producer->metadata_begin >= producer_metadata_count ||
        strcmp(producer_metadata[producer->metadata_begin], "String") != 0 ||
        argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != 0 ||
        argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        argument->ownership_action != XR_SEM_OPERAND_BORROW ||
        !xr_semantic_string_utf8_source_type_is_exact(plan, argument->type))
        return false;
    if (method_symbol)
        *method_symbol = symbol;
    if (argument_value)
        *argument_value = argument->value;
    return true;
}

#endif  // XR_SEMANTIC_STRING_UTF8_SHAPE_H
