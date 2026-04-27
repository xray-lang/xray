/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmutex.h - Cross-platform three-state mutex
 *
 * KEY CONCEPT:
 *   Three-state lock optimized for short critical sections.
 *   States: UNLOCKED(0), LOCKED(1), SLEEPING(2)
 *
 * LOCK ACQUISITION STRATEGY:
 *   1. Fast CAS attempt
 *   2. Active spin (4 iterations with CPU pause)
 *   3. Passive spin (1 sched_yield)
 *   4. True sleep (futex/WaitOnAddress/pthread_cond)
 */

#ifndef XMUTEX_H
#define XMUTEX_H

#include <stdatomic.h>
#include <stdbool.h>
#include "xplatform.h"

// Platform-specific headers
#ifdef XR_OS_WINDOWS
#include <windows.h>
#elif defined(XR_OS_LINUX)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sched.h>
#elif defined(XR_OS_MACOS)
#include "../os/os_thread.h"
#include <sched.h>
// Darwin kernel futex equivalent (used by Go runtime, Rust std, etc.)
extern int __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout_us);
extern int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);
#define XR_UL_COMPARE_AND_WAIT 1
#endif  // Lock state constants
#define XR_AMUTEX_UNLOCKED 0
#define XR_AMUTEX_LOCKED 1
#define XR_AMUTEX_SLEEPING 2

// Spin parameters
#define XR_ACTIVE_SPIN 4       // Active spin iterations
#define XR_ACTIVE_SPIN_CNT 30  // CPU pause per iteration
#define XR_PASSIVE_SPIN 1      // Passive spin iterations

/* ========== Cross-platform Three-state Lock ========== */

// Unified mutex structure
typedef struct XrAdaptiveMutex {
    _Atomic(int) state;
} XrAdaptiveMutex;

static inline void xr_amutex_init(XrAdaptiveMutex *m) {
    atomic_store_explicit(&m->state, XR_AMUTEX_UNLOCKED, memory_order_relaxed);
}

// Platform-specific futex operations
#ifdef XR_OS_LINUX

static inline void xr_futex_wait(XrAdaptiveMutex *m, int expected) {
    syscall(SYS_futex, &m->state, FUTEX_WAIT_PRIVATE, expected, NULL, NULL, 0);
}

static inline void xr_futex_wake(XrAdaptiveMutex *m) {
    syscall(SYS_futex, &m->state, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

#elif defined(XR_OS_WINDOWS)

static inline void xr_futex_wait(XrAdaptiveMutex *m, int expected) {
    WaitOnAddress(&m->state, &expected, sizeof(int), INFINITE);
}

static inline void xr_futex_wake(XrAdaptiveMutex *m) {
    WakeByAddressSingle(&m->state);
}

#elif defined(XR_OS_MACOS)

/* macOS: use __ulock_wait/__ulock_wake (Darwin kernel futex equivalent).
 * Blocks the calling thread when *addr == expected, wakes on __ulock_wake.
 * Same semantics as Linux FUTEX_WAIT/FUTEX_WAKE. */
static inline void xr_futex_wait(XrAdaptiveMutex *m, int expected) {
    __ulock_wait(XR_UL_COMPARE_AND_WAIT, &m->state, (uint64_t) (uint32_t) expected, 0);
}

static inline void xr_futex_wake(XrAdaptiveMutex *m) {
    __ulock_wake(XR_UL_COMPARE_AND_WAIT, &m->state, 0);
}

#endif  // ========== Unified lock/unlock Interface ==========

static inline void xr_amutex_lock(XrAdaptiveMutex *m) {
    // Fast path: try to acquire unlocked mutex
    int expected = XR_AMUTEX_UNLOCKED;
    if (atomic_compare_exchange_strong_explicit(&m->state, &expected, XR_AMUTEX_LOCKED,
                                                memory_order_acquire, memory_order_relaxed)) {
        return;
    }

    // Slow path
    int wait = XR_AMUTEX_LOCKED;

    for (;;) {
        // Active spin with CPU pause
        for (int i = 0; i < XR_ACTIVE_SPIN; i++) {
            while (atomic_load_explicit(&m->state, memory_order_relaxed) == XR_AMUTEX_UNLOCKED) {
                expected = XR_AMUTEX_UNLOCKED;
                if (atomic_compare_exchange_weak_explicit(
                        &m->state, &expected, wait, memory_order_acquire, memory_order_relaxed)) {
                    return;
                }
            }
            // CPU pause instruction
            for (int j = 0; j < XR_ACTIVE_SPIN_CNT; j++) {
                XR_CPU_PAUSE();
            }
        }

        // Passive spin with sched_yield
        for (int i = 0; i < XR_PASSIVE_SPIN; i++) {
            while (atomic_load_explicit(&m->state, memory_order_relaxed) == XR_AMUTEX_UNLOCKED) {
                expected = XR_AMUTEX_UNLOCKED;
                if (atomic_compare_exchange_weak_explicit(
                        &m->state, &expected, wait, memory_order_acquire, memory_order_relaxed)) {
                    return;
                }
            }
#ifdef XR_OS_WINDOWS
            SwitchToThread();
#else
            sched_yield();
#endif
        }

        // True sleep
        int v = atomic_exchange_explicit(&m->state, XR_AMUTEX_SLEEPING, memory_order_acquire);
        if (v == XR_AMUTEX_UNLOCKED) {
            return;
        }
        wait = XR_AMUTEX_SLEEPING;
        xr_futex_wait(m, XR_AMUTEX_SLEEPING);
    }
}

static inline void xr_amutex_unlock(XrAdaptiveMutex *m) {
    int v = atomic_exchange_explicit(&m->state, XR_AMUTEX_UNLOCKED, memory_order_release);
    if (v == XR_AMUTEX_SLEEPING) {
        // Wake up waiters
        xr_futex_wake(m);
    }
}

static inline bool xr_amutex_trylock(XrAdaptiveMutex *m) {
    int expected = XR_AMUTEX_UNLOCKED;
    return atomic_compare_exchange_strong_explicit(&m->state, &expected, XR_AMUTEX_LOCKED,
                                                   memory_order_acquire, memory_order_relaxed);
}

#endif  // XMUTEX_H
