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

#include "../os/os_thread.h"
#include <stdatomic.h>
#include <stdbool.h>
#include "../runtime/gc/xgc_internal.h"  // XrLocalAlloc
#include "xnetpoll.h"                    // XrNetpoll
#include "xproc.h"                       // XrProc, XrRunQueue, XR_RUNQ_COUNT
#include "xmachine.h"                    // XrMachine
#include "xbalance.h"                    // XrMigrationPath
#include "xcoro_abi.h"                   // XrCoroRunResult

// Forward decl: full definition in src/io/xio_runtime.h. Coro is the
// lower layer and must not include the IO header.
struct XrIoRuntime;

/* ========== Worker Structure (P + M* pointer) ========== */

typedef struct XrWorker {
    /* === Scheduling Resources (P) === */
    XrProc p;

    /* === Bound Machine (M) === */
    XrMachine *m;
} XrWorker;

/* ========== Scheduler Diagnostics ========== */

typedef struct XrSchedGlobalStats {
    _Atomic uint64_t chan_wake_cmd_alloc_count;
    _Atomic uint64_t chan_wake_cmd_free_count;
    _Atomic uint64_t chan_wake_cmd_dispatch_count;
    _Atomic uint64_t chan_wake_cmd_drain_count;
    _Atomic uint64_t chan_wake_cmd_coalesce_count;
    _Atomic uint64_t chan_wake_cmd_stale_count;
    _Atomic uint64_t select_block_count;
    _Atomic uint64_t select_heap_alloc_count;
    _Atomic uint64_t select_inline_alloc_count;
    _Atomic uint64_t timeout_yield_retry_count;
    _Atomic uint64_t timeout_event_block_count;
    _Atomic uint64_t chan_buffer_no_waiter_count;
    _Atomic uint64_t chan_kind_spsc_count;
    _Atomic uint64_t chan_kind_mpsc_count;
    _Atomic uint64_t chan_kind_mpmc_count;
    _Atomic uint64_t inject_push_count;
    _Atomic uint64_t inject_pop_count;
    _Atomic uint64_t inject_spill_count;
    _Atomic uint64_t handoff_reuse_count;
    _Atomic uint64_t handoff_create_count;
    _Atomic uint64_t handoff_cap_hit_count;
    _Atomic uint64_t handoff_create_fail_count;
} XrSchedGlobalStats;

/* ========== Global Injection Queue ========== */

typedef struct XrInjectQueue {
    xr_mutex_t lock;
    XrCoroutine *head;
    XrCoroutine *tail;
    _Atomic int len;
} XrInjectQueue;

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
     * or XrProc::idle_link. The previous xr_mutex_t sched_lock has been
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
    _Atomic(XrProc *) idle_p_head;     // Idle P Treiber stack (via p->idle_link)
    _Atomic int idle_p_count;          // Approximate, for heuristics
    _Atomic(XrMachine *) idle_m_head;  // Idle M Treiber stack (via m->idle_link)
    _Atomic int idle_m_count;          // Approximate, for heuristics
    _Atomic int m_count;               // Total M count (grows on demand)
    int handoff_max_m;                 // Hard cap for P/M handoff threads

    /* === O(1) Idle Worker Stack (lock-free Treiber stack) === */
    _Atomic(XrMachine *) idle_worker_list;  // Head of parked-worker stack
    _Atomic int idle_worker_count;          // Approximate, for wake heuristic

    /* === State (atomic) === */
    _Atomic bool running;
    _Atomic bool threads_started;  // Worker/sysmon threads created (lazy start)
    _Atomic int started_workers;
    _Atomic int exited_workers;  // Number of workers that have fully exited
    _Atomic int active_workers;
    _Atomic int spinning_count;
    _Atomic int wake_spinner;
    _Atomic int needspinning;  // Last spinner notify protocol

    /* === Statistics === */
    _Atomic int64_t total_inbox_len;  // Global atomic counter for inbox items
    _Atomic int next_coro_id;
    bool sched_stats_enabled;
    XrSchedGlobalStats sched_stats;

    /* === Global Injection Queue === */
    XrInjectQueue injectq[XR_CORO_PRIORITY_COUNT];
    _Atomic uint32_t nonempty_inject_mask;

    /* === Scheduler Hint Bitsets === */
    _Atomic uint64_t nonempty_p_mask[XR_CORO_PRIORITY_COUNT];
    _Atomic uint64_t stealable_p_mask[XR_CORO_PRIORITY_COUNT];
    _Atomic uint64_t timer_p_mask;
    _Atomic uint64_t idle_p_mask;
    _Atomic int searching_count;

    /* === I/O & Async === */
    XrNetpoll netpoll;
    xr_thread_t sysmon_thread;  // Sysmon: heartbeat monitoring + stuck detection
    struct XrAsyncPool *async_pool;
    struct XrIoRuntime *io;  // DNS cache + future handle registry / deadline policy

    /* === Scope & Migration === */
    XrScopeContext *current_scope;
    XrMigrationPath migration_paths[XR_MAX_WORKERS];
    int64_t last_balance_time;

    /* === Load Balance State (per-Isolate) === */
    XrBalanceInfo balance_info;

    /* === Sysmon Per-Worker State === */
    struct {
        uint64_t last_heartbeat;
        int64_t stuck_since_us;
        bool warned;
    } sysmon_state[XR_MAX_WORKERS];
} XrRuntime;

