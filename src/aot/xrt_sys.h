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

typedef struct xrt_sys_mutex_object {
    xr_mutex_t mutex;
} xrt_sys_mutex_object_t;

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

static inline int xrt_sys_mutex_is(XrValue value) {
    return value.tag == XR_TAG_SYS_MUTEX && value.ptr != NULL;
}

static inline xrt_sys_mutex_object_t *xrt_sys_mutex_ptr(XrValue value) {
    return xrt_sys_mutex_is(value) ? (xrt_sys_mutex_object_t *) value.ptr : NULL;
}

static inline XrValue xrt_sys_mutex_box(xrt_sys_mutex_object_t *mutex) {
    return mutex ? xr_mkptr(mutex, XR_TAG_SYS_MUTEX) : XR_NULL_VAL;
}

static inline XrValue xrt_sys_mutex_new(void) {
    xrt_sys_mutex_object_t *mutex = (xrt_sys_mutex_object_t *) xrt_arc_alloc(sizeof(*mutex));
    xrt_sys_mutex_init(&mutex->mutex);
    xrt_arc_mark_builtin(mutex, XRT_ARC_KIND_SYS_MUTEX);
    return xrt_sys_mutex_box(mutex);
}

static inline void xrt_sys_mutex_destroy_builtin(void *obj) {
    if (!obj)
        return;
    xrt_sys_mutex_object_t *mutex = (xrt_sys_mutex_object_t *) obj;
    xrt_sys_mutex_destroy(&mutex->mutex);
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

#endif /* XRT_SYS_H */
