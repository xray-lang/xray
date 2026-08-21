/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_owner_forward_shape.h - Exact ARC owner forward authority
 */

#ifndef XR_SEMANTIC_OWNER_FORWARD_SHAPE_H
#define XR_SEMANTIC_OWNER_FORWARD_SHAPE_H

#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "xr_semantic_plan.h"

/* An ARC owner transfer to a fresh SSA value: the result holds exactly what the
 * source held, and the source's binding is not invalidated.
 *
 * It sits beside the identity copy and differs in one field that matters: a
 * copy borrows, this one owns, because the point of the operation is to move
 * ownership rather than to name the same thing twice. Everything else is
 * enumerated the same way, so that difference is the only one this admits.
 *
 * Read by the target builder to give the result its source's representation.
 */
static inline bool xr_semantic_owner_forward_is_exact(const XrSemanticPlan *plan,
                                                      const XrSemanticOperationRecord *operation,
                                                      uint32_t *source_value_out) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!plan || !operation || !operands || operation->opcode != XI_OWNER_FORWARD ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->semantic_immediate != 0 || operation->allocation_key ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE || operation->metadata_count != 0 ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_OWNER_FORWARD) ||
        operation->flags != xi_generated_op_default_flags(XI_OWNER_FORWARD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_OWNER_FORWARD) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->return_complete != 1)
        return false;
    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    if (source->role != XR_SEM_OPERAND_VALUE || source->parameter != -1 || source->flags != 0)
        return false;
    if (source_value_out)
        *source_value_out = source->value;
    return true;
}

#endif  // XR_SEMANTIC_OWNER_FORWARD_SHAPE_H
