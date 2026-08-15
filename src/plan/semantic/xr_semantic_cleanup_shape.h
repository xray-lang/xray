/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_cleanup_shape.h - Exact semantic ownership cleanup shapes
 */

#ifndef XR_SEMANTIC_CLEANUP_SHAPE_H
#define XR_SEMANTIC_CLEANUP_SHAPE_H

#include "xr_semantic_string_shape.h"
#include "../ownership/xr_ownership_certificate.h"
#include "../../ir/xi_ops_gen.h"

typedef struct XrSemanticStringConcatReleaseShape {
    uint32_t function;
    uint32_t operation;
    uint32_t released_value;
    uint32_t producer_operation;
    uint32_t owner;
} XrSemanticStringConcatReleaseShape;

/* This is deliberately a narrow first cleanup family.  An ordinary explicit
 * RELEASE is admitted only when the frozen ownership certificate binds it to
 * the fresh owner created by one exact String concatenation. */
static inline bool xr_semantic_string_concat_release_is_exact(
    const XrSemanticPlan *plan, uint32_t operation_index,
    XrSemanticStringConcatReleaseShape *out) {
    if (out)
        *out = (XrSemanticStringConcatReleaseShape) {0};
    const XrSemanticOperationRecord *release =
        xr_semantic_plan_operation(plan, operation_index);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan, &operand_count);
    if (!plan || !release || !operands || release->opcode != XI_RELEASE ||
        release->operand_count != 1 || release->operand_begin >= operand_count ||
        release->metadata_count != 0 || release->semantic_immediate != 0 ||
        release->auxiliary_kind != 0 ||
        release->constant != XR_SEMANTIC_INDEX_NONE ||
        release->callable_function != XR_SEMANTIC_INDEX_NONE ||
        release->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        release->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        release->effects != xi_generated_op_effects(XI_RELEASE) ||
        release->flags != xi_generated_op_default_flags(XI_RELEASE) ||
        release->ownership_use != xi_generated_op_own_use(XI_RELEASE) ||
        release->result_alias_operand != -1 || release->transfer_mode != 0 ||
        release->parameter_mode != 0 || release->parameter_ownership != 0 ||
        release->return_provenance != XR_SEM_RETURN_NONE ||
        release->return_parameter != -1 || release->return_complete != 0 ||
        release->view_complete != 0 || release->view_source_operand != -1 ||
        release->view_source_parameter != -1)
        return false;

    const XrSemanticOperandRecord *operand = &operands[release->operand_begin];
    if (operand->role != XR_SEM_OPERAND_VALUE || operand->parameter != -1 ||
        operand->transfer_mode != XR_TRANSFER_SHARE ||
        operand->ownership_action != XR_SEM_OPERAND_CONSUME ||
        operand->parameter_mode != XR_PARAM_READ ||
        operand->access != XR_CALL_ARG_PLAIN ||
        operand->origin != XI_PLACE_ORIGIN_NONE ||
        operand->lifetime != XI_PLACE_LIFETIME_NONE ||
        operand->escape != XI_PLACE_ESCAPE_NONE || operand->flags != 0)
        return false;

    uint32_t producer_index = XR_SEMANTIC_INDEX_NONE;
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(plan);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate =
            xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->function != release->function ||
            candidate->result_value != operand->value)
            continue;
        if (producer_index != XR_SEMANTIC_INDEX_NONE)
            return false;
        producer_index = i;
    }
    const XrSemanticOperationRecord *producer =
        xr_semantic_plan_operation(plan, producer_index);
    if (!xr_semantic_string_concat_is_exact(plan, producer))
        return false;

    const XrOwnershipCertificate *certificate =
        xr_semantic_plan_ownership(plan);
    uint32_t matched_owner = XR_SEMANTIC_INDEX_NONE;
    size_t event_count = xr_ownership_certificate_event_count(certificate);
    for (uint32_t i = 0; i < event_count; i++) {
        const XrOwnershipEventRecord *event =
            xr_ownership_certificate_event(certificate, i);
        if (!event || event->operation != operation_index)
            continue;
        if (matched_owner != XR_SEMANTIC_INDEX_NONE ||
            event->kind != XR_OWN_EVENT_RELEASE || event->logical_delta != -1 ||
            event->state_after != XR_OWN_RELEASED ||
            event->program_point != XR_OWN_POINT_AFTER_OPERATION ||
            event->block != release->block ||
            event->successor != XR_SEMANTIC_INDEX_NONE || event->reserved != 0)
            return false;
        matched_owner = event->owner;
    }
    const XrOwnershipOwnerRecord *owner =
        xr_ownership_certificate_owner(certificate, matched_owner);
    if (!owner || owner->function != release->function ||
        owner->origin_value != operand->value ||
        owner->initial_state != XR_OWN_OWNED_LOCAL ||
        owner->exit_state != XR_OWN_RELEASED || owner->flags != 0)
        return false;

    if (out) {
        *out = (XrSemanticStringConcatReleaseShape) {
            .function = release->function,
            .operation = operation_index,
            .released_value = operand->value,
            .producer_operation = producer_index,
            .owner = matched_owner,
        };
    }
    return true;
}

#endif  // XR_SEMANTIC_CLEANUP_SHAPE_H
