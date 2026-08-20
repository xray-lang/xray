/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_identity_copy_shape.h - Exact identity copy authority
 */

#ifndef XR_SEMANTIC_IDENTITY_COPY_SHAPE_H
#define XR_SEMANTIC_IDENTITY_COPY_SHAPE_H

#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "../../shared/xr_copy_core.h"
#include "xr_semantic_plan.h"

/* A copy that only renames its operand: the result holds exactly what the
 * source holds. Unlike the container copy next door, nothing is materialised
 * and no ownership root is created -- the result borrows, which is why the
 * operation is required to say so.
 *
 * Every field the operation carries is enumerated, in the same style as the
 * two specialised identity-copy recipes in the target builder, because
 * anything left unnamed is a difference that could change what the copy means.
 *
 * Deliberately not enumerated: the semantic type. Those two recipes pin theirs
 * because each serves exactly one type; this one serves whatever the source
 * already proved, and the source's own family is what decided that.
 *
 * Read by the target builder, to give the result the source's representation,
 * and by the target verifier, to accept that binding. One statement of the
 * shape, so the two layers cannot drift apart.
 */
static inline bool xr_semantic_identity_copy_is_exact(const XrSemanticPlan *plan,
                                                      const XrSemanticOperationRecord *operation,
                                                      uint32_t *source_value_out) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!plan || !operation || !operands || operation->opcode != XI_COPY ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->semantic_immediate != XI_COPY_KIND_IDENTITY || operation->allocation_key ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE || operation->auxiliary_kind != 0 ||
        operation->metadata_count != 0 || operation->effects != xi_generated_op_effects(XI_COPY) ||
        operation->flags != xi_generated_op_default_flags(XI_COPY) ||
        operation->ownership_use != xi_generated_op_own_use(XI_COPY) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_alias_operand != 0)
        return false;
    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    if (source->role != XR_SEM_OPERAND_VALUE || source->parameter != -1 ||
        source->type != operation->result_type || source->flags != 0)
        return false;
    if (source_value_out)
        *source_value_out = source->value;
    return true;
}

#endif  // XR_SEMANTIC_IDENTITY_COPY_SHAPE_H
