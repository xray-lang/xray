/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_assert_condition_core.h - Runtime-neutral assertion condition semantics.
 *
 * Truthiness classification belongs to xr_truthy_core.h. This core owns only
 * the observable assertion decision after an adapter has classified the value.
 * Error construction, printing, aborting, and trapping remain backend-local.
 */

#ifndef XR_ASSERT_CONDITION_CORE_H
#define XR_ASSERT_CONDITION_CORE_H

#if !defined(XR_ASSERT_CONDITION_C90)
#include "xr_semantic_owner_ids_gen.h"
#include <stdbool.h>
#include <stdint.h>
#define XR_ASSERT_CONDITION_INLINE static inline
#else
/* Restricted C90 provides bool before including this core. */
#define XR_ASSERT_CONDITION_INLINE static
#endif

XR_ASSERT_CONDITION_INLINE bool xr_assert_condition_failed_core(bool truthy,
                                                                 bool expected_truthy) {
    return truthy != expected_truthy;
}

#if !defined(XR_ASSERT_CONDITION_C90)
#define XR_ASSERT_CONDITION_OWNER_GUARD(owner_hi, owner_lo)                                      \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_assert_condition                                    \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_ASSERT_CONDITION_HI &&           \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_ASSERT_CONDITION_LO)              \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_ASSERT_CONDITION_CONSUMER_GUARD(consumer_bit)                                         \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_assert_condition                       \
            : (((uint32_t) (consumer_bit) != 0 &&                                                 \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&           \
                (XR_SEM_OWNER_ID_SHARED_ASSERT_CONDITION_CONSUMERS &                              \
                 (uint32_t) (consumer_bit)) != 0)                                                 \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_ASSERT_CONDITION_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, truthy,                \
                                        expected_truthy)                                          \
    (XR_ASSERT_CONDITION_OWNER_GUARD((owner_hi), (owner_lo)),                                     \
     XR_ASSERT_CONDITION_CONSUMER_GUARD((consumer_bit)),                                          \
     xr_assert_condition_failed_core((truthy), (expected_truthy)))
#endif

#undef XR_ASSERT_CONDITION_INLINE

#endif /* XR_ASSERT_CONDITION_CORE_H */
