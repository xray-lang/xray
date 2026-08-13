/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_reference_count_core.h - Runtime-neutral reference-count actions.
 */

#ifndef XR_REFERENCE_COUNT_CORE_H
#define XR_REFERENCE_COUNT_CORE_H

#include "xr_semantic_owner_ids_gen.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum XrReferenceCountAction {
    XR_REFERENCE_COUNT_INVALID = 0,
    XR_REFERENCE_COUNT_RETAIN,
    XR_REFERENCE_COUNT_RELEASE
} XrReferenceCountAction;

typedef struct XrReferenceCountPlan {
    XrReferenceCountAction action;
    bool acquires_owner;
    bool relinquishes_owner;
    bool destroys_on_last_release;
} XrReferenceCountPlan;

static inline XrReferenceCountPlan xr_reference_count_plan_core(
    XrReferenceCountAction action) {
    XrReferenceCountPlan plan = {action, false, false, false};
    if (action == XR_REFERENCE_COUNT_RETAIN) {
        plan.acquires_owner = true;
    } else if (action == XR_REFERENCE_COUNT_RELEASE) {
        plan.relinquishes_owner = true;
        plan.destroys_on_last_release = true;
    }
    return plan;
}

static inline bool xr_reference_count_plan_is_exact_core(XrReferenceCountPlan plan) {
    if (plan.action == XR_REFERENCE_COUNT_RETAIN)
        return plan.acquires_owner && !plan.relinquishes_owner &&
               !plan.destroys_on_last_release;
    if (plan.action == XR_REFERENCE_COUNT_RELEASE)
        return !plan.acquires_owner && plan.relinquishes_owner &&
               plan.destroys_on_last_release;
    return false;
}

#define XR_REFERENCE_COUNT_OWNER_GUARD(owner_hi, owner_lo)                                      \
    ((void) sizeof(struct {                                                                      \
        unsigned int owner_id_must_be_shared_reference_count                                    \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_REFERENCE_COUNT_HI &&           \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_REFERENCE_COUNT_LO)             \
                   ? 1                                                                          \
                   : -1);                                                                       \
    }))

#define XR_REFERENCE_COUNT_CONSUMER_GUARD(consumer_bit)                                         \
    ((void) sizeof(struct {                                                                      \
        unsigned int consumer_must_be_declared_for_shared_reference_count                       \
            : (((uint32_t) (consumer_bit) != 0 &&                                               \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&         \
                (XR_SEM_OWNER_ID_SHARED_REFERENCE_COUNT_CONSUMERS &                             \
                 (uint32_t) (consumer_bit)) != 0)                                               \
                   ? 1                                                                          \
                   : -1);                                                                       \
    }))

#define XR_REFERENCE_COUNT_OWNER_PLAN(owner_hi, owner_lo, consumer_bit, action)                 \
    (XR_REFERENCE_COUNT_OWNER_GUARD((owner_hi), (owner_lo)),                                    \
     XR_REFERENCE_COUNT_CONSUMER_GUARD((consumer_bit)),                                         \
     xr_reference_count_plan_core((action)))

#endif /* XR_REFERENCE_COUNT_CORE_H */
