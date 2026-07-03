/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_sys.h - AOT helpers for sys.* OS-domain primitives.
 */

#ifndef XRT_SYS_H
#define XRT_SYS_H

#include "xrt_arc.h"
#include "xrt_method_symbols.h"
#include "xrt_value.h"
#include "../os/os_thread.h"
#include <stdatomic.h>
#if !defined(XR_OS_WINDOWS)
#include <time.h>
#endif

static inline XrValue xrt_closure_call0(XrValue callback);

typedef struct xrt_sys_mutex_object {
    xr_mutex_t mutex;
} xrt_sys_mutex_object_t;

typedef struct xrt_sys_rwlock_object {
    xr_rwlock_t rwlock;
} xrt_sys_rwlock_object_t;

typedef struct xrt_sys_condvar_object {
    xr_cond_t cond;
} xrt_sys_condvar_object_t;

typedef struct xrt_sys_barrier_object {
    xr_mutex_t mutex;
    xr_cond_t cond;
    int64_t parties;
    int64_t arrived;
    int64_t generation;
} xrt_sys_barrier_object_t;

typedef struct xrt_sys_once_object {
    xr_once_t once;
} xrt_sys_once_object_t;

typedef enum xrt_thread_state {
    XRT_THREAD_CREATED = 0,
    XRT_THREAD_JOINING,
    XRT_THREAD_JOINED,
    XRT_THREAD_DETACHED,
} xrt_thread_state_t;

typedef struct xrt_thread_object {
    xr_thread_t handle;
    _Atomic(int) state;
    _Atomic(bool) finished;
    XrValue retval;
} xrt_thread_object_t;

static XR_THREAD_LOCAL XrValue xrt_sys_once_callback = {.tag = XR_TAG_NULL};

static inline void xrt_sys_once_trampoline(void) {
    (void) xrt_closure_call0(xrt_sys_once_callback);
}

#if defined(XR_OS_WINDOWS)
static BOOL CALLBACK xrt_sys_once_win_thunk(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void) once;
    (void) param;
    (void) ctx;
    xrt_sys_once_trampoline();
    return TRUE;
}
#endif

static inline void xrt_sys_mutex_init(xr_mutex_t *mutex) {
#if defined(XR_OS_WINDOWS)
    InitializeSRWLock(mutex);
#else
    pthread_mutex_init(mutex, NULL);
#endif
}

static inline void xrt_sys_mutex_destroy(xr_mutex_t *mutex) {
#if defined(XR_OS_WINDOWS)
    (void) mutex;
#else
    pthread_mutex_destroy(mutex);
#endif
}

static inline void xrt_sys_mutex_lock_native(xr_mutex_t *mutex) {
#if defined(XR_OS_WINDOWS)
    AcquireSRWLockExclusive(mutex);
#else
    pthread_mutex_lock(mutex);
#endif
}

static inline void xrt_sys_mutex_unlock_native(xr_mutex_t *mutex) {
#if defined(XR_OS_WINDOWS)
    ReleaseSRWLockExclusive(mutex);
#else
    pthread_mutex_unlock(mutex);
#endif
}

static inline bool xrt_sys_mutex_trylock_native(xr_mutex_t *mutex) {
#if defined(XR_OS_WINDOWS)
    return TryAcquireSRWLockExclusive(mutex) != 0;
#else
    return pthread_mutex_trylock(mutex) == 0;
#endif
}

static inline void xrt_sys_rwlock_init(xr_rwlock_t *rwlock) {
#if defined(XR_OS_WINDOWS)
    InitializeSRWLock(rwlock);
#else
    pthread_rwlock_init(rwlock, NULL);
#endif
}

static inline void xrt_sys_rwlock_destroy(xr_rwlock_t *rwlock) {
#if defined(XR_OS_WINDOWS)
    (void) rwlock;
#else
    pthread_rwlock_destroy(rwlock);
#endif
}

static inline void xrt_sys_rwlock_rdlock_native(xr_rwlock_t *rwlock) {
#if defined(XR_OS_WINDOWS)
    AcquireSRWLockShared(rwlock);
#else
    pthread_rwlock_rdlock(rwlock);
#endif
}

static inline void xrt_sys_rwlock_rdunlock_native(xr_rwlock_t *rwlock) {
#if defined(XR_OS_WINDOWS)
    ReleaseSRWLockShared(rwlock);
#else
    pthread_rwlock_unlock(rwlock);
#endif
}

