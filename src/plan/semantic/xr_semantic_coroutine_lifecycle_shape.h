/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_coroutine_lifecycle_shape.h - Exact first coroutine owner family
 */

#ifndef XR_SEMANTIC_COROUTINE_LIFECYCLE_SHAPE_H
#define XR_SEMANTIC_COROUTINE_LIFECYCLE_SHAPE_H

#include "xr_semantic_cleanup_shape.h"
#include "xr_semantic_graph.h"

typedef struct XrSemanticCoroutineLifecycleShape {
    uint32_t function;
    uint32_t state_entity;
    uint32_t state_operation;
    uint32_t logical_state;
    uint32_t producer_operation;
    uint32_t producer_value;
    uint32_t release_operation;
    uint32_t owner;
} XrSemanticCoroutineLifecycleShape;

#define XR_SEMANTIC_COROUTINE_LIFECYCLE_MAX_WORK \
    XR_SEMANTIC_LIFECYCLE_MAX_WORK

static inline bool xr_semantic_coroutine_lifecycle_work_budget_is_valid(
    uint32_t state_count, uint32_t release_count) {
    return state_count == 0 ||
           (uint64_t) release_count <=
               XR_SEMANTIC_COROUTINE_LIFECYCLE_MAX_WORK / state_count;
}

static inline bool xr_semantic_operation_precedes(
    const XrSemanticPlan *plan, const XrSemanticGraph *graph,
    uint32_t before_index, uint32_t after_index) {
    const XrSemanticOperationRecord *before =
        xr_semantic_plan_operation(plan, before_index);
    const XrSemanticOperationRecord *after =
        xr_semantic_plan_operation(plan, after_index);
    if (!before || !after || before->function != after->function)
        return false;
    if (before->block == after->block)
        return before_index < after_index;
    return xr_semantic_graph_dominates(graph, before->block, after->block);
}

static inline bool xr_semantic_operation_follows(
    const XrSemanticPlan *plan, const XrSemanticGraph *graph,
    uint32_t after_index, uint32_t before_index) {
    const XrSemanticOperationRecord *after =
        xr_semantic_plan_operation(plan, after_index);
    const XrSemanticOperationRecord *before =
        xr_semantic_plan_operation(plan, before_index);
    if (!after || !before || after->function != before->function)
        return false;
    if (after->block == before->block)
        return after_index > before_index;
    return xr_semantic_graph_postdominates(graph, after->block, before->block);
}

/* This intentionally recognizes only the first lifecycle family: one fresh,
 * owned dynamic String produced by an exact concatenation, live across one
 * frozen coroutine state, and consumed by its unique exact RELEASE after the
 * continuation.  Dominance and post-dominance come solely from frozen XSM CFG
 * rows; operation indexes are used only for ordering inside one frozen block. */
static inline bool xr_semantic_owned_string_coroutine_lifecycle_from_release_is_exact(
    const XrSemanticPlan *plan, const XrSemanticGraph *graph,
    uint32_t state_entity_index,
    const XrSemanticStringConcatReleaseShape *release_shape,
    XrSemanticCoroutineLifecycleShape *out) {
    if (out)
        *out = (XrSemanticCoroutineLifecycleShape) {0};
    uint32_t entity_count =
        plan ? (uint32_t) xr_semantic_plan_entity_count(plan) : 0;
    const XrSemanticEntityRecord *state =
        state_entity_index < entity_count
            ? xr_semantic_plan_entity(plan, state_entity_index)
            : NULL;
    const XrSemanticOperationRecord *producer = xr_semantic_plan_operation(
        plan, release_shape ? release_shape->producer_operation
                            : XR_SEMANTIC_INDEX_NONE);
    if (!plan || !graph || !state || !release_shape || !producer ||
        state->kind != XR_SEM_ENTITY_COROUTINE_STATE ||
        state->subject_kind != XR_SEM_ENTITY_SUBJECT_OPERATION ||
        state->subject >= xr_semantic_plan_operation_count(plan) ||
        state->ordinal == 0 ||
        !xr_semantic_string_concat_is_exact(plan, producer) ||
        producer->result_value == XR_SEMANTIC_INDEX_NONE ||
        release_shape->released_value != producer->result_value ||
        release_shape->function != producer->function ||
        producer->function !=
            xr_semantic_plan_operation(plan, state->subject)->function ||
        !xr_semantic_operation_precedes(plan, graph,
                                        release_shape->producer_operation,
                                        state->subject) ||
        !xr_semantic_operation_follows(plan, graph, release_shape->operation,
                                       state->subject))
        return false;
    if (out) {
        *out = (XrSemanticCoroutineLifecycleShape) {
            .function = producer->function,
            .state_entity = state_entity_index,
            .state_operation = state->subject,
            .logical_state = state->ordinal,
            .producer_operation = release_shape->producer_operation,
            .producer_value = producer->result_value,
            .release_operation = release_shape->operation,
            .owner = release_shape->owner,
        };
    }
    return true;
}

static inline bool xr_semantic_owned_string_coroutine_lifecycle_for_release_is_exact(
    const XrSemanticPlan *plan, const XrSemanticGraph *graph,
    uint32_t state_entity_index, uint32_t release_operation,
    XrSemanticCoroutineLifecycleShape *out) {
    XrSemanticStringConcatReleaseShape release_shape = {0};
    return xr_semantic_string_concat_release_is_exact(
               plan, release_operation, &release_shape) &&
           xr_semantic_owned_string_coroutine_lifecycle_from_release_is_exact(
               plan, graph, state_entity_index, &release_shape, out);
}

static inline bool xr_semantic_owned_string_coroutine_lifecycle_is_exact(
    const XrSemanticPlan *plan, const XrSemanticGraph *graph,
    uint32_t state_entity_index, uint32_t producer_operation,
    XrSemanticCoroutineLifecycleShape *out) {
    if (out)
        *out = (XrSemanticCoroutineLifecycleShape) {0};
    uint32_t matched_release = XR_SEMANTIC_INDEX_NONE;
    XrSemanticCoroutineLifecycleShape matched = {0};
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(plan);
    for (uint32_t operation = 0; operation < operation_count; operation++) {
        XrSemanticCoroutineLifecycleShape candidate = {0};
        if (!xr_semantic_owned_string_coroutine_lifecycle_for_release_is_exact(
                plan, graph, state_entity_index, operation, &candidate) ||
            candidate.producer_operation != producer_operation)
            continue;
        if (matched_release != XR_SEMANTIC_INDEX_NONE)
            return false;
        matched_release = operation;
        matched = candidate;
    }
    if (matched_release == XR_SEMANTIC_INDEX_NONE)
        return false;
    if (out)
        *out = matched;
    return true;
}

static inline const XrSemanticEntityRecord *
xr_semantic_coroutine_lifecycle_owner_entity(
    const XrSemanticPlan *plan,
    const XrSemanticCoroutineLifecycleShape *shape) {
    uint32_t count =
        plan ? (uint32_t) xr_semantic_plan_entity_count(plan) : 0;
    const XrSemanticEntityRecord *matched = NULL;
    for (uint32_t i = 0; shape && i < count; i++) {
        const XrSemanticEntityRecord *candidate =
            xr_semantic_plan_entity(plan, i);
        if (candidate->kind != XR_SEM_ENTITY_OWNER ||
            candidate->subject_kind != XR_SEM_ENTITY_SUBJECT_OWNER ||
            candidate->subject != shape->owner)
            continue;
        if (matched)
            return NULL;
        matched = candidate;
    }
    return matched;
}

#endif  // XR_SEMANTIC_COROUTINE_LIFECYCLE_SHAPE_H
