/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xworker_handoff.c - Syscall enter/exit and handoff M thread entry
 *
 * KEY CONCEPT:
 *   True P/M separation for blocking C functions:
 *     entersyscall: detach M from P, hand off P to idle/new M
 *     exitsyscall:  signal handoff M to release P, re-acquire P
 *
 *   This preserves P's cache locality (run queue, timer wheel) while
 *   allowing the blocked M's OS thread to stay blocked in the syscall.
 *
 * RELATED:
 *   - xproc.h: P status transitions (P_RUNNING -> P_SYSCALL -> P_HANDOFF)
 *   - xmachine.h: M state transitions
 *   - xworker_sched.c: worker_poll_sources (used by handoff loop)
 *   - xworker_exec.c: worker_exec_with_cont_stealing (used by handoff loop)
 */
#include "xworker_internal.h"
#include "../base/xchecks.h"
#include <sched.h>
#include <time.h>

// Get current time (microseconds)
int64_t get_current_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// ========== Syscall Enter/Exit (P Handoff) ==========

void xr_worker_entersyscall(void) {
    XrWorker *worker = tls_current_worker;
    XrMachine *m = tls_current_machine;
    if (!worker || !m)
        return;

    XrRuntime *runtime = worker->p.runtime;
    if (!runtime)
        return;

    // Single worker: just mark P_SYSCALL, no handoff possible
    if (runtime->worker_count <= 1) {
        atomic_store(&worker->p.status, P_SYSCALL);
        return;
    }

    // Remember which Worker this M belongs to (for exitsyscall)
    m->blocked_worker = worker;

    // Reset handoff exit flag
    atomic_store(&worker->p.handoff_exit, false);

    // Detach M from P
    atomic_store(&m->current_p, NULL);
    atomic_store(&worker->p.current_m, NULL);
    worker->m = NULL;
    atomic_store(&m->state, M_BLOCKED);

    // Mark P as awaiting handoff (must be after detach)
    atomic_store(&worker->p.status, P_SYSCALL);

    // Hand off P to an idle or new M
    xr_handoffp(&worker->p);
}

void xr_worker_exitsyscall(void) {
    XrMachine *m = tls_current_machine;
    if (!m)
        return;

    XrWorker *worker = m->blocked_worker;
    if (!worker) {
        // Single-worker fallback path
        worker = tls_current_worker;
        if (worker)
            atomic_store(&worker->p.status, P_RUNNING);
        return;
    }
    m->blocked_worker = NULL;

    XrProc *p = &worker->p;

    // Fast path: try CAS P_SYSCALL → P_RUNNING (handoff M hasn't acquired yet)
    uint32_t expected = P_SYSCALL;
    if (atomic_compare_exchange_strong(&p->status, &expected, P_RUNNING)) {
        // Won the race: re-bind M directly
        atomic_store(&m->current_p, p);
        atomic_store(&p->current_m, m);
        worker->m = m;
        atomic_store(&m->state, M_RUNNING);
        return;
    }

    // Slow path: handoff M is running (P_HANDOFF) or already finished (P_IDLE)
    if (expected == P_HANDOFF) {
        // Signal handoff M to release P
        atomic_store_explicit(&p->handoff_sync, 0, memory_order_relaxed);
        atomic_store(&p->handoff_exit, true);

        // Wait for handoff M to fully release P.
        // Brief spin for the common case (handoff exits within microseconds),
        // then fall back to futex wait with 1000us timeout.
        for (int spin = 0; spin < 64; spin++) {
            if (atomic_load_explicit(&p->current_m, memory_order_acquire) == NULL)
                goto handoff_done;
        }
        while (atomic_load_explicit(&p->current_m, memory_order_acquire) != NULL) {
            xr_park_futex_wait(&p->handoff_sync, 0, 1000 /* us */);
        }
    handoff_done:;
    }
    // else: P_IDLE (handoff M already finished and released)

    // Re-acquire P
    worker->m = m;
    xr_acquirep(m, p);
    atomic_store(&m->state, M_RUNNING);
}

// ========== Handoff Thread Entry ==========
//
// Runs P's scheduling loop on behalf of a blocked M.
// Exits when: handoff_exit is signaled, no work remains, or runtime stops.

void *xr_handoff_thread_entry(void *arg) {
    XrMachine *m = (XrMachine *) arg;
    XrRuntime *runtime = m->runtime;

handoff_restart:;
    XrProc *p = m->next_p;
    m->next_p = NULL;

    if (!p || !runtime) {
        goto handoff_park;
    }

    // Try CAS P_SYSCALL → P_HANDOFF (original M may have already returned)
    uint32_t expected = P_SYSCALL;
    if (!atomic_compare_exchange_strong(&p->status, &expected, P_HANDOFF)) {
        // Original M already reclaimed P — nothing to do
        goto handoff_park;
    }

    // Find Worker containing this P
    XrWorker *worker = NULL;
    for (int i = 0; i < runtime->worker_count; i++) {
        if (&runtime->workers[i].p == p) {
            worker = &runtime->workers[i];
            break;
        }
    }
    if (!worker) {
        atomic_store(&p->status, P_IDLE);
        goto handoff_park;
    }

    // Bind M to Worker and P
    worker->m = m;
    atomic_store(&m->current_p, p);
    atomic_store(&p->current_m, m);
    atomic_store(&m->state, M_RUNNING);
    tls_current_worker = worker;
    tls_current_machine = m;

    // ===== Handoff Scheduling Loop =====
    {
        int idle_iterations = 0;

        while (atomic_load(&runtime->running)) {
            if (atomic_load(&p->handoff_exit))
                break;

            XrCoroutine *fast_io = worker_poll_sources(worker);

            XrCoroutine *coro = fast_io ? fast_io : xr_worker_pop(worker);

            if (!coro) {
                if (++idle_iterations > 100)
                    break;
                struct timespec ts = {0, 100000};
                nanosleep(&ts, NULL);
                continue;
            }
            idle_iterations = 0;

            if (atomic_load(&p->handoff_exit)) {
                xr_worker_push(worker, coro);
                break;
            }

            if (xr_coro_flags_has(coro, XR_CORO_FLG_DONE))
                continue;

            worker_exec_with_cont_stealing(worker, coro);
        }
    }

    // Release P and unbind.
    // handoff_sync wake must follow current_m = NULL so the exitsyscall
    // waiter sees the NULL when it re-checks after futex return.
    atomic_store(&m->current_p, NULL);
    atomic_store_explicit(&p->current_m, NULL, memory_order_release);
    atomic_store_explicit(&p->handoff_sync, 1, memory_order_release);
    xr_park_futex_wake(&p->handoff_sync);
    atomic_store(&p->status, P_IDLE);
    worker->m = NULL;
    m->current_coro = NULL;
    atomic_store(&m->state, M_PARKED);
    tls_current_worker = NULL;
    tls_current_machine = NULL;

handoff_park:
    // Park this M into idle pool, keeping thread alive for reuse
    atomic_store(&m->has_thread, true);
    xr_put_idle_m(runtime, m);

    // Wait for next_p to be set (thread reuse instead of exit+create)
    while (!m->next_p && atomic_load(&runtime->running)) {
        atomic_store_explicit(&m->park_state, XR_PARK_IDLE, memory_order_release);
        xr_park_futex_wait(&m->park_state, XR_PARK_IDLE, 0);
    }

    // Shutdown check
    if (!atomic_load(&runtime->running)) {
        atomic_store(&m->has_thread, false);
        return NULL;
    }

    // Reuse: jump back to process next_p
    goto handoff_restart;
}
