/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_ownership_owner_shape.h - Which operations own, and which define an owner
 *
 * KEY CONCEPT:
 *   The obligation builder decides which operations get an owner record; the
 *   audit then checks the record it finds against the same question. Asked
 *   twice, the two answers are free to drift, and a drift here does not read
 *   as a disagreement -- it reads as a missing owner (XR_OWN_3002), which
 *   points at the plan rather than at the pair of predicates. So the question
 *   is answered once, here, and both sides call it.
 */

#ifndef XR_OWNERSHIP_OWNER_SHAPE_H
#define XR_OWNERSHIP_OWNER_SHAPE_H

#include "../semantic/xr_semantic_plan.h"
#include "../semantic/xr_semantic_plan_internal.h"
#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"

static inline bool xr_ownership_type_is_root(const XrSemanticPlan *plan, uint32_t type_index) {
    return plan && type_index < plan->type_count &&
           (plan->types[type_index].flags & XR_SEM_TYPE_OWNERSHIP_ROOT) != 0;
}

/* True when the operation copies a `null` constant into a nullable reference.
 *
 * Its result type is an ownership root, but the value it holds is null: it
 * owns nothing, releases nothing, and has nothing for an owner to be defined
 * over. Tracking it produced a class with no defining operation at all, which
 * surfaced as XR_OWN_3002 -- and the two repairs that treat the copy itself as
 * the definition only moved the failure to XR_OWN_3003, because an owner whose
 * balance never rises reads as already released. Ownership is about values
 * that hold something; this one provably does not. */
static inline bool xr_ownership_result_is_null_alias(const XrSemanticPlan *plan,
                                                     const XrSemanticOperationRecord *operation) {
    if (!plan || !operation || operation->result_alias_operand < 0 ||
        (uint16_t) operation->result_alias_operand >= operation->operand_count)
        return false;
    const XrSemanticOperandRecord *alias =
        &plan->operands[operation->operand_begin + (uint16_t) operation->result_alias_operand];
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, alias->type);
    return type && type->kind == XR_KIND_NULL;
}

/* The operation produces a value ownership tracks. */
static inline bool xr_ownership_operation_has_owner(const XrSemanticPlan *plan,
                                                    const XrSemanticOperationRecord *operation) {
    return operation && xr_ownership_type_is_root(plan, operation->result_type) &&
           xi_generated_op_result_kind(operation->opcode) != XI_GEN_RESULT_VOID &&
           operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_NONE &&
           !xr_ownership_result_is_null_alias(plan, operation);
}

/* The operation is where its equivalence class's ownership begins, rather than
 * a later reader of an owner someone else established. */
static inline bool
xr_ownership_operation_defines_owner(const XrSemanticPlan *plan,
                                     const XrSemanticOperationRecord *operation) {
    if (!xr_ownership_operation_has_owner(plan, operation))
        return false;
    if (operation->opcode == XI_PARAM || operation->opcode == XI_CONST ||
        operation->opcode == XI_STACK_ALLOC || operation->opcode == XI_PHI)
        return true;
    if (operation->result_alias_operand >= 0 || operation->ownership_use == XI_GEN_OWN_USE_PASS ||
        operation->opcode == XI_RETAIN || operation->opcode == XI_RELEASE)
        return false;
    return operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED ||
           operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED ||
           operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT;
}

/* The operand an aliasing operation shares an ownership class with, or
 * XR_SEMANTIC_INDEX_NONE when it shares one with nobody.
 *
 * Three passes build this equivalence -- the obligation builder, the audit and
 * the replay -- and each used to spell the rule out for itself. They must
 * agree: a class the obligation builder splits but the replay merges makes the
 * replay read a copy as a use of the class it just defined, which surfaces as
 * XR_OWN_3003 rather than as the disagreement it is.
 *
 */
static inline uint32_t
xr_ownership_alias_class_operand(const XrSemanticPlan *plan,
                                 const XrSemanticOperationRecord *operation) {
    if (!plan || !operation || operation->result_alias_operand < 0 ||
        (uint16_t) operation->result_alias_operand >= operation->operand_count)
        return XR_SEMANTIC_INDEX_NONE;
    return plan->operands[operation->operand_begin + (uint16_t) operation->result_alias_operand]
        .value;
}

#endif /* XR_OWNERSHIP_OWNER_SHAPE_H */