static inline void xrt_sys_rwlock_wrlock_native(xr_rwlock_t *rwlock) {
#if defined(XR_OS_WINDOWS)
    AcquireSRWLockExclusive(rwlock);
#else
    pthread_rwlock_wrlock(rwlock);
#endif
}

static inline void xrt_sys_rwlock_wrunlock_native(xr_rwlock_t *rwlock) {
#if defined(XR_OS_WINDOWS)
    ReleaseSRWLockExclusive(rwlock);
#else
    pthread_rwlock_unlock(rwlock);
#endif
}

static inline void xrt_sys_condvar_init(xr_cond_t *cond) {
#if defined(XR_OS_WINDOWS)
    InitializeConditionVariable(cond);
#else
    pthread_cond_init(cond, NULL);
#endif
}

static inline void xrt_sys_condvar_destroy(xr_cond_t *cond) {
#if defined(XR_OS_WINDOWS)
    (void) cond;
#else
    pthread_cond_destroy(cond);
#endif
}

static inline void xrt_sys_condvar_wait_native(xr_cond_t *cond, xr_mutex_t *mutex) {
#if defined(XR_OS_WINDOWS)
    SleepConditionVariableSRW(cond, mutex, INFINITE, 0);
#else
    pthread_cond_wait(cond, mutex);
#endif
}

static inline bool xrt_sys_condvar_wait_for_ns_native(xr_cond_t *cond, xr_mutex_t *mutex,
                                                      uint64_t timeout_ns) {
#if defined(XR_OS_WINDOWS)
    DWORD ms;
    if (timeout_ns >= (uint64_t) INFINITE * 1000000ULL)
        ms = INFINITE - 1;
    else
        ms = (DWORD) ((timeout_ns + 999999ULL) / 1000000ULL);
    return SleepConditionVariableSRW(cond, mutex, ms, 0) != 0;
#else
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    uint64_t total_ns = (uint64_t) deadline.tv_nsec + timeout_ns;
    deadline.tv_sec += (time_t) (total_ns / 1000000000ULL);
    deadline.tv_nsec = (long) (total_ns % 1000000000ULL);
    return pthread_cond_timedwait(cond, mutex, &deadline) == 0;
#endif
}

static inline void xrt_sys_condvar_signal_native(xr_cond_t *cond) {
#if defined(XR_OS_WINDOWS)
    WakeConditionVariable(cond);
#else
    pthread_cond_signal(cond);
#endif
}

static inline void xrt_sys_condvar_broadcast_native(xr_cond_t *cond) {
#if defined(XR_OS_WINDOWS)
    WakeAllConditionVariable(cond);
#else
    pthread_cond_broadcast(cond);
#endif
}

static inline void xrt_sys_once_init(xr_once_t *once) {
#if defined(XR_OS_WINDOWS)
    InitOnceInitialize(once);
#else
    xr_once_t init = XR_ONCE_INITIALIZER;
    *once = init;
#endif
}

static inline void xrt_sys_once_call_native(xr_once_t *once) {
#if defined(XR_OS_WINDOWS)
    InitOnceExecuteOnce(once, xrt_sys_once_win_thunk, NULL, NULL);
#else
    pthread_once(once, xrt_sys_once_trampoline);
#endif
}

static inline int xrt_sys_mutex_is(XrValue value) {
    return value.tag == XR_TAG_SYS_MUTEX && value.ptr != NULL;
}

static inline int xrt_sys_rwlock_is(XrValue value) {
    return value.tag == XR_TAG_SYS_RWLOCK && value.ptr != NULL;
}

static inline int xrt_sys_condvar_is(XrValue value) {
    return value.tag == XR_TAG_SYS_CONDVAR && value.ptr != NULL;
}

static inline int xrt_sys_barrier_is(XrValue value) {
    return value.tag == XR_TAG_SYS_BARRIER && value.ptr != NULL;
}

static inline int xrt_sys_once_is(XrValue value) {
    return value.tag == XR_TAG_SYS_ONCE && value.ptr != NULL;
}

static inline int xrt_thread_is(XrValue value) {
    return value.tag == XR_TAG_THREAD && value.ptr != NULL;
}

static inline xrt_sys_mutex_object_t *xrt_sys_mutex_ptr(XrValue value) {
    return xrt_sys_mutex_is(value) ? (xrt_sys_mutex_object_t *) value.ptr : NULL;
}

static inline xrt_sys_rwlock_object_t *xrt_sys_rwlock_ptr(XrValue value) {
    return xrt_sys_rwlock_is(value) ? (xrt_sys_rwlock_object_t *) value.ptr : NULL;
}

