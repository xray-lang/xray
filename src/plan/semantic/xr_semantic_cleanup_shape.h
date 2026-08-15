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
#include "../../base/xmalloc.h"
#include "../../ir/xi_ops_gen.h"

typedef struct XrSemanticStringConcatReleaseShape {
    uint32_t function;
    uint32_t operation;
    uint32_t released_value;
    uint32_t producer_operation;
    uint32_t owner;
} XrSemanticStringConcatReleaseShape;

typedef enum XrSemanticStringConcatReleaseIndexStatus {
    XR_SEMANTIC_RELEASE_INDEX_OK = 0,
    XR_SEMANTIC_RELEASE_INDEX_INVALID,
    XR_SEMANTIC_RELEASE_INDEX_BUDGET_EXHAUSTED,
    XR_SEMANTIC_RELEASE_INDEX_ALLOCATION_FAILED,
} XrSemanticStringConcatReleaseIndexStatus;

#define XR_SEMANTIC_LIFECYCLE_MAX_WORK UINT64_C(100000000)

typedef struct XrSemanticStringConcatReleaseIndex {
    XrSemanticStringConcatReleaseShape *rows;
    uint32_t count;
    uint32_t capacity;
    uint64_t linear_work;
} XrSemanticStringConcatReleaseIndex;

static inline bool xr_semantic_string_concat_release_resolved_is_exact(
    const XrSemanticPlan *plan, uint32_t operation_index,
    uint32_t producer_index, uint32_t matched_owner, bool producer_is_exact,
    XrSemanticStringConcatReleaseShape *out) {
    if (out)
        *out = (XrSemanticStringConcatReleaseShape) {0};
    const XrSemanticOperationRecord *release =
        xr_semantic_plan_operation(plan, operation_index);
    const XrSemanticOperationRecord *producer =
        xr_semantic_plan_operation(plan, producer_index);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan, &operand_count);
    if (!plan || !release || !producer || !operands || !producer_is_exact ||
        release->opcode != XI_RELEASE || release->operand_count != 1 ||
        release->operand_begin >= operand_count || release->metadata_count != 0 ||
        release->semantic_immediate != 0 || release->auxiliary_kind != 0 ||
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
        operand->escape != XI_PLACE_ESCAPE_NONE || operand->flags != 0 ||
        producer->function != release->function ||
        producer->result_value != operand->value)
        return false;

    const XrOwnershipCertificate *certificate =
        xr_semantic_plan_ownership(plan);
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
    return xr_semantic_string_concat_release_resolved_is_exact(
        plan, operation_index, producer_index, matched_owner,
        xr_semantic_string_concat_is_exact(
            plan, xr_semantic_plan_operation(plan, producer_index)), out);
}

static inline void xr_semantic_string_concat_release_index_dispose(
    XrSemanticStringConcatReleaseIndex *index) {
    if (!index)
        return;
    xr_free(index->rows);
    *index = (XrSemanticStringConcatReleaseIndex) {0};
}

static inline bool xr_semantic_lifecycle_work_charge(uint64_t *work,
                                                     uint64_t amount) {
    if (!work || amount > XR_SEMANTIC_LIFECYCLE_MAX_WORK - *work)
        return false;
    *work += amount;
    return true;
}

static inline bool xr_semantic_lifecycle_work_charge_product(
    uint64_t *work, uint64_t count, uint64_t cost) {
    if (count != 0 && cost > XR_SEMANTIC_LIFECYCLE_MAX_WORK / count)
        return false;
    return xr_semantic_lifecycle_work_charge(work, count * cost);
}

static inline uint32_t xr_semantic_lifecycle_sort_height(uint32_t count) {
    uint32_t height = 0;
    for (uint32_t span = count; span > 1u; span = (span + 1u) / 2u)
        height++;
    return height;
}

/* Build exact release authority once.  Dense operation/value/event indexes
 * keep release discovery linear in the frozen plan instead of rescanning all
 * operations and ownership events for every RELEASE candidate. */
