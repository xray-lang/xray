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
 * Self-contained (only <stdatomic.h>) so the AOT prelude can adopt it while
 * keeping the zero-runtime-symbol contract (consumed via a static-inline
 * wrapper in src/aot/xrt_mem.h).
 *
 * Ordering codes mirror the prelude `Ordering` enum ordinals
 * (see stdlib/prelude/prelude.c): 0 Relaxed, 1 Acquire, 2 Release,
 * 3 AcquireRelease, 4 SeqCst.
 */

#ifndef XR_SYNC_CORE_H
#define XR_SYNC_CORE_H

#include <stdatomic.h>
#include <stdint.h>

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
