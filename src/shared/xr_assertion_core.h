/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_assertion_core.h - Stable owner guard for typed assertion execution
 */

#ifndef XR_ASSERTION_CORE_H
#define XR_ASSERTION_CORE_H

#include "xr_assertion_plan.h"
#include "xr_semantic_owner_ids_gen.h"

/* AssertionPlan owns the schema and renderer. This guard makes every runtime
 * adapter prove at compile time that it consumes the generated owner ID; it
 * does not introduce a second assertion decision or renderer. */
#define XR_ASSERTION_OWNER_GUARD(owner_hi, owner_lo)                                               \
    ((void) sizeof(struct {                                                                         \
        unsigned int owner_id_must_be_shared_assertion                                             \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_ASSERTION_HI &&                    \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_ASSERTION_LO)                       \
                   ? 1                                                                              \
                   : -1);                                                                           \
    }))

#define XR_ASSERTION_CONSUMER_GUARD(consumer_bit)                                                  \
    ((void) sizeof(struct {                                                                         \
        unsigned int consumer_must_be_declared_for_shared_assertion                                \
            : (((uint32_t) (consumer_bit) != 0 &&                                                   \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&             \
                (XR_SEM_OWNER_ID_SHARED_ASSERTION_CONSUMERS &                                      \
                 (uint32_t) (consumer_bit)) != 0)                                                   \
                   ? 1                                                                              \
                   : -1);                                                                           \
    }))

#endif /* XR_ASSERTION_CORE_H */
