/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_panic_catch_shape.h - Exact caught-panic payload authority
 */

#ifndef XR_SEMANTIC_PANIC_CATCH_SHAPE_H
#define XR_SEMANTIC_PANIC_CATCH_SHAPE_H

#include <string.h>

#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "xr_semantic_plan.h"

/* The value a handler receives when it catches a panic.  Two opcodes reach the
 * same payload by different control flow: XI_CATCH is the handler entry of a
 * `catch` clause, and XI_ERR_CATCH is the one on the error-channel path.  Both
 * take no operand, name no callee, and hand back one owned reference whose type
 * the plan cannot narrow -- a handler binds whatever was raised.  Writing the
 * judgement once is what keeps the two from being answered differently: the
 * opcode-derived facts are read from the operation's own opcode rather than
 * from a spelling fixed at one of them. */
static inline bool xr_semantic_panic_catch_is_exact(const XrSemanticPlan *plan,
                                                    const XrSemanticOperationRecord *operation) {
    XrStableId zero = {{0}};
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (!plan || !operation ||
        (operation->opcode != XI_CATCH && operation->opcode != XI_ERR_CATCH) ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->function >= xr_semantic_plan_function_count(plan) ||
        operation->operand_count != 0 || operation->metadata_count != 0 ||
        operation->allocation_key != NULL || !xr_stable_id_equal(operation->allocation_id, zero) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->semantic_immediate != 0 || operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->effects != xi_generated_op_effects(operation->opcode) ||
        operation->flags != xi_generated_op_default_flags(operation->opcode) ||
        operation->ownership_use != xi_generated_op_own_use(operation->opcode) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE || operation->result_alias_operand != -1 ||
        operation->return_parameter != -1 || operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_complete != 1 || operation->view_source_value != XR_SEMANTIC_INDEX_NONE ||
        operation->view_element_type != XR_SEMANTIC_INDEX_NONE ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        operation->view_origin != XI_VIEW_ORIGIN_NONE || operation->view_capability != 0 ||
        operation->view_lifetime != 0 || operation->view_complete != 0)
        return false;
    for (uint32_t i = 0; i < 8; i++)
        if (operation->evidence[i] != (i == 7 ? XR_SEMANTIC_INDEX_NONE : 0u))
            return false;
    return type && type->kind == XR_KIND_UNKNOWN && type->builtin_type == XR_TID_NULL &&
           type->source_class == XR_SEMANTIC_INDEX_NONE && type->source_enum_key == NULL &&
           xr_stable_id_equal(type->source_enum_identity, zero) &&
           xr_stable_id_equal(type->source_class_identity, zero) && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 && type->enum_layout_id == 0 &&
           type->enum_member_count == 0 && type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           type->enum_flags == 0 && type->reserved_enum == 0;
}

/* Reading a field off a caught payload.  The handler's binding is the one value
 * this read may name: a payload's type is never narrowed, so the field is
 * reached through the runtime's own property lookup rather than a class layout,
 * and the read borrows the payload instead of taking it. */
static inline bool
xr_semantic_panic_catch_field_read_is_exact(const XrSemanticPlan *plan,
                                            const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!plan || !operation || !operands || operation->opcode != XI_LOAD_FIELD ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->metadata_count != 1 || operation->result_value == XR_SEMANTIC_INDEX_NONE)
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
    return definition && definition->function == operation->function &&
           xr_semantic_panic_catch_is_exact(plan, definition);
}

#endif /* XR_SEMANTIC_PANIC_CATCH_SHAPE_H */