static inline xrt_sys_condvar_object_t *xrt_sys_condvar_ptr(XrValue value) {
    return xrt_sys_condvar_is(value) ? (xrt_sys_condvar_object_t *) value.ptr : NULL;
}

static inline xrt_sys_barrier_object_t *xrt_sys_barrier_ptr(XrValue value) {
    return xrt_sys_barrier_is(value) ? (xrt_sys_barrier_object_t *) value.ptr : NULL;
}

static inline xrt_sys_once_object_t *xrt_sys_once_ptr(XrValue value) {
    return xrt_sys_once_is(value) ? (xrt_sys_once_object_t *) value.ptr : NULL;
}

static inline xrt_thread_object_t *xrt_thread_ptr(XrValue value) {
    return xrt_thread_is(value) ? (xrt_thread_object_t *) value.ptr : NULL;
}

static inline XrValue xrt_sys_mutex_box(xrt_sys_mutex_object_t *mutex) {
    return mutex ? xr_mkptr(mutex, XR_TAG_SYS_MUTEX) : XR_NULL_VAL;
}

static inline XrValue xrt_sys_rwlock_box(xrt_sys_rwlock_object_t *rwlock) {
    return rwlock ? xr_mkptr(rwlock, XR_TAG_SYS_RWLOCK) : XR_NULL_VAL;
}

static inline XrValue xrt_sys_condvar_box(xrt_sys_condvar_object_t *condvar) {
    return condvar ? xr_mkptr(condvar, XR_TAG_SYS_CONDVAR) : XR_NULL_VAL;
}

static inline XrValue xrt_sys_barrier_box(xrt_sys_barrier_object_t *barrier) {
    return barrier ? xr_mkptr(barrier, XR_TAG_SYS_BARRIER) : XR_NULL_VAL;
}

static inline XrValue xrt_sys_once_box(xrt_sys_once_object_t *once) {
    return once ? xr_mkptr(once, XR_TAG_SYS_ONCE) : XR_NULL_VAL;
}

static inline XrValue xrt_thread_box(xrt_thread_object_t *thread) {
    return thread ? xr_mkptr(thread, XR_TAG_THREAD) : XR_NULL_VAL;
}

static inline XrValue xrt_sys_mutex_new(void) {
    xrt_sys_mutex_object_t *mutex = (xrt_sys_mutex_object_t *) xrt_arc_alloc(sizeof(*mutex));
    xrt_sys_mutex_init(&mutex->mutex);
    xrt_arc_mark_builtin(mutex, XRT_ARC_KIND_SYS_MUTEX);
    return xrt_sys_mutex_box(mutex);
}

static inline XrValue xrt_sys_rwlock_new(void) {
    xrt_sys_rwlock_object_t *rwlock = (xrt_sys_rwlock_object_t *) xrt_arc_alloc(sizeof(*rwlock));
    xrt_sys_rwlock_init(&rwlock->rwlock);
    xrt_arc_mark_builtin(rwlock, XRT_ARC_KIND_SYS_RWLOCK);
    return xrt_sys_rwlock_box(rwlock);
}

static inline XrValue xrt_sys_condvar_new(void) {
    xrt_sys_condvar_object_t *condvar =
        (xrt_sys_condvar_object_t *) xrt_arc_alloc(sizeof(*condvar));
    xrt_sys_condvar_init(&condvar->cond);
    xrt_arc_mark_builtin(condvar, XRT_ARC_KIND_SYS_CONDVAR);
    return xrt_sys_condvar_box(condvar);
}

static inline XrValue xrt_sys_barrier_new(XrValue parties_value) {
    int64_t parties =
        (parties_value.tag == XR_TAG_I64 && parties_value.i > 0) ? parties_value.i : 0;
    if (parties <= 0) {
        fprintf(stderr, "sys.Barrier parties must be > 0\n");
        abort();
    }
    xrt_sys_barrier_object_t *barrier =
        (xrt_sys_barrier_object_t *) xrt_arc_alloc(sizeof(*barrier));
    xrt_sys_mutex_init(&barrier->mutex);
    xrt_sys_condvar_init(&barrier->cond);
    barrier->parties = parties;
    barrier->arrived = 0;
    barrier->generation = 0;
    xrt_arc_mark_builtin(barrier, XRT_ARC_KIND_SYS_BARRIER);
    return xrt_sys_barrier_box(barrier);
}

static inline XrValue xrt_sys_once_new(void) {
    xrt_sys_once_object_t *once = (xrt_sys_once_object_t *) xrt_arc_alloc(sizeof(*once));
    xrt_sys_once_init(&once->once);
    xrt_arc_mark_builtin(once, XRT_ARC_KIND_SYS_ONCE);
    return xrt_sys_once_box(once);
}