static inline XrSemanticStringConcatReleaseIndexStatus
xr_semantic_string_concat_release_index_build(
    const XrSemanticPlan *plan, XrSemanticStringConcatReleaseIndex *out) {
    if (!out)
        return XR_SEMANTIC_RELEASE_INDEX_INVALID;
    *out = (XrSemanticStringConcatReleaseIndex) {0};
    if (!plan)
        return XR_SEMANTIC_RELEASE_INDEX_INVALID;
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(plan);
    uint32_t function_count =
        (uint32_t) xr_semantic_plan_function_count(plan);
    uint32_t value_extent = 0;
    for (uint32_t function = 0; function < function_count; function++) {
        const XrSemanticFunctionRecord *record =
            xr_semantic_plan_function(plan, function);
        if (!record || record->value_begin > UINT32_MAX - record->value_count)
            return XR_SEMANTIC_RELEASE_INDEX_INVALID;
        uint32_t end = record->value_begin + record->value_count;
        if (end > value_extent)
            value_extent = end;
    }
    const XrOwnershipCertificate *certificate =
        xr_semantic_plan_ownership(plan);
    size_t event_count = xr_ownership_certificate_event_count(certificate);
    uint64_t required_work = 0;
    if (!xr_semantic_lifecycle_work_charge(
            &required_work, (uint64_t) function_count * 2u) ||
        !xr_semantic_lifecycle_work_charge(&required_work, value_extent) ||
        !xr_semantic_lifecycle_work_charge(&required_work, event_count) ||
        !xr_semantic_lifecycle_work_charge(
            &required_work, (uint64_t) operation_count * 8u))
        return XR_SEMANTIC_RELEASE_INDEX_BUDGET_EXHAUSTED;
    for (uint32_t operation = 0; operation < operation_count; operation++) {
        const XrSemanticOperationRecord *record =
            xr_semantic_plan_operation(plan, operation);
        if (!record)
            return XR_SEMANTIC_RELEASE_INDEX_INVALID;
        if (!xr_semantic_lifecycle_work_charge(&required_work,
                                               record->operand_count))
            return XR_SEMANTIC_RELEASE_INDEX_BUDGET_EXHAUSTED;
    }
    out->linear_work = required_work;
    uint32_t *producer_by_value = value_extent
        ? (uint32_t *) xr_malloc((size_t) value_extent *
                                 sizeof(*producer_by_value))
        : NULL;
    uint32_t *event_index_by_operation = operation_count
        ? (uint32_t *) xr_malloc((size_t) operation_count *
                                 sizeof(*event_index_by_operation))
        : NULL;
    uint8_t *event_count_by_operation = operation_count
        ? (uint8_t *) xr_calloc(operation_count,
                                sizeof(*event_count_by_operation))
        : NULL;
    uint8_t *concat_exact = operation_count
        ? (uint8_t *) xr_calloc(operation_count, sizeof(*concat_exact))
        : NULL;
    if ((value_extent && !producer_by_value) ||
        (operation_count && (!event_index_by_operation ||
                             !event_count_by_operation || !concat_exact))) {
        xr_free(producer_by_value);
        xr_free(event_index_by_operation);
        xr_free(event_count_by_operation);
        xr_free(concat_exact);
        return XR_SEMANTIC_RELEASE_INDEX_ALLOCATION_FAILED;
    }
    for (uint32_t value = 0; value < value_extent; value++)
        producer_by_value[value] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t operation = 0; operation < operation_count; operation++)
        event_index_by_operation[operation] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t operation = 0; operation < operation_count; operation++) {
        const XrSemanticOperationRecord *record =
            xr_semantic_plan_operation(plan, operation);
        if (!record) {
            xr_free(producer_by_value);
            xr_free(event_index_by_operation);
            xr_free(event_count_by_operation);
            xr_free(concat_exact);
            return XR_SEMANTIC_RELEASE_INDEX_INVALID;
        }
        concat_exact[operation] =
            xr_semantic_string_concat_is_exact(plan, record);
        if (record->result_value == XR_SEMANTIC_INDEX_NONE)
            continue;
        if (record->result_value >= value_extent ||
            producer_by_value[record->result_value] != XR_SEMANTIC_INDEX_NONE) {
            xr_free(producer_by_value);
            xr_free(event_index_by_operation);
            xr_free(event_count_by_operation);
            xr_free(concat_exact);
            return XR_SEMANTIC_RELEASE_INDEX_INVALID;
        }
        producer_by_value[record->result_value] = operation;
    }
    for (size_t event_index = 0; event_index < event_count; event_index++) {
        const XrOwnershipEventRecord *event =
            xr_ownership_certificate_event(certificate, event_index);
        if (!event || event->operation == XR_SEMANTIC_INDEX_NONE)
            continue;
        if (event->operation >= operation_count) {
            xr_free(producer_by_value);
            xr_free(event_index_by_operation);
            xr_free(event_count_by_operation);
            xr_free(concat_exact);
            return XR_SEMANTIC_RELEASE_INDEX_INVALID;
        }
        uint8_t *count = &event_count_by_operation[event->operation];
        if (*count != UINT8_MAX)
            (*count)++;
        if (*count == 1 && event_index <= UINT32_MAX)
            event_index_by_operation[event->operation] =
                (uint32_t) event_index;
    }
    for (uint32_t operation = 0; operation < operation_count; operation++) {
        const XrSemanticOperationRecord *release =
            xr_semantic_plan_operation(plan, operation);
        if (!release || release->opcode != XI_RELEASE ||
            release->operand_count != 1 ||
            event_count_by_operation[operation] != 1)
            continue;
        uint32_t operand_count = 0;
        const XrSemanticOperandRecord *operands =
            xr_semantic_plan_operands(plan, &operand_count);
        if (!operands || release->operand_begin >= operand_count)
            continue;
        uint32_t value = operands[release->operand_begin].value;
        uint32_t producer = value < value_extent
                                ? producer_by_value[value]
                                : XR_SEMANTIC_INDEX_NONE;
        const XrOwnershipEventRecord *matched_event =
            event_index_by_operation[operation] != XR_SEMANTIC_INDEX_NONE
                ? xr_ownership_certificate_event(
                      certificate, event_index_by_operation[operation])
                : NULL;
        if (!matched_event || producer == XR_SEMANTIC_INDEX_NONE ||
            matched_event->kind != XR_OWN_EVENT_RELEASE ||
            matched_event->logical_delta != -1 ||
            matched_event->state_after != XR_OWN_RELEASED ||
            matched_event->program_point != XR_OWN_POINT_AFTER_OPERATION ||
            matched_event->block != release->block ||
            matched_event->successor != XR_SEMANTIC_INDEX_NONE ||
            matched_event->reserved != 0)
            continue;
        XrSemanticStringConcatReleaseShape shape = {0};
        if (!xr_semantic_string_concat_release_resolved_is_exact(
                plan, operation, producer,
                matched_event->owner, concat_exact[producer],
                &shape))
            continue;
        if (out->count == out->capacity) {
            uint32_t next_capacity = out->capacity ? out->capacity * 2u : 8u;
            if (next_capacity < out->capacity || next_capacity > operation_count)
                next_capacity = operation_count;
            if (next_capacity <= out->capacity) {
                xr_semantic_string_concat_release_index_dispose(out);
                xr_free(producer_by_value);
                xr_free(event_index_by_operation);
                xr_free(event_count_by_operation);
                xr_free(concat_exact);
                return XR_SEMANTIC_RELEASE_INDEX_ALLOCATION_FAILED;
            }
            void *next = xr_realloc(
                out->rows, (size_t) next_capacity * sizeof(*out->rows));
            if (!next) {
                xr_semantic_string_concat_release_index_dispose(out);
                xr_free(producer_by_value);
                xr_free(event_index_by_operation);
                xr_free(event_count_by_operation);
                xr_free(concat_exact);
                return XR_SEMANTIC_RELEASE_INDEX_ALLOCATION_FAILED;
            }
            out->rows = (XrSemanticStringConcatReleaseShape *) next;
            out->capacity = next_capacity;
        }
        out->rows[out->count++] = shape;
    }
    xr_free(producer_by_value);
    xr_free(event_index_by_operation);
    xr_free(event_count_by_operation);
    xr_free(concat_exact);
    return XR_SEMANTIC_RELEASE_INDEX_OK;
}

#endif  // XR_SEMANTIC_CLEANUP_SHAPE_H
