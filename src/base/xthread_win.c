/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xthread_win.c - Windows implementation of xthread.h.
 *
 * Maps the unified xthread API onto:
 *   - CreateThread / WaitForSingleObject / CloseHandle for the
 *     thread lifecycle
 *   - SRWLOCK for both xr_mutex_t (exclusive only) and xr_rwlock_t
 *     (shared+exclusive)
 *   - CONDITION_VARIABLE + SleepConditionVariableSRW for condvars
 *   - InitOnceExecuteOnce for xr_once_call
 *
 * Thread function trampoline:
 *   The user-provided fn matches pthread's `void *(*)(void *)`,
 *   but CreateThread expects `DWORD WINAPI (*)(LPVOID)`. We heap-
 *   allocate a small ctx that owns the fn/arg/retval triple and
 *   wrap fn in xr_thread_trampoline_. xr_thread_join retrieves the
 *   user retval through the surviving ctx pointer, then frees it.
 */

#include "xthread.h"

#ifdef _WIN32

#include "xmalloc.h"
#include <process.h>

struct xr_thread_ctx {
    xr_thread_fn fn;
    void *arg;
    void *retval;
};

static DWORD WINAPI xr_thread_trampoline_(LPVOID p) {
    struct xr_thread_ctx *c = (struct xr_thread_ctx *) p;
    c->retval = c->fn(c->arg);
    return 0;
}

static bool create_with_stack(xr_thread_t *t, xr_thread_fn fn, void *arg, size_t stack_size) {
    struct xr_thread_ctx *ctx = (struct xr_thread_ctx *) xr_malloc(sizeof(*ctx));
    if (!ctx)
        return false;
    ctx->fn = fn;
    ctx->arg = arg;
    ctx->retval = NULL;

    HANDLE h = CreateThread(NULL, stack_size, xr_thread_trampoline_, ctx, 0, NULL);
    if (!h) {
        xr_free(ctx);
        return false;
    }
    t->handle = h;
    t->ctx = ctx;
    return true;
}

bool xr_thread_create(xr_thread_t *t, xr_thread_fn fn, void *arg) {
    return create_with_stack(t, fn, arg, 0);
}

bool xr_thread_create_ex(xr_thread_t *t, xr_thread_fn fn, void *arg, size_t stack_size) {
    return create_with_stack(t, fn, arg, stack_size);
}

int xr_thread_join(xr_thread_t t, void **retval) {
    if (!t.handle)
        return -1;
    WaitForSingleObject(t.handle, INFINITE);
    if (retval && t.ctx)
        *retval = t.ctx->retval;
    CloseHandle(t.handle);
    if (t.ctx)
        xr_free(t.ctx);
    return 0;
}

void xr_thread_detach(xr_thread_t t) {
    // The trampoline ctx leaks on detach because there is no later
    // join to free it. This matches pthread_detach semantics where
    // the OS reclaims thread state without an explicit join, and
    // is what callers want: the ctx is tiny (24 bytes) and detach
    // is rare.
    if (t.handle)
        CloseHandle(t.handle);
}

xr_thread_t xr_thread_self(void) {
    xr_thread_t r;
    r.handle = GetCurrentThread();
    r.ctx = NULL;
    return r;
}

void xr_thread_set_name(xr_thread_t t, const char *name) {
    // SetThreadDescription is the modern (Win10 1607+) API and
    // takes a wide string. We convert in a small stack buffer.
    if (!t.handle || !name)
        return;
    wchar_t wbuf[64];
    int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, wbuf, (int) (sizeof(wbuf) / sizeof(wbuf[0])));
    if (n <= 0)
        return;
    SetThreadDescription(t.handle, wbuf);
}

// === Mutex ===

void xr_mutex_init(xr_mutex_t *m) {
    InitializeSRWLock(m);
}

void xr_mutex_destroy(xr_mutex_t *m) {
    // SRWLOCK does not require teardown.
    (void) m;
}

void xr_mutex_lock(xr_mutex_t *m) {
    AcquireSRWLockExclusive(m);
}

void xr_mutex_unlock(xr_mutex_t *m) {
    ReleaseSRWLockExclusive(m);
}

// === Condition variable ===

void xr_cond_init(xr_cond_t *c) {
    InitializeConditionVariable(c);
}

void xr_cond_destroy(xr_cond_t *c) {
    // CONDITION_VARIABLE does not require teardown.
    (void) c;
}

void xr_cond_wait(xr_cond_t *c, xr_mutex_t *m) {
    SleepConditionVariableSRW(c, m, INFINITE, 0);
}

bool xr_cond_wait_for_ns(xr_cond_t *c, xr_mutex_t *m, uint64_t timeout_ns) {
    DWORD ms;
    if (timeout_ns >= (uint64_t) INFINITE * 1000000ULL) {
        ms = INFINITE - 1;
    } else {
        // Round up: API contract is "at least", caller expects
        // not to be woken before the deadline.
        ms = (DWORD) ((timeout_ns + 999999ULL) / 1000000ULL);
    }
    BOOL ok = SleepConditionVariableSRW(c, m, ms, 0);
    return ok ? true : false;  // FALSE => GetLastError ERROR_TIMEOUT
}

void xr_cond_signal(xr_cond_t *c) {
    WakeConditionVariable(c);
}

void xr_cond_broadcast(xr_cond_t *c) {
    WakeAllConditionVariable(c);
}

// === Read-write lock ===

void xr_rwlock_init(xr_rwlock_t *l) {
    InitializeSRWLock(l);
}

void xr_rwlock_destroy(xr_rwlock_t *l) {
    (void) l;
}

void xr_rwlock_rdlock(xr_rwlock_t *l) {
    AcquireSRWLockShared(l);
}

void xr_rwlock_wrlock(xr_rwlock_t *l) {
    AcquireSRWLockExclusive(l);
}

void xr_rwlock_rdunlock(xr_rwlock_t *l) {
    ReleaseSRWLockShared(l);
}

void xr_rwlock_wrunlock(xr_rwlock_t *l) {
    ReleaseSRWLockExclusive(l);
}

// === Once ===

// InitOnceExecuteOnce takes a callback with a different signature
// than the user's `void (*)(void)`. Trampoline through a thunk.
static void *xr_once_user_fn_;  // thread-local? no — InitOnce serialises calls.

static BOOL CALLBACK xr_once_thunk_(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void) once;
    (void) ctx;
    void (*fn)(void) = (void (*)(void)) param;
    fn();
    return TRUE;
}

void xr_once_call(xr_once_t *o, void (*init_fn)(void)) {
    InitOnceExecuteOnce(o, xr_once_thunk_, (PVOID) init_fn, NULL);
}

#endif  // _WIN32
