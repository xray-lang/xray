/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xworker_handoff.c - Blocking-cfunc syscall enter/exit (P status marking)
 *
 * KEY CONCEPT:
 *   A worker M never blocks for the hot path: network I/O, channels and timers
 *   are yieldable — they suspend the coroutine through a continuation and return
 *   to the scheduler. The scheduler therefore stays non-blocking with no P/M
 *   handoff at all.
 *
 *   The only thing that can block an M is a non-yieldable SLOW cfunc (hostname
 *   getaddrinfo, synchronous file I/O, a blocking FFI call). Such a call marks
 *   its P P_SYSCALL for its duration so sysmon does not mistake the stalled
 *   heartbeat for a stuck coroutine; the M keeps ownership of its P throughout.
 *
 *   There is deliberately NO proactive P handoff (detach M, run P on a borrowed
 *   M, reclaim on return). That transition was complex and carried a
 *   lost-wakeup/orphan race under load. The correct way to keep a P busy while
 *   one operation blocks is to make that operation yieldable (e.g. async DNS via
 *   the worker pool), not to drive one P from two Ms.
 *
 * RELATED:
 *   - xproc.h: P status (advisory, sysmon only): P_RUNNING / P_SYSCALL / P_IDLE
 */
#include "xworker_internal.h"
#include "../base/xchecks.h"
#include "../os/os_time.h"

int64_t get_current_time_us(void) {
    return (int64_t) (xr_time_monotonic_ns() / 1000ULL);
}

// ========== Syscall Enter/Exit (P status marking) ==========
//
// A genuinely M-blocking C function (a non-yieldable SLOW cfunc: hostname
// getaddrinfo, synchronous file I/O, a blocking FFI call) marks its P
// P_SYSCALL across the call so sysmon does not mistake the stalled heartbeat for
// a stuck coroutine. The worker M keeps ownership of its P throughout.
//
// There is deliberately NO proactive P/M handoff. The hot path (network I/O,
// channels, timers) is yieldable — it suspends the coroutine via a continuation
// and returns to the scheduler, so an M never blocks there. The scheduler thus
// stays non-blocking without a handoff. The old handoff (detach M, run P on a
// borrowed M, reclaim on return) added a complex, racy P-ownership transition
// for the rare blocking cfunc and was the source of a lost-wakeup/orphan race
// under load. The correct way to keep a P busy while one operation blocks is to
// make that operation yieldable (e.g. async DNS via the worker pool), not to
// drive the same P from two Ms.

// Per-OS-thread syscall nesting depth: a blocking cfunc body may invoke another
// blocking helper (e.g. ws.connect -> dns_resolve). Only the outermost
// enter/exit toggles P status so a nested helper does not clear it early.
static XR_THREAD_LOCAL int tls_syscall_depth = 0;

void xr_worker_entersyscall(void) {
    XrWorker *worker = tls_current_worker;
    if (!worker)
        return;
    if (tls_syscall_depth++ > 0)
        return;
    atomic_store(&worker->p.status, P_SYSCALL);
}

void xr_worker_exitsyscall(void) {
    XrWorker *worker = tls_current_worker;
    if (!worker)
        return;
    if (tls_syscall_depth == 0)
        return;
    if (--tls_syscall_depth > 0)
        return;
    atomic_store(&worker->p.status, P_RUNNING);
}
