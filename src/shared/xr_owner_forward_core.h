/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_owner_forward_core.h - Runtime-neutral owning-reference forwarding semantics.
 *
 * Owner forwarding preserves the represented value while consuming the source
 * owning reference and producing an owned result. Register allocation and C
 * representation conversion remain mechanical backend work.
 */

#ifndef XR_OWNER_FORWARD_CORE_H
#define XR_OWNER_FORWARD_CORE_H

#include "xr_semantic_owner_ids_gen.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct XrOwnerForwardPlan {
    bool preserves_value;
    bool consumes_source_owner;
    bool result_is_owned;
} XrOwnerForwardPlan;

static inline XrOwnerForwardPlan xr_owner_forward_plan_core(void) {
    XrOwnerForwardPlan plan = {true, true, true};
    return plan;
}

static inline bool xr_owner_forward_plan_is_exact_core(XrOwnerForwardPlan plan) {
    return plan.preserves_value && plan.consumes_source_owner && plan.result_is_owned;
}

#define XR_OWNER_FORWARD_OWNER_GUARD(owner_hi, owner_lo)                                        \
    ((void) sizeof(struct {                                                                       \
        unsigned int owner_id_must_be_shared_owner_forward                                      \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_OWNER_FORWARD_HI &&             \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_OWNER_FORWARD_LO)                \
                   ? 1                                                                           \
                   : -1);                                                                        \
    }))

#define XR_OWNER_FORWARD_CONSUMER_GUARD(consumer_bit)                                           \
    ((void) sizeof(struct {                                                                       \
        unsigned int consumer_must_be_declared_for_shared_owner_forward                         \
            : (((uint32_t) (consumer_bit) != 0 &&                                                \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&          \
                (XR_SEM_OWNER_ID_SHARED_OWNER_FORWARD_CONSUMERS &                               \
                 (uint32_t) (consumer_bit)) != 0)                                                \
                   ? 1                                                                           \
                   : -1);                                                                        \
    }))

#define XR_OWNER_FORWARD_OWNER_APPLY(owner_hi, owner_lo, consumer_bit)                          \
    (XR_OWNER_FORWARD_OWNER_GUARD((owner_hi), (owner_lo)),                                       \
     XR_OWNER_FORWARD_CONSUMER_GUARD((consumer_bit)), xr_owner_forward_plan_core())

#endif /* XR_OWNER_FORWARD_CORE_H */
