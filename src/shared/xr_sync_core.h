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
 * (see module/xprelude_runtime.c): 0 Relaxed, 1 Acquire, 2 Release,
 * 3 AcquireRelease, 4 SeqCst.
 */

#ifndef XR_SYNC_CORE_H
#define XR_SYNC_CORE_H

#ifndef XR_ATOMIC_COMPAT_H
#include "xr_atomic_compat.h"
#endif
#ifndef XR_SEMANTIC_OWNER_IDS_GEN_H
#include "xr_semantic_owner_ids_gen.h"
#endif
#include <stdint.h>
#include <string.h>

/* A generated translation unit embeds this library with only the helpers
 * needed by its Program. Keep the unused-inline convention of shared kernels. */
#if defined(__GNUC__) || defined(__clang__)
#define XR_SYNC_CORE_FUNCTION static inline __attribute__((unused))
#else
#define XR_SYNC_CORE_FUNCTION static inline
#endif

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

typedef enum XrAtomicStoreOrder {
    XR_ATOMIC_STORE_ORDER_RELAXED = 0,
    XR_ATOMIC_STORE_ORDER_RELEASE,
    XR_ATOMIC_STORE_ORDER_SEQ_CST
} XrAtomicStoreOrder;

typedef struct XrAtomicStorePlan {
    XrAtomicStoreOrder order;
    int64_t canonical_ordering;
    int valid;
} XrAtomicStorePlan;

