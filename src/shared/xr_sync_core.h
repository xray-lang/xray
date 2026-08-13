/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_sync_core.h - Single semantic source for standalone sync primitives
 * shared by the VM stdlib binding and the AOT direct-call helper.
 *
 * Self-contained (only xr_atomic_compat.h) so the AOT prelude can adopt it while
 * keeping the zero-runtime-symbol contract (consumed via a static-inline
 * wrapper in src/aot/xrt_mem.h).
 *
 * Ordering codes mirror the prelude `Ordering` enum ordinals
 * (see stdlib/prelude/prelude.c): 0 Relaxed, 1 Acquire, 2 Release,
 * 3 AcquireRelease, 4 SeqCst.
 */

#ifndef XR_SYNC_CORE_H
#define XR_SYNC_CORE_H

#include "xr_atomic_compat.h"
#include "xr_semantic_owner_ids_gen.h"
#include <stdint.h>

typedef enum XrAtomicLoadOrder {
    XR_ATOMIC_LOAD_ORDER_RELAXED = 0,
    XR_ATOMIC_LOAD_ORDER_ACQUIRE,
    XR_ATOMIC_LOAD_ORDER_SEQ_CST
} XrAtomicLoadOrder;

typedef struct XrAtomicLoadPlan {
    XrAtomicLoadOrder order;
    int64_t canonical_ordering;
    int valid;
} XrAtomicLoadPlan;

static inline XrAtomicLoadPlan xr_atomic_load_plan_core(int64_t ordering) {
    XrAtomicLoadPlan plan = {XR_ATOMIC_LOAD_ORDER_SEQ_CST, ordering, 1};
    switch (ordering) {
        case 0:
        case 2:
            plan.order = XR_ATOMIC_LOAD_ORDER_RELAXED;
            return plan;
        case 1:
        case 3:
            plan.order = XR_ATOMIC_LOAD_ORDER_ACQUIRE;
            return plan;
        case 4:
            return plan;
        default:
            plan.canonical_ordering = 4;
            plan.valid = 0;
            return plan;
    }
}

static inline int xr_atomic_load_plan_is_exact_core(XrAtomicLoadPlan plan) {
    if (!plan.valid || plan.canonical_ordering < 0 || plan.canonical_ordering > 4)
        return 0;
    return plan.order == XR_ATOMIC_LOAD_ORDER_RELAXED ||
           plan.order == XR_ATOMIC_LOAD_ORDER_ACQUIRE ||
           plan.order == XR_ATOMIC_LOAD_ORDER_SEQ_CST;
}

static inline memory_order xr_atomic_load_plan_c11_order_core(XrAtomicLoadPlan plan) {
    switch (plan.order) {
        case XR_ATOMIC_LOAD_ORDER_RELAXED:
            return memory_order_relaxed;
        case XR_ATOMIC_LOAD_ORDER_ACQUIRE:
            return memory_order_acquire;
        case XR_ATOMIC_LOAD_ORDER_SEQ_CST:
        default:
            return memory_order_seq_cst;
    }
}

#define XR_ATOMIC_LOAD_OWNER_GUARD(owner_hi, owner_lo)                                          \
    ((void) sizeof(struct {                                                                      \
        unsigned int owner_id_must_be_shared_atomic_load                                        \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_ATOMIC_LOAD_HI &&               \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_ATOMIC_LOAD_LO)                 \
                   ? 1                                                                          \
                   : -1);                                                                       \
    }))

#define XR_ATOMIC_LOAD_CONSUMER_GUARD(consumer_bit)                                             \
    ((void) sizeof(struct {                                                                      \
        unsigned int consumer_must_be_declared_for_shared_atomic_load                           \
            : (((uint32_t) (consumer_bit) != 0 &&                                               \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&         \
                (XR_SEM_OWNER_ID_SHARED_ATOMIC_LOAD_CONSUMERS & (uint32_t) (consumer_bit)) != 0)\
                   ? 1                                                                          \
                   : -1);                                                                       \
    }))

#define XR_ATOMIC_LOAD_OWNER_PLAN(owner_hi, owner_lo, consumer_bit, ordering)                   \
    (XR_ATOMIC_LOAD_OWNER_GUARD((owner_hi), (owner_lo)),                                        \
     XR_ATOMIC_LOAD_CONSUMER_GUARD((consumer_bit)),                                             \
     xr_atomic_load_plan_core((ordering)))

static inline memory_order xr_sync_core_memorder(int64_t ordering) {
    switch (ordering) {
        case 0:
            return memory_order_relaxed;
        case 1:
            return memory_order_acquire;
        case 2:
            return memory_order_release;
        case 3:
            return memory_order_acq_rel;
        case 4:
        default:
            return memory_order_seq_cst;
    }
}

/* Standalone memory fence (C11 atomic_thread_fence). Not tied to any atomic
 * variable; orders prior/subsequent memory ops per `ordering`. */
static inline void xr_sync_core_fence(int64_t ordering) {
    atomic_thread_fence(xr_sync_core_memorder(ordering));
}

#endif  // XR_SYNC_CORE_H
