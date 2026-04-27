/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xthread.h - Cross-platform threads, mutexes, condvars, rwlocks, once.
 *
 * KEY CONCEPT:
 *   Single threading API for the runtime. Implementations are split:
 *
 *     POSIX:   pthread_*  (pthread.h must be installed for the
 *              build host; macOS / Linux ship it natively)
 *     Windows: Win32 SRWLOCK / CONDITION_VARIABLE / INIT_ONCE,
 *              CreateThread for thread spawning
 *
 *   Type aliases below are macro-typedef'd to the platform-native
 *   handle so static initializers (XR_MUTEX_INITIALIZER etc.)
 *   compile to the underlying constant. There is no extra wrapper
 *   struct: zero-overhead vs raw pthread/Win32, identical layout.
 *
 *   Mutex semantics: non-recursive. Same as PTHREAD_MUTEX_DEFAULT
 *   and the SRWLOCK exclusive mode. Code that needs recursion
 *   should not be using xr_mutex; recursion is almost always a
 *   sign of a layering bug.
 *
 *   RWLock unlock: Win32 SRWLOCK has separate Release{Shared,
 *   Exclusive} calls and you cannot ask "is this held shared or
 *   exclusive". The pthread API hides that detail behind a single
 *   pthread_rwlock_unlock. To keep semantics portable without
 *   tracking state, this header exposes BOTH options:
 *     - xr_rwlock_rdunlock  : symmetric to xr_rwlock_rdlock
 *     - xr_rwlock_wrunlock  : symmetric to xr_rwlock_wrlock
 *   Callers must call the matching unlock; this is enforceable in
 *   review and matches Win32 semantics natively.
 *
 *   xr_thread_t is a struct on Windows (HANDLE + ctx pointer for
 *   join-time return-value retrieval) and a bare pthread_t on
 *   POSIX. Thread function signature is unified to
 *   `void *(*)(void *)` matching pthread; the Windows impl wraps
 *   the user fn in a CreateThread-compatible trampoline.
 *
 * RELATED MODULES:
 *   - base/xdefs.h   for visibility macros
 *   - base/xtime.h   for the cond-wait timeout helpers
 */

#ifndef XTHREAD_H
#define XTHREAD_H

#include "xdefs.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// CRITICAL_SECTION-vs-SRWLOCK choice: SRWLOCK is the modern
// (Vista+) primitive that's strictly cheaper for the
// non-recursive case and natively supports a reader/writer mode.
// Same primitive backs both xr_mutex_t (used as exclusive) and
// xr_rwlock_t (used in shared+exclusive mode).
typedef SRWLOCK xr_mutex_t;
typedef SRWLOCK xr_rwlock_t;
typedef CONDITION_VARIABLE xr_cond_t;
typedef INIT_ONCE xr_once_t;

#define XR_MUTEX_INITIALIZER SRWLOCK_INIT
#define XR_RWLOCK_INITIALIZER SRWLOCK_INIT
#define XR_COND_INITIALIZER CONDITION_VARIABLE_INIT
#define XR_ONCE_INITIALIZER INIT_ONCE_STATIC_INIT

// xr_thread_t holds the OS handle plus a heap-allocated ctx for
// the user fn / arg / retval triple. xr_thread_join pulls the
// retval back out of ctx.
typedef struct xr_thread_ctx xr_thread_ctx;
typedef struct {
    HANDLE handle;
    xr_thread_ctx *ctx;
} xr_thread_t;

#else  // POSIX

#include <pthread.h>

typedef pthread_t xr_thread_t;
typedef pthread_mutex_t xr_mutex_t;
typedef pthread_cond_t xr_cond_t;
typedef pthread_rwlock_t xr_rwlock_t;
typedef pthread_once_t xr_once_t;

#define XR_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#define XR_RWLOCK_INITIALIZER PTHREAD_RWLOCK_INITIALIZER
#define XR_COND_INITIALIZER PTHREAD_COND_INITIALIZER
#define XR_ONCE_INITIALIZER PTHREAD_ONCE_INIT

#endif  // _WIN32