static inline uint64_t xr_runtime_worker_bit(int worker_id) {
    if (worker_id < 0 || worker_id >= 64)
        return 0;
    return (uint64_t) 1ull << worker_id;
}

static inline void xr_runtime_set_mask_bit(_Atomic uint64_t *mask, int worker_id, bool enabled) {
    uint64_t bit = xr_runtime_worker_bit(worker_id);
    if (bit == 0)
        return;
    if (enabled) {
        atomic_fetch_or_explicit(mask, bit, memory_order_release);
    } else {
        atomic_fetch_and_explicit(mask, ~bit, memory_order_release);
    }
}

static inline void xr_runtime_set_runq_nonempty(XrRuntime *runtime, int worker_id, int priority,
                                                bool nonempty) {
    if (!runtime || priority < 0 || priority >= XR_CORO_PRIORITY_COUNT)
        return;
    xr_runtime_set_mask_bit(&runtime->nonempty_p_mask[priority], worker_id, nonempty);
}

static inline void xr_runtime_set_runq_stealable(XrRuntime *runtime, int worker_id, int priority,
                                                 bool stealable) {
    if (!runtime || priority < 0 || priority >= XR_CORO_PRIORITY_COUNT)
        return;
    xr_runtime_set_mask_bit(&runtime->stealable_p_mask[priority], worker_id, stealable);
}

static inline void xr_runtime_set_timer_pending(XrRuntime *runtime, int worker_id, bool pending) {
    if (!runtime)
        return;
    xr_runtime_set_mask_bit(&runtime->timer_p_mask, worker_id, pending);
}

static inline void xr_runtime_set_idle_worker_bit(XrRuntime *runtime, int worker_id, bool idle) {
    if (!runtime)
        return;
    xr_runtime_set_mask_bit(&runtime->idle_p_mask, worker_id, idle);
}

static inline bool xr_sched_stats_enabled(XrRuntime *runtime) {
    return runtime && runtime->sched_stats_enabled;
}

static inline void xr_sched_metric_inc(XrRuntime *runtime, _Atomic uint64_t *counter) {
    if (xr_sched_stats_enabled(runtime)) {
        atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
    }
}

static inline void xr_sched_metric_add(XrRuntime *runtime, _Atomic uint64_t *counter,
                                       uint64_t value) {
    if (xr_sched_stats_enabled(runtime) && value > 0) {
        atomic_fetch_add_explicit(counter, value, memory_order_relaxed);
    }
}

static inline uint64_t xr_sched_metric_load(_Atomic uint64_t *counter) {
    return atomic_load_explicit(counter, memory_order_relaxed);
}

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
XR_FUNC void xr_worker_refresh_runq_masks(XrWorker *worker);
XR_FUNC XrCoroRunResult xr_coro_run_on_worker(XrWorker *worker, XrCoroutine *coro);

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
XR_FUNC void xr_worker_dispatch_chan_wake(XrRuntime *runtime, int target_id, void *channel,
                                          bool wake_sender, bool is_close);

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

XR_FUNC void xr_worker_block_select(XrWorker *worker, XrCoroutine *coro, void **channels,
                                    int count);
XR_FUNC XrCoroutine *xr_worker_wake_select_with_status(XrWorker *worker, void *channel,
                                                       int resume_status);
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

// Reserve a new M id within the runtime handoff budget.
XR_FUNC int xr_runtime_reserve_handoff_m_id(XrRuntime *runtime);

/* ========== Diagnostics ========== */

// Print per-worker scheduling statistics to stderr
XR_FUNC void xr_runtime_print_stats(XrRuntime *runtime);

/* ========== Main Thread Entry ========== */

// Run main coroutine on calling thread using unified scheduling loop
XR_FUNC int xr_main_thread_run(XrayIsolate *X, XrCoroutine *main_coro);

/* ========== Debug Support ========== */

// Resume execution after debug break, returns when next breakpoint hit or program ends
XR_FUNC int xr_debug_resume_vm(XrayIsolate *isolate, XrCoroutine *coro);

#endif  // XWORKER_H
