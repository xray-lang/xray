/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_coroutine_function_shape.h - Frozen function suspension authority
 *
 * KEY CONCEPT:
 *   Suspension is reconstructed from operations and exact call relationships,
 *   independently of coroutine state rows. Unresolved dependency effects do
 *   not prove a synchronous body; an indirect callback conservatively suspends.
 */
#ifndef XR_SEMANTIC_COROUTINE_FUNCTION_SHAPE_H
#define XR_SEMANTIC_COROUTINE_FUNCTION_SHAPE_H

#include "xr_semantic_class_shape.h"
#include "../../base/xmalloc.h"
#include "../../ir/xi.h"

typedef struct XrSemanticFunctionSuspensionWork {
    uint8_t *facts;
    uint8_t *covered_operations;
    uint32_t *head;
    uint32_t *next;
    uint32_t *queue;
} XrSemanticFunctionSuspensionWork;

/* A local constructor target names a declaration instead of a callee function
 * ID. Recover its unique body from the frozen declaration and argument shape.
 * An allocation-only declaration has no body to enter. */
static inline bool xr_semantic_constructor_local_function(
    const XrSemanticPlan *plan, const XrSemanticCallTargetRecord *target, uint32_t *out) {
    if (!plan || !target || !out ||
        target->kind != XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR ||
        target->dependency != XR_SEMANTIC_INDEX_NONE ||
        target->function != XR_SEMANTIC_INDEX_NONE ||
        target->source_export != XR_SEMANTIC_INDEX_NONE)
        return false;
    XrStableId zero = {{0}};
    if (!xr_stable_id_equal(target->export_identity, zero) ||
        !xr_stable_id_equal(target->callee_function, zero))
        return false;
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(plan, target->operation);
    uint32_t source_class = xr_semantic_class_construction_source_class(plan, operation);
    if (source_class == XR_SEMANTIC_INDEX_NONE ||
        target->callable_type != operation->result_type)
        return false;
    uint32_t constructor = xr_semantic_class_constructor_function(plan, source_class);
    *out = constructor;
    return true;
}

static inline void xr_semantic_function_suspension_dispose(XrSemanticFunctionSuspensionWork *work) {
    xr_free(work->facts);
    xr_free(work->covered_operations);
    xr_free(work->head);
    xr_free(work->next);
    xr_free(work->queue);
}

static inline bool xr_semantic_function_suspension_edges(const XrSemanticPlan *plan,
                                                         XrSemanticFunctionSuspensionWork *work) {
    uint32_t count = (uint32_t) xr_semantic_plan_function_count(plan);
    for (uint32_t f = 0; f < count; f++)
        work->head[f] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t t = 0; t < xr_semantic_plan_call_target_count(plan); t++) {
        const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(plan, t);
        const XrSemanticOperationRecord *operation =
            target ? xr_semantic_plan_operation(plan, target->operation) : NULL;
        work->next[t] = XR_SEMANTIC_INDEX_NONE;
        if (!target || !operation || operation->function >= count ||
            work->covered_operations[target->operation] != 0)
            return false;
        work->covered_operations[target->operation] = 1;
        uint32_t callee = XR_SEMANTIC_INDEX_NONE;
        if (target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL ||
            target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL ||
            target->kind == XR_SEM_CALL_TARGET_SOURCE_STATIC_METHOD_LOCAL ||
            target->kind == XR_SEM_CALL_TARGET_SOURCE_TEMPLATE_METHOD_LOCAL) {
            callee = target->function;
            if (callee >= count)
                return false;
        } else if (target->kind == XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR &&
                   target->dependency == XR_SEMANTIC_INDEX_NONE) {
            if (!xr_semantic_constructor_local_function(plan, target, &callee))
                return false;
        } else if (target->kind == XR_SEM_CALL_TARGET_NATIVE_YIELDABLE ||
                   target->kind == XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE ||
                   target->kind == XR_SEM_CALL_TARGET_BUILTIN_INSTANCE_YIELDABLE ||
                   target->kind == XR_SEM_CALL_TARGET_INDIRECT_CALLABLE) {
            work->facts[operation->function] |= 1u;
        } else if (target->kind == 0u ||
                   target->kind == XR_SEM_CALL_TARGET_SOURCE_EXPORT ||
                   target->kind == XR_SEM_CALL_TARGET_SOURCE_METHOD_DEPENDENCY ||
                   target->kind == XR_SEM_CALL_TARGET_SOURCE_STATIC_METHOD_DEPENDENCY ||
                   target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_SEALED_CANDIDATE ||
                   (target->kind == XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR &&
                    target->dependency != XR_SEMANTIC_INDEX_NONE)) {
            work->facts[operation->function] |= 2u;
        }
        if (callee != XR_SEMANTIC_INDEX_NONE) {
            work->next[t] = work->head[callee];
            work->head[callee] = t;
        }
    }
    for (uint32_t o = 0; o < xr_semantic_plan_operation_count(plan); o++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(plan, o);
        if (!operation || operation->function >= count)
            return false;
        if ((operation->effects & XI_EFFECT_MAY_SUSPEND) != 0 || operation->opcode == XI_GO)
            work->facts[operation->function] |= 1u;
        if (work->covered_operations[o] == 0 &&
            (operation->opcode == XI_CALL || operation->opcode == XI_TAIL_CALL ||
             operation->opcode == XI_CALL_METHOD))
            work->facts[operation->function] |= 2u;
    }
    return true;
}

