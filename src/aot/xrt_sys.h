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
#if !defined(XR_OS_WINDOWS)
#include <time.h>
#endif

typedef struct xrt_sys_mutex_object {
    xr_mutex_t mutex;
} xrt_sys_mutex_object_t;

typedef struct xrt_sys_rwlock_object {
    xr_rwlock_t rwlock;
} xrt_sys_rwlock_object_t;

typedef struct xrt_sys_condvar_object {
    xr_cond_t cond;
} xrt_sys_condvar_object_t;

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

static inline int xrt_sys_mutex_is(XrValue value) {
    return value.tag == XR_TAG_SYS_MUTEX && value.ptr != NULL;
}

static inline int xrt_sys_rwlock_is(XrValue value) {
    return value.tag == XR_TAG_SYS_RWLOCK && value.ptr != NULL;
}

static inline int xrt_sys_condvar_is(XrValue value) {
    return value.tag == XR_TAG_SYS_CONDVAR && value.ptr != NULL;
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

static inline XrValue xrt_sys_mutex_box(xrt_sys_mutex_object_t *mutex) {
    return mutex ? xr_mkptr(mutex, XR_TAG_SYS_MUTEX) : XR_NULL_VAL;
}

static inline XrValue xrt_sys_rwlock_box(xrt_sys_rwlock_object_t *rwlock) {
    return rwlock ? xr_mkptr(rwlock, XR_TAG_SYS_RWLOCK) : XR_NULL_VAL;
}

static inline XrValue xrt_sys_condvar_box(xrt_sys_condvar_object_t *condvar) {
    return condvar ? xr_mkptr(condvar, XR_TAG_SYS_CONDVAR) : XR_NULL_VAL;
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

#endif /* XRT_SYS_H */