XR_SYNC_CORE_FUNCTION XrAtomicLoadPlan xr_atomic_load_plan_core(int64_t ordering) {
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

XR_SYNC_CORE_FUNCTION int xr_atomic_load_plan_is_exact_core(XrAtomicLoadPlan plan) {
    if (!plan.valid || plan.canonical_ordering < 0 || plan.canonical_ordering > 4)
        return 0;
    return plan.order == XR_ATOMIC_LOAD_ORDER_RELAXED ||
           plan.order == XR_ATOMIC_LOAD_ORDER_ACQUIRE ||
           plan.order == XR_ATOMIC_LOAD_ORDER_SEQ_CST;
}

XR_SYNC_CORE_FUNCTION memory_order xr_atomic_load_plan_c11_order_core(XrAtomicLoadPlan plan) {
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

XR_SYNC_CORE_FUNCTION XrAtomicStorePlan xr_atomic_store_plan_core(int64_t ordering) {
    XrAtomicStorePlan plan = {XR_ATOMIC_STORE_ORDER_SEQ_CST, ordering, 1};
    switch (ordering) {
        case 0:
        case 1:
            plan.order = XR_ATOMIC_STORE_ORDER_RELAXED;
            return plan;
        case 2:
        case 3:
            plan.order = XR_ATOMIC_STORE_ORDER_RELEASE;
            return plan;
        case 4:
            return plan;
        default:
            plan.canonical_ordering = 4;
            plan.valid = 0;
            return plan;
    }
}

XR_SYNC_CORE_FUNCTION int xr_atomic_store_plan_is_exact_core(XrAtomicStorePlan plan) {
    if (!plan.valid || plan.canonical_ordering < 0 || plan.canonical_ordering > 4)
        return 0;
    return plan.order == XR_ATOMIC_STORE_ORDER_RELAXED ||
           plan.order == XR_ATOMIC_STORE_ORDER_RELEASE ||
           plan.order == XR_ATOMIC_STORE_ORDER_SEQ_CST;
}

XR_SYNC_CORE_FUNCTION memory_order xr_atomic_store_plan_c11_order_core(XrAtomicStorePlan plan) {
    switch (plan.order) {
        case XR_ATOMIC_STORE_ORDER_RELAXED:
            return memory_order_relaxed;
        case XR_ATOMIC_STORE_ORDER_RELEASE:
            return memory_order_release;
        case XR_ATOMIC_STORE_ORDER_SEQ_CST:
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

#define XR_ATOMIC_STORE_OWNER_GUARD(owner_hi, owner_lo)                                         \
    ((void) sizeof(struct {                                                                      \
        unsigned int owner_id_must_be_shared_atomic_store                                       \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_ATOMIC_STORE_HI &&              \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_ATOMIC_STORE_LO)                \
                   ? 1                                                                          \
                   : -1);                                                                       \
    }))

#define XR_ATOMIC_STORE_CONSUMER_GUARD(consumer_bit)                                            \
    ((void) sizeof(struct {                                                                      \
        unsigned int consumer_must_be_declared_for_shared_atomic_store                          \
            : (((uint32_t) (consumer_bit) != 0 &&                                               \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&         \
                (XR_SEM_OWNER_ID_SHARED_ATOMIC_STORE_CONSUMERS & (uint32_t) (consumer_bit)) != 0)\
                   ? 1                                                                          \
                   : -1);                                                                       \
    }))

#define XR_ATOMIC_STORE_OWNER_PLAN(owner_hi, owner_lo, consumer_bit, ordering)                  \
    (XR_ATOMIC_STORE_OWNER_GUARD((owner_hi), (owner_lo)),                                       \
     XR_ATOMIC_STORE_CONSUMER_GUARD((consumer_bit)),                                            \
     xr_atomic_store_plan_core((ordering)))

XR_SYNC_CORE_FUNCTION memory_order xr_sync_core_memorder(int64_t ordering) {
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

/* One synchronized identity, independent of any executor allocation list.
 * Executors allocate/free the enclosing storage. Retain requires an existing
 * live owner; it cannot resurrect memory after the last release. Only the
 * thread receiving LAST may reclaim storage. Count overflow fails unchanged. */
typedef struct XrAtomicStorageCore {
    _Atomic(uint32_t) owners;
    _Atomic(int64_t) value;
} XrAtomicStorageCore;

typedef enum XrAtomicStorageRelease {
    XR_ATOMIC_STORAGE_RELEASE_INVALID = 0,
    XR_ATOMIC_STORAGE_RELEASE_RETAINED,
    XR_ATOMIC_STORAGE_RELEASE_LAST
} XrAtomicStorageRelease;

XR_SYNC_CORE_FUNCTION void xr_atomic_storage_init_core(XrAtomicStorageCore *storage, int64_t value) {
    atomic_init(&storage->owners, 1u);
    atomic_init(&storage->value, value);
}

XR_SYNC_CORE_FUNCTION int xr_atomic_storage_retain_core(XrAtomicStorageCore *storage) {
    uint32_t observed = atomic_load_explicit(&storage->owners, memory_order_relaxed);
    while (observed != 0u && observed != UINT32_MAX) {
        if (atomic_compare_exchange_weak_explicit(&storage->owners, &observed, observed + 1u,
                                                   memory_order_relaxed, memory_order_relaxed))
            return 1;
    }
    return 0;
}

XR_SYNC_CORE_FUNCTION XrAtomicStorageRelease xr_atomic_storage_release_core(XrAtomicStorageCore *storage) {
    uint32_t observed = atomic_load_explicit(&storage->owners, memory_order_relaxed);
    while (observed != 0u) {
        if (atomic_compare_exchange_weak_explicit(&storage->owners, &observed, observed - 1u,
                                                   memory_order_acq_rel, memory_order_relaxed))
            return observed == 1u ? XR_ATOMIC_STORAGE_RELEASE_LAST
                                  : XR_ATOMIC_STORAGE_RELEASE_RETAINED;
    }
    return XR_ATOMIC_STORAGE_RELEASE_INVALID;
}

/* Scalar operations do not own storage. The caller must hold a live owner
 * throughout the operation; synchronization does not extend object lifetime. */
XR_SYNC_CORE_FUNCTION int64_t xr_atomic_i64_load_core(const _Atomic(int64_t) *storage,
                                              int64_t ordering) {
    XrAtomicLoadPlan plan = xr_atomic_load_plan_core(ordering);
    return atomic_load_explicit(storage, xr_atomic_load_plan_c11_order_core(plan));
}

XR_SYNC_CORE_FUNCTION void xr_atomic_i64_store_core(_Atomic(int64_t) *storage, int64_t value,
                                             int64_t ordering) {
    XrAtomicStorePlan plan = xr_atomic_store_plan_core(ordering);
    atomic_store_explicit(storage, value, xr_atomic_store_plan_c11_order_core(plan));
}

XR_SYNC_CORE_FUNCTION int64_t xr_atomic_i64_exchange_core(_Atomic(int64_t) *storage, int64_t value,
                                                  int64_t ordering) {
    return atomic_exchange_explicit(storage, value, xr_sync_core_memorder(ordering));
}

/* Strong CAS cannot spuriously fail. A failed comparison returns the actual
 * observed bits through expected and does not modify storage. */
XR_SYNC_CORE_FUNCTION int xr_atomic_i64_compare_exchange_core(_Atomic(int64_t) *storage,
                                                       int64_t *expected, int64_t desired,
                                                       int64_t ordering) {
    return atomic_compare_exchange_strong_explicit(storage, expected, desired,
                                                    xr_sync_core_memorder(ordering),
                                                    memory_order_relaxed);
}

/* C11 atomic signed arithmetic is defined to wrap, including INT64_MIN. */
XR_SYNC_CORE_FUNCTION int64_t xr_atomic_i64_fetch_add_core(_Atomic(int64_t) *storage, int64_t value,
                                                   int64_t ordering) {
    return atomic_fetch_add_explicit(storage, value, xr_sync_core_memorder(ordering));
}

XR_SYNC_CORE_FUNCTION int64_t xr_atomic_i64_fetch_sub_core(_Atomic(int64_t) *storage, int64_t value,
                                                   int64_t ordering) {
    return atomic_fetch_sub_explicit(storage, value, xr_sync_core_memorder(ordering));
}

XR_SYNC_CORE_FUNCTION int64_t xr_atomic_i64_fetch_xor_core(_Atomic(int64_t) *storage, int64_t value,
                                                   int64_t ordering) {
    return atomic_fetch_xor_explicit(storage, value, xr_sync_core_memorder(ordering));
}

/* Operate on scalar storage, independent of executor and object lifetime.
 * A failed CAS supplies the next observed bit pattern, including NaNs. Keep
 * subtraction distinct from adding a negated operand to preserve signed zero. */
XR_SYNC_CORE_FUNCTION double xr_atomic_f64_fetch_update_core(_Atomic(int64_t) *storage, double operand,
                                                      int subtract, int64_t ordering) {
    XrAtomicLoadPlan load = xr_atomic_load_plan_core(ordering);
    int64_t observed = atomic_load_explicit(storage, xr_atomic_load_plan_c11_order_core(load));
    for (;;) {
        double previous;
        memcpy(&previous, &observed, sizeof(previous));
        double next = subtract ? previous - operand : previous + operand;
        int64_t desired;
        memcpy(&desired, &next, sizeof(desired));
        if (atomic_compare_exchange_weak_explicit(storage, &observed, desired,
                                                   xr_sync_core_memorder(ordering),
                                                   memory_order_relaxed))
            return previous;
    }
}

/* Standalone memory fence (C11 atomic_thread_fence). Not tied to any atomic
 * variable; orders prior/subsequent memory ops per `ordering`. */
XR_SYNC_CORE_FUNCTION void xr_sync_core_fence(int64_t ordering) {
    atomic_thread_fence(xr_sync_core_memorder(ordering));
}

#endif  // XR_SYNC_CORE_H
