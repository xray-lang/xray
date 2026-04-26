/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xworker.h - Multi-core runtime Worker definitions
 *
 * KEY CONCEPT:
 *   Worker = embedded XrProc (P) + pointer to XrMachine (M).
 *   P owns scheduling resources (run queues, timer wheel, etc).
 *   M provides the OS thread and VM context.
 *   P and M are structurally separate; M count grows on demand via handoff.
 *
 * RELATED MODULES:
 *   - xproc.h: XrProc (P) scheduling resource definitions
 *   - xmachine.h: XrMachine (M) OS thread definitions
 */

#ifndef XWORKER_H
#define XWORKER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include "../runtime/gc/xgc_internal.h" // XrLocalAlloc
#include "xnetpoll.h" // XrNetpoll
#include "xproc.h" // XrProc, XrRunQueue, XR_RUNQ_COUNT
#include "xmachine.h" // XrMachine
#include "xbalance.h" // XrMigrationPath

/* ========== Worker Structure (P + M* pointer) ========== */

typedef struct XrWorker {
    /* === Scheduling Resources (P) === */
    XrProc p;

    /* === Bound Machine (M) === */
    XrMachine *m;
} XrWorker;

/* ========== Runtime Structure ========== */

typedef struct XrRuntime {
    /* === Workers (P) === */
    XrWorker *workers;
    int worker_count;
    XrayIsolate *isolate;

    /* === Machines (M) — pre-allocated 1:1 with Workers === */
    XrMachine *machines;

    /* === Idle P/M Management (lock-free Treiber stacks) ===
     *
     * All three lists are lock-free stacks chained via XrMachine::idle_link
     * or XrProc::idle_link. The previous pthread_mutex_t sched_lock has been
     * removed; mutual exclusion is now achieved via atomic CAS.
     *
     * ABA: XrProc / XrMachine instances are never freed during runtime
     * lifetime (P is 1:1 with worker; M grows monotonically via handoff).
     * Re-push intervals exceed microsecond-scale CAS windows, so ABA has
     * not been observed in stress tests; a versioned tag can be added here
     * if load ever demonstrates a hazard.
     *
     * Sharing idle_link: A given M is in exactly one list at a time.
     *   - idle_worker_list : M is still bound to its parked Worker.
     *   - idle_m_head      : M has been detached via handoff
     *                        (worker->m = NULL before xr_put_idle_m).
     */
    _Atomic(XrProc *)    idle_p_head;      // Idle P Treiber stack (via p->idle_link)
    _Atomic int          idle_p_count;     // Approximate, for heuristics
    _Atomic(XrMachine *) idle_m_head;      // Idle M Treiber stack (via m->idle_link)
    _Atomic int          idle_m_count;     // Approximate, for heuristics
    _Atomic int          m_count;          // Total M count (grows on demand)

    /* === O(1) Idle Worker Stack (lock-free Treiber stack) === */
    _Atomic(XrMachine *) idle_worker_list;  // Head of parked-worker stack
    _Atomic int          idle_worker_count; // Approximate, for wake heuristic

    /* === State (atomic) === */
    _Atomic bool running;
    _Atomic bool threads_started;    // Worker/sysmon threads created (lazy start)
    _Atomic int started_workers;
    _Atomic int exited_workers;      // Number of workers that have fully exited
    _Atomic int active_workers;
    _Atomic int spinning_count;
    _Atomic int wake_spinner;
    _Atomic int needspinning;        // Last spinner notify protocol

    /* === Statistics === */
    _Atomic int64_t total_inbox_len;  // Global atomic counter for inbox items
    _Atomic int next_coro_id;

    /* === I/O & Async === */
    XrNetpoll netpoll;
    pthread_t sysmon_thread;      // Sysmon: heartbeat monitoring + stuck detection
    struct XrAsyncPool *async_pool;

    /* === Scope & Migration === */
    XrScopeContext *current_scope;
    XrMigrationPath migration_paths[XR_MAX_WORKERS];
    int64_t last_balance_time;

    /* === Load Balance State (per-Isolate) === */
    XrBalanceInfo balance_info;

    /* === Sysmon Per-Worker State === */
    struct {
        uint64_t last_heartbeat;
        int64_t  stuck_since_us;
        bool     warned;
    } sysmon_state[XR_MAX_WORKERS];
} XrRuntime;

/* ========== API ========== */

XR_FUNC XrRuntime *xr_runtime_create(XrayIsolate *isolate, int num_workers);
XR_FUNC void xr_runtime_destroy(XrRuntime *runtime);
XR_FUNC void xr_runtime_start(XrRuntime *runtime);
XR_FUNC void xr_runtime_ensure_workers(XrRuntime *runtime);
XR_FUNC void xr_runtime_stop(XrRuntime *runtime);
XR_FUNC void xr_runtime_force_stop(XrRuntime *runtime);
XR_FUNC void xr_runtime_spawn(XrRuntime *runtime, XrCoroutine *coro);
XR_FUNC void xr_runtime_spawn_local(XrWorker *worker, XrCoroutine *coro);
XR_FUNC void xr_worker_init(XrWorker *worker, int id, XrRuntime *runtime);
XR_FUNC void xr_worker_destroy(XrWorker *worker);
XR_FUNC XrCoroutine *xr_worker_pop(XrWorker *worker);
XR_FUNC void xr_worker_push(XrWorker *worker, XrCoroutine *coro);
XR_FUNC void xr_worker_push_lifo(XrWorker *worker, XrCoroutine *coro);
XR_FUNC XrVMResult xr_coro_run_on_worker(XrWorker *worker, XrCoroutine *coro);
XR_FUNC XrVMResult xr_worker_run_simple(XrWorker *worker);

