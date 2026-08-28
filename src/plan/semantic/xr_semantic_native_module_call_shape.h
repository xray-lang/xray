/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_native_module_call_shape.h - Exact native module scalar call shape
 */

#ifndef XR_SEMANTIC_NATIVE_MODULE_CALL_SHAPE_H
#define XR_SEMANTIC_NATIVE_MODULE_CALL_SHAPE_H

#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "xr_semantic_native_module_shape.h"

/* The frozen shape of a native stdlib module member call: a method call whose
 * receiver is a module namespace handle rather than a constructible value, with
 * every argument and the result crossing the boundary as one plain scalar.
 *
 * This judgement had four copies -- SemanticPlan verifier, TargetPlan builder
 * and verifier, AOT refinement -- and the copies disagreed. Two were byte
 * identical, one carried an extra function-window check, and the verifier spelt
 * the same terms inline against raw plan arrays. None of the four restated the
 * auxiliary kind, so a term the classifier requires had no witness in the frozen
 * record. A judgement written four times is a judgement that can drift four
 * ways, and the end-to-end suites cannot see the drift: they compare printed
 * output, which stays correct while a copy quietly admits or refuses more than
 * its siblings. One definition is what keeps the layers answering the same
 * question.
 *
 * The auxiliary kind is the term that motivated collecting them. The classifier
 * proves `aux_kind == XI_AUX_KIND_NONE` over the Xi value (the selector is a
 * plain spelling and not some other auxiliary payload), and the builder freezes
 * that fact into `auxiliary_kind`. A frozen row that never restates it would let
 * a build-time fact stand in as an admission term with no witness in the
 * artifact the consumers actually read. Restating it here costs nothing for a
 * plan this builder produced and closes the gap for one it did not.
 *
 * `semantic_immediate` is the frozen `XI_CALL_METHOD.aux_int`, which encodes
 * `(method_symbol << 1) | optional_chaining`. Requiring it positive and even
 * states that the selector resolved and that the callsite is not an optional
 * chain, whose short circuit this row does not describe. */
static inline bool xr_semantic_native_module_scalar_call_shape_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    const char **out_selector, uint32_t *out_receiver_value, uint32_t *out_arity) {
    uint32_t operand_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation || !operands || !metadata || operation->opcode != XI_CALL_METHOD ||
        operation->semantic_immediate <= 0 || (operation->semantic_immediate & 1) != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE || operation->operand_count == 0 ||
        operation->operand_begin >= operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        (operation->flags & XI_FLAG_MAY_SUSPEND) != 0 ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        operation->result_alias_operand != -1 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        !xr_semantic_native_module_boundary_type_is_exact(
            xr_semantic_plan_type(plan, operation->result_type), true))
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    if (receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW)
        return false;
    for (uint16_t i = 1; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *argument = receiver + i;
        if (argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != (int16_t) (i - 1) ||
            argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
            !xr_semantic_native_module_boundary_type_is_exact(
                xr_semantic_plan_type(plan, argument->type), false))
            return false;
    }
    if (out_selector)
        *out_selector = metadata[operation->metadata_begin];
    if (out_receiver_value)
        *out_receiver_value = receiver->value;
    if (out_arity)
        *out_arity = (uint32_t) (operation->operand_count - 1u);
    return true;
}

#endif /* XR_SEMANTIC_NATIVE_MODULE_CALL_SHAPE_H */
