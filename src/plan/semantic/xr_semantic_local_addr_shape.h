/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_local_addr_shape.h - Taking the address of a local
 */

#ifndef XR_SEMANTIC_LOCAL_ADDR_SHAPE_H
#define XR_SEMANTIC_LOCAL_ADDR_SHAPE_H

#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "xr_semantic_plan.h"

/* The address of a local, which a cleanup block takes so it can read the
 * binding at the time the block runs rather than at the time it was written.
 *
 * The one thing to know about this operation is that its result type is the
 * type of what it points at, not of the address: source has no way to write
 * "pointer to int" here, so the plan records the subject's type on both sides.
 * Every judgement about the result therefore has to come from the operation --
 * asking the type gives the answer for the wrong value, which is exactly how
 * the address came to be bound as an integer.
 *
 * The borrow is total: the address does not own, escape, outlive, or alias
 * anything, and the operand states each of those rather than leaving it open.
 *
 * Lowering marks a cleanup block's capture with its own bit, and this shape is
 * that bit exactly -- set, and nothing else set with it.  The opcode is shared
 * by several operations that are not this one: a raw dereference, a direct
 * projection, and the plain address a `ref` argument takes, which carries no
 * bit at all and whose storage the families that know about ref parameters
 * already answer for.  Admitting "the cleanup bit or nothing" took that last
 * one away from them and broke a case that built before. */
static inline bool xr_semantic_local_addr_is_exact(const XrSemanticPlan *plan,
                                                   const XrSemanticOperationRecord *operation,
                                                   const XrSemanticOperandRecord **out_source) {
    if (out_source)
        *out_source = NULL;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    XrStableId zero = {{0}};
    if (!plan || !operation || !operands || operation->opcode != XI_LOCAL_ADDR ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->function >= xr_semantic_plan_function_count(plan) ||
        operation->metadata_count != 0 ||
        operation->semantic_immediate != (int64_t) XI_LOCAL_ADDR_AUX_CLEANUP_LIVE ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE || operation->allocation_key != NULL ||
        !xr_stable_id_equal(operation->allocation_id, zero) ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1)
        return false;
    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    if (source->type != operation->result_type || source->role != XR_SEM_OPERAND_VALUE ||
        source->parameter != -1 || source->transfer_mode != XR_TRANSFER_SHARE ||
        source->ownership_action != XR_SEM_OPERAND_BORROW ||
        source->parameter_mode != XR_PARAM_READ || source->access != XR_CALL_ARG_PLAIN ||
        source->origin != XI_PLACE_ORIGIN_NONE || source->lifetime != XI_PLACE_LIFETIME_NONE ||
        source->escape != XI_PLACE_ESCAPE_NONE || source->flags != 0)
        return false;
    if (out_source)
        *out_source = source;
    return true;
}

#endif /* XR_SEMANTIC_LOCAL_ADDR_SHAPE_H */