static inline void xrt_sys_mutex_destroy_builtin(void *obj) {
    if (!obj)
        return;
    xrt_sys_mutex_object_t *mutex = (xrt_sys_mutex_object_t *) obj;
    xrt_sys_mutex_destroy(&mutex->mutex);
}

static inline void xrt_sys_rwlock_destroy_builtin(void *obj) {
    if (!obj)
        return;
    xrt_sys_rwlock_object_t *rwlock = (xrt_sys_rwlock_object_t *) obj;
    xrt_sys_rwlock_destroy(&rwlock->rwlock);
}

static inline void xrt_sys_condvar_destroy_builtin(void *obj) {
    if (!obj)
        return;
    xrt_sys_condvar_object_t *condvar = (xrt_sys_condvar_object_t *) obj;
    xrt_sys_condvar_destroy(&condvar->cond);
}

static inline void xrt_sys_barrier_destroy_builtin(void *obj) {
    if (!obj)
        return;
    xrt_sys_barrier_object_t *barrier = (xrt_sys_barrier_object_t *) obj;
    xrt_sys_condvar_destroy(&barrier->cond);
    xrt_sys_mutex_destroy(&barrier->mutex);
}

static inline void xrt_sys_once_destroy_builtin(void *obj) {
    (void) obj;
}

static inline void xrt_thread_destroy_builtin(void *obj) {
    xrt_thread_object_t *thread = (xrt_thread_object_t *) obj;
    if (!thread)
        return;
    int expected = XRT_THREAD_CREATED;
    if (atomic_compare_exchange_strong_explicit(&thread->state, &expected, XRT_THREAD_DETACHED,
                                                memory_order_acq_rel, memory_order_acquire) &&
        xr_thread_is_valid(thread->handle)) {
        xr_thread_detach(thread->handle);
    }
}

static inline XrValue xrt_thread_done_value(XrValue recv) {
    xrt_thread_object_t *thread = xrt_thread_ptr(recv);
    return XR_FROM_BOOL(thread && atomic_load_explicit(&thread->finished, memory_order_acquire));
}

