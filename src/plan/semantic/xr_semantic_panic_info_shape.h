/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_panic_info_shape.h - Exact PanicInfo constructor authority
 */

#ifndef XR_SEMANTIC_PANIC_INFO_SHAPE_H
#define XR_SEMANTIC_PANIC_INFO_SHAPE_H

#include <stdio.h>
#include <string.h>

#include "../../base/xglobal_indices.h"
#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "xr_semantic_plan.h"

/* `PanicInfo` is a compiler-owned class namespace with no source declaration:
 * every callsite is synthesised by lowering, never written by a program, so the
 * class token is the reserved XI_GET_BUILTIN global and the type record can
 * never carry a source class.  The namespace plus the frozen `constructor`
 * selector names exactly one implementation with no open dispatch domain.
 *
 * The constructor's receiver and result share this one type record: the class
 * token is spelled with the class type itself rather than an instance type,
 * because no instance type exists for a class that source can never name. */
static inline bool xr_semantic_panic_info_class_type_is_exact(const XrSemanticTypeRecord *type) {
    char expected[160];
    int written = snprintf(
        expected, sizeof(expected), "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;named:9:PanicInfo[0]",
        (unsigned) XR_KIND_CLASS, (unsigned) XR_TID_NULL, (unsigned) XR_SCALAR_REP_NONE);
    XrStableId zero = {{0}};
    return type && written > 0 && (size_t) written < sizeof(expected) &&
           type->kind == XR_KIND_CLASS && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) && type->canonical_key &&
           strcmp(type->canonical_key, expected) == 0;
}

/* The class token operation itself: a reserved global read that yields the
 * namespace type above.  Only the reserved index can present this record, so
 * matching it proves the receiver is the compiler's own PanicInfo token. */
static inline bool
xr_semantic_panic_info_global_is_exact(const XrSemanticPlan *plan,
                                       const XrSemanticOperationRecord *operation) {
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    return plan && operation && metadata && operation->opcode == XI_GET_BUILTIN &&
           operation->operand_count == 0 && operation->metadata_count == 1 &&
           operation->metadata_begin < metadata_count &&
           strcmp(metadata[operation->metadata_begin], "PanicInfo") == 0 &&
           operation->auxiliary_kind == XI_AUX_KIND_NONE &&
           operation->semantic_immediate == XR_GLOBAL_VAR_PANIC_INFO &&
           operation->constant == XR_SEMANTIC_INDEX_NONE &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
           operation->effects == xi_generated_op_effects(XI_GET_BUILTIN) &&
           operation->flags == xi_generated_op_default_flags(XI_GET_BUILTIN) &&
           operation->result_alias_operand == -1 &&
           xr_semantic_panic_info_class_type_is_exact(type);
}

/* The constructor callsite: class token receiver, one string message argument,
 * and an owned result of the namespace type.  The message argument is consumed
 * into the panic record, so the operand carries the call contract flag and the
 * result owns what it allocates. */
static inline bool
xr_semantic_panic_info_constructor_is_exact(const XrSemanticPlan *plan,
                                            const XrSemanticOperationRecord *operation,
                                            uint32_t *argument_value) {
    uint32_t operand_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation || !operands || !metadata ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_PANIC_INFO_CONSTRUCTOR ||
        operation->opcode != XI_CALL_METHOD || operation->semantic_immediate <= 0 ||
        (operation->semantic_immediate & 1) != 0 || operation->operand_count != 2 ||
        operation->operand_begin + 1u >= operand_count || operation->metadata_count != 1 ||
        operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "constructor") != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->result_alias_operand != -1 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *argument = receiver + 1;
    const XrSemanticTypeRecord *receiver_type = xr_semantic_plan_type(plan, receiver->type);
    const XrSemanticTypeRecord *argument_type = xr_semantic_plan_type(plan, argument->type);
    const XrSemanticTypeRecord *result_type = xr_semantic_plan_type(plan, operation->result_type);
    if (!xr_semantic_panic_info_class_type_is_exact(receiver_type) ||
        !xr_semantic_panic_info_class_type_is_exact(result_type) || !argument_type ||
        argument_type->kind != XR_KIND_STRING || argument_type->builtin_type != XR_TID_NULL ||
        argument_type->child_count != 0 || argument_type->scalar_rep != XR_SCALAR_REP_NONE ||
        receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != 0 ||
        argument->flags != XR_SEM_OPERAND_CALL_CONTRACT)
        return false;
    if (argument_value)
        *argument_value = argument->value;
    return true;
}

/* The refinement layer additionally proves the receiver came from the class
 * token in this same plan rather than from any other operation that happens to
 * present the namespace type, because a representation answer is bound to the
 * value that produced it. */
static inline bool xr_semantic_panic_info_constructor_with_receiver_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *argument_value) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!xr_semantic_panic_info_constructor_is_exact(plan, operation, argument_value))
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperationRecord *definition = NULL;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->result_value != receiver->value)
            continue;
        if (definition)
            return false;
        definition = candidate;
    }
    return definition && xr_semantic_panic_info_global_is_exact(plan, definition);
}

#endif /* XR_SEMANTIC_PANIC_INFO_SHAPE_H */