static inline int xr_semantic_function_frozen_suspendability(const XrSemanticPlan *plan,
                                                             uint32_t selected) {
    if (!plan || !xr_semantic_plan_is_verified(plan))
        return -1;
    uint32_t count = (uint32_t) xr_semantic_plan_function_count(plan);
    uint32_t targets = (uint32_t) xr_semantic_plan_call_target_count(plan);
    uint32_t operations = (uint32_t) xr_semantic_plan_operation_count(plan);
    if (selected >= count)
        return -1;
    XrSemanticFunctionSuspensionWork work = {
        .facts = (uint8_t *) xr_calloc(count, 1),
        .covered_operations = operations ? (uint8_t *) xr_calloc(operations, 1) : NULL,
        .head = (uint32_t *) xr_malloc((size_t) count * sizeof(uint32_t)),
        .next = targets ? (uint32_t *) xr_malloc((size_t) targets * sizeof(uint32_t)) : NULL,
        .queue = (uint32_t *) xr_malloc((size_t) count * sizeof(uint32_t)),
    };
    if (!work.facts || !work.head || !work.queue || (targets && !work.next) ||
        (operations && !work.covered_operations) ||
        !xr_semantic_function_suspension_edges(plan, &work)) {
        xr_semantic_function_suspension_dispose(&work);
        return -1;
    }
    for (uint8_t fact = 1; fact <= 2; fact++) {
        uint32_t begin = 0, end = 0;
        for (uint32_t f = 0; f < count; f++)
            if ((work.facts[f] & fact) != 0)
                work.queue[end++] = f;
        while (begin < end) {
            uint32_t callee = work.queue[begin++];
            for (uint32_t edge = work.head[callee]; edge != XR_SEMANTIC_INDEX_NONE;
                 edge = work.next[edge]) {
                const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(plan, edge);
                const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(plan, target->operation);
                uint32_t caller = operation->function;
                if ((work.facts[caller] & fact) == 0) {
                    work.facts[caller] |= fact;
                    work.queue[end++] = caller;
                }
            }
        }
    }
    uint8_t result = work.facts[selected];
    xr_semantic_function_suspension_dispose(&work);
    return (result & 1u) != 0 ? 1 : (result & 2u) != 0 ? -1 : 0;
}

#endif  // XR_SEMANTIC_COROUTINE_FUNCTION_SHAPE_H