static inline XrValue xrt_thread_method_0(XrValue recv, int sym) {
    xrt_thread_object_t *thread = xrt_thread_ptr(recv);
    if (!thread)
        return XR_NULL_VAL;
    if (sym == XRT_SYM_JOIN) {
        for (;;) {
            int state = atomic_load_explicit(&thread->state, memory_order_acquire);
            switch ((xrt_thread_state_t) state) {
                case XRT_THREAD_JOINED:
                    return thread->retval;
                case XRT_THREAD_DETACHED:
                    return XR_NULL_VAL;
                case XRT_THREAD_JOINING:
                    xr_thread_yield();
                    break;
                case XRT_THREAD_CREATED: {
                    int expected = XRT_THREAD_CREATED;
                    if (!atomic_compare_exchange_strong_explicit(
                            &thread->state, &expected, XRT_THREAD_JOINING, memory_order_acq_rel,
                            memory_order_acquire)) {
                        break;
                    }
                    if (xr_thread_is_valid(thread->handle))
                        (void) xr_thread_join(thread->handle, NULL);
                    atomic_store_explicit(&thread->finished, true, memory_order_release);
                    atomic_store_explicit(&thread->state, XRT_THREAD_JOINED, memory_order_release);
                    return thread->retval;
                }
            }
        }
    }
    if (sym == XRT_SYM_DETACH) {
        int expected = XRT_THREAD_CREATED;
        if (atomic_compare_exchange_strong_explicit(&thread->state, &expected, XRT_THREAD_DETACHED,
                                                    memory_order_acq_rel, memory_order_acquire) &&
            xr_thread_is_valid(thread->handle)) {
            xr_thread_detach(thread->handle);
        }
        return XR_NULL_VAL;
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_sys_mutex_method_0(XrValue recv, int sym) {
    xrt_sys_mutex_object_t *mutex = xrt_sys_mutex_ptr(recv);
    if (!mutex)
        return XR_NULL_VAL;
    if (sym == XRT_SYM_LOCK) {
        xrt_sys_mutex_lock_native(&mutex->mutex);
        return XR_NULL_VAL;
    }
    if (sym == XRT_SYM_UNLOCK) {
        xrt_sys_mutex_unlock_native(&mutex->mutex);
        return XR_NULL_VAL;
    }
    if (sym == XRT_SYM_TRYLOCK)
        return XR_FROM_BOOL(xrt_sys_mutex_trylock_native(&mutex->mutex));
    return XR_NULL_VAL;
}

static inline XrValue xrt_sys_rwlock_method_0(XrValue recv, int sym) {
    xrt_sys_rwlock_object_t *rwlock = xrt_sys_rwlock_ptr(recv);
    if (!rwlock)
        return XR_NULL_VAL;
    if (sym == XRT_SYM_RDLOCK) {
        xrt_sys_rwlock_rdlock_native(&rwlock->rwlock);
        return XR_NULL_VAL;
    }
    if (sym == XRT_SYM_RDUNLOCK) {
        xrt_sys_rwlock_rdunlock_native(&rwlock->rwlock);
        return XR_NULL_VAL;
    }
    if (sym == XRT_SYM_WRLOCK) {
        xrt_sys_rwlock_wrlock_native(&rwlock->rwlock);
        return XR_NULL_VAL;
    }
    if (sym == XRT_SYM_WRUNLOCK) {
        xrt_sys_rwlock_wrunlock_native(&rwlock->rwlock);
        return XR_NULL_VAL;
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_sys_barrier_method_0(XrValue recv, int sym) {
    xrt_sys_barrier_object_t *barrier = xrt_sys_barrier_ptr(recv);
    if (!barrier || sym != XRT_SYM_WAIT)
        return XR_NULL_VAL;

    xrt_sys_mutex_lock_native(&barrier->mutex);
    int64_t generation = barrier->generation;
    barrier->arrived++;
    if (barrier->arrived >= barrier->parties) {
        barrier->arrived = 0;
        barrier->generation++;
        xrt_sys_condvar_broadcast_native(&barrier->cond);
        xrt_sys_mutex_unlock_native(&barrier->mutex);
        return XR_TRUE_VAL;
    }
    while (generation == barrier->generation)
        xrt_sys_condvar_wait_native(&barrier->cond, &barrier->mutex);
    xrt_sys_mutex_unlock_native(&barrier->mutex);
    return XR_TRUE_VAL;
}

static inline XrValue xrt_sys_condvar_method_0(XrValue recv, int sym) {
    xrt_sys_condvar_object_t *condvar = xrt_sys_condvar_ptr(recv);
    if (!condvar)
        return XR_NULL_VAL;
    if (sym == XRT_SYM_SIGNAL) {
        xrt_sys_condvar_signal_native(&condvar->cond);
        return XR_NULL_VAL;
    }
    if (sym == XRT_SYM_BROADCAST) {
        xrt_sys_condvar_broadcast_native(&condvar->cond);
        return XR_NULL_VAL;
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_sys_condvar_method_1(XrValue recv, int sym, XrValue arg0) {
    xrt_sys_condvar_object_t *condvar = xrt_sys_condvar_ptr(recv);
    xrt_sys_mutex_object_t *mutex = xrt_sys_mutex_ptr(arg0);
    if (!condvar || !mutex)
        return XR_NULL_VAL;
    if (sym == XRT_SYM_WAIT) {
        xrt_sys_condvar_wait_native(&condvar->cond, &mutex->mutex);
        return XR_NULL_VAL;
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_sys_condvar_method_2(XrValue recv, int sym, XrValue arg0, XrValue arg1) {
    xrt_sys_condvar_object_t *condvar = xrt_sys_condvar_ptr(recv);
    xrt_sys_mutex_object_t *mutex = xrt_sys_mutex_ptr(arg0);
    if (!condvar || !mutex || sym != XRT_SYM_WAITFOR)
        return XR_NULL_VAL;
    uint64_t timeout_ns = (arg1.tag == XR_TAG_I64 && arg1.i > 0) ? (uint64_t) arg1.i : 0u;
    return XR_FROM_BOOL(
        xrt_sys_condvar_wait_for_ns_native(&condvar->cond, &mutex->mutex, timeout_ns));
}

static inline XrValue xrt_sys_once_method_1(XrValue recv, int sym, XrValue arg0) {
    xrt_sys_once_object_t *once = xrt_sys_once_ptr(recv);
    if (!once || sym != XRT_SYM_CALL || arg0.tag != XR_TAG_CLOSURE)
        return XR_NULL_VAL;

    XrValue previous = xrt_sys_once_callback;
    xrt_sys_once_callback = arg0;
    xrt_sys_once_call_native(&once->once);
    xrt_sys_once_callback = previous;
    return XR_NULL_VAL;
}

#endif /* XRT_SYS_H */