#ifdef __cplusplus
extern "C" {
#endif

// User thread entry point. Mirrors pthread; the void* return is
// retrievable via xr_thread_join's retval out-parameter on every
// platform.
typedef void *(*xr_thread_fn)(void *arg);

// === Thread lifecycle ===

// Create and start a thread. Returns true on success.
XR_FUNC bool xr_thread_create(xr_thread_t *t, xr_thread_fn fn, void *arg);

// Same as xr_thread_create with an explicit stack size in bytes.
// Pass 0 to use the platform default. Implementations round up to
// the platform-specific minimum.
XR_FUNC bool xr_thread_create_ex(xr_thread_t *t, xr_thread_fn fn, void *arg, size_t stack_size);

// Block until `t` exits and reap its resources. If `retval` is
// non-NULL, the thread's return pointer is written there.
XR_FUNC int xr_thread_join(xr_thread_t t, void **retval);

// Mark `t` so its resources are released automatically when it
// exits. After this call the handle must not be joined.
XR_FUNC void xr_thread_detach(xr_thread_t t);

// Return the calling thread's handle.
XR_FUNC xr_thread_t xr_thread_self(void);

// Test whether a thread handle is non-zero, i.e. has been
// populated by a successful xr_thread_create*. Lets callers
// keep arrays of thread handles in heap-allocated zero-init
// memory and detect "this slot was never used" without
// platform-specific struct accesses.
static inline bool xr_thread_is_valid(xr_thread_t t) {
#ifdef _WIN32
    return t.handle != NULL;
#else
    // pthread_t is a scalar on every supported POSIX platform;
    // a zero-initialised slot compares equal to (pthread_t)0
    // and never matches a real, joinable thread.
    return t != (xr_thread_t) 0;
#endif
}

// Best-effort thread name for debuggers / profilers. Silent
// failure on platforms that do not support naming the calling
// thread out-of-band.
XR_FUNC void xr_thread_set_name(xr_thread_t t, const char *name);

// Hint the scheduler to run another runnable thread of equal
// priority. Wraps POSIX sched_yield / Win32 SwitchToThread so
// callers don't have to pull in <sched.h> directly (which is
// not present on MSVC).
XR_FUNC void xr_thread_yield(void);

// Sleep the calling thread for at least `ms` milliseconds.
// Wraps POSIX nanosleep / Win32 Sleep. Coarse: callers that
// need sub-ms precision should use the runtime's timer wheel.
XR_FUNC void xr_thread_sleep_ms(unsigned int ms);

// === Mutex ===

XR_FUNC void xr_mutex_init(xr_mutex_t *m);
XR_FUNC void xr_mutex_destroy(xr_mutex_t *m);
XR_FUNC void xr_mutex_lock(xr_mutex_t *m);
XR_FUNC void xr_mutex_unlock(xr_mutex_t *m);

// === Condition variable ===

XR_FUNC void xr_cond_init(xr_cond_t *c);
XR_FUNC void xr_cond_destroy(xr_cond_t *c);
XR_FUNC void xr_cond_wait(xr_cond_t *c, xr_mutex_t *m);

// Bounded wait. Returns true if the condvar was signalled before
// `timeout_ns` elapsed, false on timeout. The mutex `m` is
// re-acquired before returning either way (matches pthread).
XR_FUNC bool xr_cond_wait_for_ns(xr_cond_t *c, xr_mutex_t *m, uint64_t timeout_ns);

XR_FUNC void xr_cond_signal(xr_cond_t *c);
XR_FUNC void xr_cond_broadcast(xr_cond_t *c);

// === Read-write lock ===

XR_FUNC void xr_rwlock_init(xr_rwlock_t *l);
XR_FUNC void xr_rwlock_destroy(xr_rwlock_t *l);
XR_FUNC void xr_rwlock_rdlock(xr_rwlock_t *l);
XR_FUNC void xr_rwlock_wrlock(xr_rwlock_t *l);

// Symmetric unlock pair. Call rdunlock after rdlock, wrunlock
// after wrlock. Win32 SRWLOCK requires the distinction natively;
// the POSIX impl maps both to pthread_rwlock_unlock.
XR_FUNC void xr_rwlock_rdunlock(xr_rwlock_t *l);
XR_FUNC void xr_rwlock_wrunlock(xr_rwlock_t *l);

// === Once ===

XR_FUNC void xr_once_call(xr_once_t *o, void (*init_fn)(void));

#ifdef __cplusplus
}
#endif

#endif  // XTHREAD_H
