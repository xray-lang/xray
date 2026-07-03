/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xthread_unix.c - POSIX implementation of xthread.h.
 *
 * Thin pthread wrapper. Most calls are 1:1; the only translation
 * is xr_cond_wait_for_ns: it converts a relative timeout into the
 * platform's timed-wait primitive using a MONOTONIC clock, so the
 * wait is immune to wall-clock (CLOCK_REALTIME) jumps.
 */

#include "../os_thread.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <string.h>
#include <unistd.h>  // sysconf

#if defined(XR_OS_MACOS)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

bool xr_thread_create(xr_thread_t *t, xr_thread_fn fn, void *arg) {
    return pthread_create(t, NULL, fn, arg) == 0;
}

bool xr_thread_create_ex(xr_thread_t *t, xr_thread_fn fn, void *arg, size_t stack_size) {
    if (stack_size == 0)
        return pthread_create(t, NULL, fn, arg) == 0;

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
        return false;
    pthread_attr_setstacksize(&attr, stack_size);
    int rc = pthread_create(t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc == 0;
}

int xr_thread_join(xr_thread_t t, void **retval) {
    return pthread_join(t, retval);
}

void xr_thread_detach(xr_thread_t t) {
    pthread_detach(t);
}

xr_thread_t xr_thread_self(void) {
    return pthread_self();
}

void xr_thread_set_name(xr_thread_t t, const char *name) {
#if defined(XR_OS_MACOS)
    // Apple's pthread_setname_np only operates on the current
    // thread; calling on another thread is a no-op.
    if (pthread_equal(t, pthread_self()))
        pthread_setname_np(name);
#elif defined(XR_OS_LINUX)
    // glibc accepts an arbitrary thread; truncates to 16 bytes.
    char buf[16];
    strncpy(buf, name, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    pthread_setname_np(t, buf);
#else
    (void) t;
    (void) name;
#endif
}

void xr_thread_yield(void) {
    sched_yield();
}

void xr_thread_sleep_ms(unsigned int ms) {
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (long) (ms % 1000) * 1000000L;
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
        // resume on the same `req` so remaining time is honored
    }
}

unsigned int xr_os_cpu_count(void) {
    static unsigned int cached = 0;
    if (cached != 0) {
        return cached;
    }
    long n = -1;
#if defined(_SC_NPROCESSORS_ONLN)
    n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
#if defined(XR_OS_MACOS)
    if (n <= 0) {
        // sysconf can return -1 on older macOS; fall back to sysctl.
        int mib[2] = {CTL_HW, HW_NCPU};
        int v = 0;
        size_t sz = sizeof(v);
        if (sysctl(mib, 2, &v, &sz, NULL, 0) == 0 && v > 0) {
            n = v;
        }
    }
#endif
    cached = (n > 0) ? (unsigned int) n : 1u;
    return cached;
}

int xr_thread_pin_to_cpu(unsigned int cpu_index) {
#if defined(XR_OS_LINUX)
    unsigned int n = xr_os_cpu_count();
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((int) (cpu_index % n), &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0 ? 0 : -1;
#else
    // macOS: thread_policy_set with THREAD_AFFINITY_POLICY exists but
    // is advisory and tied to the L2 cache topology, not CPU index.
    // Treat the call as unsupported; callers fall back to OS scheduling.
    (void) cpu_index;
    return -1;
#endif
}

// === Mutex ===

void xr_mutex_init(xr_mutex_t *m) {
    pthread_mutex_init(m, NULL);
}

void xr_mutex_destroy(xr_mutex_t *m) {
    pthread_mutex_destroy(m);
}

void xr_mutex_lock(xr_mutex_t *m) {
    pthread_mutex_lock(m);
}

void xr_mutex_unlock(xr_mutex_t *m) {
    pthread_mutex_unlock(m);
}

bool xr_mutex_trylock(xr_mutex_t *m) {
    return pthread_mutex_trylock(m) == 0;
}

// === Condition variable ===

void xr_cond_init(xr_cond_t *c) {
#if defined(XR_OS_MACOS)
    // macOS lacks pthread_condattr_setclock; xr_cond_wait_for_ns uses the
    // relative-timeout variant (pthread_cond_timedwait_relative_np) instead,
    // which is inherently monotonic and immune to wall-clock jumps.
    pthread_cond_init(c, NULL);
#else
    // Linux/BSD: bind the condvar to CLOCK_MONOTONIC so relative timeouts
    // computed in xr_cond_wait_for_ns are not skewed by NTP/manual wall-clock
    // adjustments. The deadline clock and the init clock MUST match.
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(c, &attr);
    pthread_condattr_destroy(&attr);
#endif
}

void xr_cond_destroy(xr_cond_t *c) {
    pthread_cond_destroy(c);
}

void xr_cond_wait(xr_cond_t *c, xr_mutex_t *m) {
    pthread_cond_wait(c, m);
}

bool xr_cond_wait_for_ns(xr_cond_t *c, xr_mutex_t *m, uint64_t timeout_ns) {
#if defined(XR_OS_MACOS)
    // Relative timeout is naturally monotonic; no absolute deadline math and
    // no dependency on the (unavailable) CLOCK_MONOTONIC condattr.
    struct timespec rel;
    rel.tv_sec = (time_t) (timeout_ns / 1000000000ULL);
    rel.tv_nsec = (long) (timeout_ns % 1000000000ULL);
    return pthread_cond_timedwait_relative_np(c, m, &rel) == 0;
#else
    // Deadline is interpreted against CLOCK_MONOTONIC (set in xr_cond_init),
    // so a wall-clock rollback can never stall the wait past its true timeout.
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    uint64_t total_ns = (uint64_t) deadline.tv_nsec + timeout_ns;
    deadline.tv_sec += (time_t) (total_ns / 1000000000ULL);
    deadline.tv_nsec = (long) (total_ns % 1000000000ULL);
    return pthread_cond_timedwait(c, m, &deadline) == 0;
#endif
}

void xr_cond_signal(xr_cond_t *c) {
    pthread_cond_signal(c);
}

void xr_cond_broadcast(xr_cond_t *c) {
    pthread_cond_broadcast(c);
}

// === Read-write lock ===

void xr_rwlock_init(xr_rwlock_t *l) {
    pthread_rwlock_init(l, NULL);
}

void xr_rwlock_destroy(xr_rwlock_t *l) {
    pthread_rwlock_destroy(l);
}

void xr_rwlock_rdlock(xr_rwlock_t *l) {
    pthread_rwlock_rdlock(l);
}

void xr_rwlock_wrlock(xr_rwlock_t *l) {
    pthread_rwlock_wrlock(l);
}

// POSIX has a single unlock; the rd/wr split exists only to match
// the Win32 SRWLOCK API surface.
void xr_rwlock_rdunlock(xr_rwlock_t *l) {
    pthread_rwlock_unlock(l);
}

void xr_rwlock_wrunlock(xr_rwlock_t *l) {
    pthread_rwlock_unlock(l);
}

// === Once ===

void xr_once_call(xr_once_t *o, void (*init_fn)(void)) {
    pthread_once(o, init_fn);
}