/* ========== Run Queue Operations (declared in xproc.h) ========== */

// xr_runq_init, xr_runq_destroy, xr_runq_enqueue, xr_runq_dequeue,
// xr_runq_steal, xr_runq_len are now declared in xproc.h

static inline int xr_worker_total_queue_len(XrWorker *worker) {
    return xr_proc_total_queue_len(&worker->p);
}

XR_FUNC XrWorker *xr_current_worker(void);

// Enqueue coro to target worker's inbox with full synchronization.
// Handles: MPSC push + total_inbox_len increment + Dekker fence + wake if parked.
// This is the ONLY correct way to push to a remote worker's inbox.
XR_FUNC void xr_worker_inbox_enqueue(XrRuntime *runtime, int target_id, XrCoroutine *coro);

XR_FUNC void xr_worker_add_sleep_timer(XrWorker *worker, XrCoroutine *coro, int64_t delay_ms);

// Cancel timer - handles cross-worker case via async queue ()
XR_FUNC void xr_worker_cancel_timer(XrWorker *current_worker, XrCoroutine *coro);

#define XR_CORO_LOCAL_FREE_MAX 1024

XR_FUNC XrCoroutine *xr_coro_pool_get(XrRuntime *runtime);
XR_FUNC void xr_coro_pool_put(XrRuntime *runtime, XrCoroutine *coro);
XR_FUNC void xr_coro_recycle_local(XrWorker *worker, XrCoroutine *coro);

/* ========== Worker Wake ========== */

// Wake one idle worker (O(1) pop from idle stack + unpark)
XR_FUNC void xr_runtime_wake_idle_worker(XrRuntime *runtime);

// Sum per-Worker local_active_coros (replaces global atomic active_coros)
static inline int xr_runtime_active_coros(XrRuntime *runtime) {
    int total = 0;
    for (int i = 0; i < runtime->worker_count; i++) {
        total += runtime->workers[i].p.local_active_coros;
    }
    return total;
}

/* ========== Channel Wake Command Queue ========== */

// Initialize a per-worker channel wake command queue (Vyukov MPSC).
XR_FUNC void xr_chan_wake_queue_init(XrChanWakeCmdQueue *q);

// Dispatch a channel wake command to a remote worker.
// Allocates an XrChanWakeCmd, enqueues via MPSC, and wakes the target
// worker if it is parked.  Must NOT be called for the local worker.
XR_FUNC void xr_worker_dispatch_chan_wake(XrRuntime *runtime, int target_id,
                                          void *channel, bool wake_sender,
                                          bool is_close);

// Drain all pending channel wake commands on the calling worker's own
// queue and execute local wake_one / wake_select / wake_all as needed.
// Called from the owner worker's scheduling loop (worker_poll_sources).
XR_FUNC void xr_worker_drain_chan_wake_queue(XrWorker *worker);

// Destroy (free residual nodes) for shutdown.
XR_FUNC void xr_chan_wake_queue_destroy(XrChanWakeCmdQueue *q);

/* ========== Blocked Queue Operations ========== */

XR_FUNC void xr_worker_block(XrWorker *worker, XrCoroutine *coro);
XR_FUNC void xr_worker_unblock(XrWorker *worker, XrCoroutine *coro);
XR_FUNC XrCoroutine *xr_worker_wake_one(XrWorker *worker, void *channel, bool wake_sender);
XR_FUNC XrCoroutine *xr_worker_dequeue_blocked(XrWorker *worker, void *channel, bool wake_sender);
XR_FUNC void xr_worker_wake_all(XrWorker *worker, void *channel);


/* ========== Select Support ========== */

XR_FUNC void xr_worker_block_select(XrWorker *worker, XrCoroutine *coro,
                            void **channels, int count);
XR_FUNC XrCoroutine *xr_worker_wake_select(XrWorker *worker, void *channel);
XR_FUNC void xr_worker_unblock_select(XrWorker *worker, XrCoroutine *coro);
XR_FUNC int xr_runtime_next_coro_id(XrRuntime *runtime);

/* ========== Syscall Enter/Exit (P Handoff) ========== */

// Release P from current M and hand off to idle/new M.
// Called before blocking C code. P transitions: P_RUNNING → P_SYSCALL.
// A handoff M acquires P and runs its scheduling loop.
XR_FUNC void xr_worker_entersyscall(void);

// Re-acquire P after blocking C code returns.
// Signals handoff M to release P, spins until P is available.
XR_FUNC void xr_worker_exitsyscall(void);

// Thread entry for handoff M. Runs P's scheduling loop until
// original M returns (handoff_exit signal) or no work remains.
XR_FUNC void *xr_handoff_thread_entry(void *arg);

/* ========== Diagnostics ========== */

// Print per-worker scheduling statistics to stderr
XR_FUNC void xr_runtime_print_stats(XrRuntime *runtime);

/* ========== Main Thread Entry ========== */

// Run main coroutine on calling thread using unified scheduling loop
XR_FUNC int xr_main_thread_run(XrayIsolate *X, XrCoroutine *main_coro);

/* ========== Debug Support ========== */

// Resume execution after debug break, returns when next breakpoint hit or program ends
XR_FUNC int xr_debug_resume_vm(XrayIsolate *isolate, XrCoroutine *coro);

#endif // XWORKER_H
