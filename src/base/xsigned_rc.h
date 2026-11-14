/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsigned_rc.h - Counter-only local and shared ownership primitives
 *
 * KEY CONCEPT:
 *   Positive counts belong to one execution owner. Negative counts use CAS.
 *   Graph publication, binding authority and final destruction are caller-owned.
 */
#ifndef XSIGNED_RC_H
#define XSIGNED_RC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

typedef enum {
    XR_SIGNED_RC_ALIVE,
    XR_SIGNED_RC_LAST,
    XR_SIGNED_RC_INVALID
} XrSignedRcRelease;

static inline int32_t xr_signed_rc_read(const _Atomic(int32_t) *references) {
    return atomic_load_explicit(references, memory_order_relaxed);
}

static inline void xr_signed_rc_initialize(_Atomic(int32_t) *references) {
    atomic_init(references, 1);
}

static inline bool xr_signed_rc_retain(_Atomic(int32_t) *references) {
    int32_t count = xr_signed_rc_read(references);
    if (count > 0) {
        if (count == INT32_MAX)
            return false;
        atomic_store_explicit(references, count + 1, memory_order_relaxed);
        return true;
    }
    for (;;) {
        if (count >= 0 || count <= -INT32_MAX)
            return false;
        if (atomic_compare_exchange_weak_explicit(references, &count, count - 1,
                memory_order_relaxed, memory_order_relaxed))
            return true;
    }
}

/* Invalid counters are left untouched so each caller keeps its own diagnostic. */
static inline XrSignedRcRelease xr_signed_rc_release(_Atomic(int32_t) *references) {
    int32_t count = xr_signed_rc_read(references);
    if (count > 0) {
        atomic_store_explicit(references, count - 1, memory_order_relaxed);
        return count == 1 ? XR_SIGNED_RC_LAST : XR_SIGNED_RC_ALIVE;
    }
    for (;;) {
        if (count >= 0 || count == INT32_MIN)
            return XR_SIGNED_RC_INVALID;
        if (atomic_compare_exchange_weak_explicit(references, &count, count + 1,
                memory_order_acq_rel, memory_order_relaxed))
            return count == -1 ? XR_SIGNED_RC_LAST : XR_SIGNED_RC_ALIVE;
    }
}

/* Only value storage with separately established mutation authority uses this.
 * A publisher must finish the reachable graph before exposing a negative band. */
static inline bool xr_signed_rc_unique(_Atomic(int32_t) *references) {
    int32_t count = xr_signed_rc_read(references);
    if (count == 1)
        return true;
    if (count != -1)
        return false;
    return atomic_compare_exchange_strong_explicit(references, &count, 1,
        memory_order_acquire, memory_order_relaxed);
}

#endif // XSIGNED_RC_H
