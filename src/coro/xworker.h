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
 *   M provides the OS thread and backend-local scratch storage.
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
#include <stddef.h>
#include "../runtime/gc/xgc_internal.h"  // XrLocalAlloc
#include "../runtime/gc/xsystem_heap.h"  // XrSystemHeap
#include "../runtime/core/xr_runtime_core.h"
#include "xnetpoll.h"   // XrNetpoll
#include "xproc.h"      // XrProc, XrRunQueue
#include "xmachine.h"   // XrMachine
#include "xbalance.h"   // XrMigrationPath
#include "xcoro_abi.h"  // XrCoroRunResult

// Forward decl: full definition in src/io/xio_runtime.h. Coro is the
// lower layer and must not include the IO header.
struct XrIoRuntime;
struct XrTask;

typedef struct XrSchedulerHostOps {
    void *(*backend_context)(void *ctx);
    void (*notify_coro)(void *ctx, XrCoroutine *coro, const char *reason);
    void (*coro_on_exit)(void *ctx, XrCoroutine *coro);
    void (*wake_scope_waiter)(void *ctx, XrCoroutine *coro);
    void (*wake_coro_waiter)(void *ctx, XrCoroutine *coro);
    void (*wake_task_waiter)(void *ctx, struct XrTask *task);
    void (*adopt_deferred_tasks)(void *ctx, struct XrTask *tasks, size_t count);
} XrSchedulerHostOps;

typedef struct XrSchedulerHost {
    const XrSchedulerHostOps *ops;
    void *ctx;
} XrSchedulerHost;

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
    _Atomic uint64_t chan_wake_cmd_forward_count;
    _Atomic uint64_t select_block_count;
    _Atomic uint64_t select_register_count;
    _Atomic uint64_t select_unregister_count;
    _Atomic uint64_t select_wake_count;
    _Atomic uint64_t select_close_wake_count;
    _Atomic uint64_t select_heap_alloc_count;
    _Atomic uint64_t select_inline_alloc_count;
    _Atomic uint64_t vm_select_probe_hit_count;
    _Atomic uint64_t timeout_yield_retry_count;
    _Atomic uint64_t timeout_event_block_count;
    _Atomic uint64_t timer_fire_count;
    _Atomic uint64_t timer_cancel_local_count;
    _Atomic uint64_t timer_cancel_remote_count;
    _Atomic uint64_t timer_cancel_process_count;
    _Atomic uint64_t timer_cancel_duplicate_count;
    _Atomic uint64_t timer_cancel_drain_batch_count;
    _Atomic uint64_t timer_cancel_drain_node_count;
    _Atomic uint64_t timer_cancel_drain_max_count;
    _Atomic uint64_t chan_send_direct_count;
    _Atomic uint64_t chan_recv_direct_count;
    _Atomic uint64_t chan_send_buffer_count;
    _Atomic uint64_t chan_recv_buffer_count;
    _Atomic uint64_t chan_send_block_count;
    _Atomic uint64_t chan_recv_block_count;
    _Atomic uint64_t chan_sendq_dequeue_count;
    _Atomic uint64_t chan_recvq_dequeue_count;
    _Atomic uint64_t chan_ready_wake_count;
    _Atomic uint64_t chan_send_no_copy_count;
    _Atomic uint64_t chan_send_deep_copy_count;
    _Atomic uint64_t chan_recv_no_copy_count;
    _Atomic uint64_t chan_recv_deep_copy_count;
    _Atomic uint64_t chan_close_ready_wake_count;
    _Atomic uint64_t chan_close_send_waiter_count;
    _Atomic uint64_t chan_close_recv_waiter_count;
    _Atomic uint64_t chan_close_deferred_send_waiter_count;
    _Atomic uint64_t chan_close_deferred_recv_waiter_count;
    _Atomic uint64_t chan_close_local_worker_count;
    _Atomic uint64_t chan_close_remote_worker_count;
    _Atomic uint64_t chan_buffer_no_waiter_count;
    _Atomic uint64_t chan_lock_fast_count;
    _Atomic uint64_t chan_lock_contended_count;
    _Atomic uint64_t chan_lock_slow_count;
    _Atomic uint64_t chan_lock_recheck_count;
    _Atomic uint64_t chan_lock_spin_count;
    _Atomic uint64_t chan_lock_yield_count;
    _Atomic uint64_t chan_lock_sleep_count;
    _Atomic uint64_t chan_buffer_fast_try_count;
    _Atomic uint64_t chan_buffer_fast_hit_count;
    _Atomic uint64_t chan_buffer_fast_miss_count;
    _Atomic uint64_t chan_buffer_fast_busy_count;
    _Atomic uint64_t chan_kind_spsc_count;
    _Atomic uint64_t chan_kind_mpsc_count;
    _Atomic uint64_t chan_kind_work_queue_count;
    _Atomic uint64_t chan_kind_mpmc_count;
    _Atomic uint64_t chan_worker_kind_spsc_count;
    _Atomic uint64_t chan_worker_kind_mpsc_count;
    _Atomic uint64_t chan_worker_kind_work_queue_count;
    _Atomic uint64_t chan_worker_kind_mpmc_count;
    _Atomic uint64_t chan_kind_generic_op_count;
    _Atomic uint64_t chan_kind_spsc_op_count;
    _Atomic uint64_t chan_kind_mpsc_op_count;
    _Atomic uint64_t chan_kind_work_queue_op_count;
    _Atomic uint64_t chan_kind_mpmc_op_count;
    _Atomic uint64_t chan_kind_rendezvous_op_count;
    _Atomic uint64_t chan_kind_generic_send_op_count;
    _Atomic uint64_t chan_kind_rendezvous_send_op_count;
    _Atomic uint64_t chan_kind_spsc_send_op_count;
    _Atomic uint64_t chan_kind_mpsc_send_op_count;
    _Atomic uint64_t chan_kind_work_queue_send_op_count;
    _Atomic uint64_t chan_kind_mpmc_send_op_count;
    _Atomic uint64_t chan_kind_generic_recv_op_count;
    _Atomic uint64_t chan_kind_rendezvous_recv_op_count;
    _Atomic uint64_t chan_kind_spsc_recv_op_count;
    _Atomic uint64_t chan_kind_mpsc_recv_op_count;
    _Atomic uint64_t chan_kind_work_queue_recv_op_count;
    _Atomic uint64_t chan_kind_mpmc_recv_op_count;
    _Atomic uint64_t chan_kind_generic_block_send_waiter_count;
    _Atomic uint64_t chan_kind_rendezvous_block_send_waiter_count;
    _Atomic uint64_t chan_kind_spsc_block_send_waiter_count;
    _Atomic uint64_t chan_kind_mpsc_block_send_waiter_count;
    _Atomic uint64_t chan_kind_work_queue_block_send_waiter_count;
    _Atomic uint64_t chan_kind_mpmc_block_send_waiter_count;
    _Atomic uint64_t chan_kind_generic_block_recv_waiter_count;
    _Atomic uint64_t chan_kind_rendezvous_block_recv_waiter_count;
    _Atomic uint64_t chan_kind_spsc_block_recv_waiter_count;
    _Atomic uint64_t chan_kind_mpsc_block_recv_waiter_count;
    _Atomic uint64_t chan_kind_work_queue_block_recv_waiter_count;
    _Atomic uint64_t chan_kind_mpmc_block_recv_waiter_count;
    _Atomic uint64_t chan_kind_generic_wake_send_waiter_count;
    _Atomic uint64_t chan_kind_rendezvous_wake_send_waiter_count;
    _Atomic uint64_t chan_kind_spsc_wake_send_waiter_count;
    _Atomic uint64_t chan_kind_mpsc_wake_send_waiter_count;
    _Atomic uint64_t chan_kind_work_queue_wake_send_waiter_count;
    _Atomic uint64_t chan_kind_mpmc_wake_send_waiter_count;
    _Atomic uint64_t chan_kind_generic_wake_recv_waiter_count;
    _Atomic uint64_t chan_kind_rendezvous_wake_recv_waiter_count;
    _Atomic uint64_t chan_kind_spsc_wake_recv_waiter_count;
    _Atomic uint64_t chan_kind_mpsc_wake_recv_waiter_count;
    _Atomic uint64_t chan_kind_work_queue_wake_recv_waiter_count;
    _Atomic uint64_t chan_kind_mpmc_wake_recv_waiter_count;
    _Atomic uint64_t chan_kind_generic_retarget_send_waiter_count;
    _Atomic uint64_t chan_kind_rendezvous_retarget_send_waiter_count;
    _Atomic uint64_t chan_kind_spsc_retarget_send_waiter_count;
    _Atomic uint64_t chan_kind_mpsc_retarget_send_waiter_count;
    _Atomic uint64_t chan_kind_work_queue_retarget_send_waiter_count;
    _Atomic uint64_t chan_kind_mpmc_retarget_send_waiter_count;
    _Atomic uint64_t chan_kind_generic_retarget_recv_waiter_count;
    _Atomic uint64_t chan_kind_rendezvous_retarget_recv_waiter_count;
    _Atomic uint64_t chan_kind_spsc_retarget_recv_waiter_count;
    _Atomic uint64_t chan_kind_mpsc_retarget_recv_waiter_count;
    _Atomic uint64_t chan_kind_work_queue_retarget_recv_waiter_count;
    _Atomic uint64_t chan_kind_mpmc_retarget_recv_waiter_count;
    _Atomic uint64_t vm_chan_send_fast_no_ext_count;
    _Atomic uint64_t vm_chan_recv_fast_no_ext_count;
    _Atomic uint64_t vm_await_done_fast_count;
    _Atomic uint64_t chan_worker_kind_generic_op_count;
    _Atomic uint64_t chan_worker_kind_spsc_op_count;
    _Atomic uint64_t chan_worker_kind_mpsc_op_count;
    _Atomic uint64_t chan_worker_kind_work_queue_op_count;
    _Atomic uint64_t chan_worker_kind_mpmc_op_count;
    _Atomic uint64_t chan_worker_kind_rendezvous_op_count;
    _Atomic uint64_t task_one_shot_await_count;
    _Atomic uint64_t task_one_shot_destroy_attempt_count;
    _Atomic uint64_t task_one_shot_destroy_success_count;
    _Atomic uint64_t task_one_shot_destroy_fail_state_count;
    _Atomic uint64_t task_one_shot_destroy_fail_coro_count;
    _Atomic uint64_t task_one_shot_destroy_fail_graph_count;
    _Atomic uint64_t task_one_shot_destroy_fail_listener_count;
    _Atomic uint64_t task_one_shot_destroy_fail_waiter_count;
    _Atomic uint64_t task_one_shot_destroy_fail_unlinked_count;
    _Atomic uint64_t inject_push_count;
    _Atomic uint64_t inject_pop_count;
    _Atomic uint64_t inject_spill_count;
    _Atomic uint64_t inject_push_batch_count;
    _Atomic uint64_t inject_pop_batch_count;
    _Atomic uint64_t work_queue_push_count;
    _Atomic uint64_t work_queue_pop_local_count;
    _Atomic uint64_t work_queue_pop_steal_count;
    _Atomic uint64_t work_queue_pop_empty_count;
    _Atomic uint64_t work_queue_block_count;
    _Atomic uint64_t work_queue_wake_count;
    _Atomic uint64_t work_queue_close_count;
    _Atomic uint64_t work_queue_close_wake_count;
    _Atomic uint64_t result_group_add_count;
    _Atomic uint64_t result_group_flush_count;
    _Atomic uint64_t result_group_flush_item_count;
    _Atomic uint64_t result_group_recv_count;
    _Atomic uint64_t result_group_recv_empty_count;
    _Atomic uint64_t result_group_block_count;
    _Atomic uint64_t result_group_wake_count;
    _Atomic uint64_t result_group_close_count;
    _Atomic uint64_t result_group_close_wake_count;
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
    /* === VM-neutral Runtime Core === */
    XrRuntimeCore *core;

    /* === Workers (P) === */
    XrWorker *workers;
    int worker_count;

    /* Host bridge for VM/AOT backend callbacks. Scheduler-owned resources must use core. */
    XrSchedulerHost host;

    /* === Machines (M) — pre-allocated 1:1 with Workers === */
    XrMachine *machines;

    /* === Deterministic Test Mode ===
     *
     * XRAY_CORO_DETERMINISTIC=1 forces a single-worker runtime, fixes the
     * worker PRNG seed, and lets the scheduler advance this virtual millisecond
     * clock to the next timer deadline instead of sleeping on wall time. */
    bool deterministic_sched;
    uint32_t deterministic_seed;
    _Atomic int64_t virtual_time_ms;

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
    _Atomic bool threads_started;  // Lazy worker startup completed for this run
    _Atomic bool sysmon_started;   // Sysmon thread was created and must be joined
    _Atomic int started_workers;
    _Atomic int exited_workers;  // Number of workers that have fully exited
    _Atomic int active_workers;
    _Atomic int spinning_count;
    _Atomic int wake_spinner;
    _Atomic int needspinning;  // Last spinner notify protocol

    /* === Statistics === */
    _Atomic int next_coro_id;
    bool sched_stats_enabled;
    XrSchedGlobalStats sched_stats;

    /* === Runtime-owned Task handles === */
    xr_mutex_t task_lock;
    struct XrTask *task_list;
    size_t task_count;

    /* === Global Injection Queue === */
    XrInjectQueue injectq;
    _Atomic bool injectq_nonempty;

    /* === Scheduler Hint Bitsets ===
     *
     * Work discovery deliberately has NO per-enqueue global state: pushing or
     * popping a coroutine touches only per-P memory (Go runqput/runqget
     * semantics). Idle workers DISCOVER work by scanning per-P queue sizes
     * and inbox heads — a bounded read-only loop on the cold path — instead
     * of every hot-path enqueue paying a shared-cacheline RMW.
     * Only park bookkeeping (idle_p_mask) and timer presence (timer_p_mask)
     * keep eager bits; both are cold-path writers. */
    _Atomic uint64_t timer_p_mask;
    _Atomic uint64_t idle_p_mask;
    _Atomic int searching_count;

    /* === I/O & Async === */
    XrNetpoll netpoll;
    xr_thread_t sysmon_thread;  // Sysmon: heartbeat monitoring + stuck detection
    /* Watchdog: a coroutine whose heartbeat is frozen this many microseconds
     * while RUNNING is force-cancelled by sysmon (safety net for code that
     * never reaches a safepoint). Default XR_SYSMON_CANCEL_US; overridable via
     * XRAY_SYSMON_CANCEL_MS. <= 0 disables forced cancel (warn-only). */
    int64_t sysmon_cancel_us;
    struct XrAsyncPool *async_pool;
    struct XrIoRuntime *io;  // DNS cache + future handle registry / deadline policy

    /* === Scope & Migration === */
    XrScopeContext *current_scope;
    XrMigrationPath migration_paths[XR_MAX_WORKERS];
    int64_t last_balance_time;

    /* === Load Balance State (per-runtime) === */
    XrBalanceInfo balance_info;

    /* === Sysmon Per-Worker State === */
    struct {
        uint64_t last_heartbeat;
        int64_t stuck_since_us;
        bool warned;
    } sysmon_state[XR_MAX_WORKERS];
} XrRuntime;

typedef XrRuntime XrSchedulerRuntime;

static inline XrRuntimeCore *xr_runtime_get_core(const XrRuntime *runtime) {
    return runtime ? runtime->core : NULL;
}

static inline XrSystemHeap *xr_runtime_get_sys_heap(const XrRuntime *runtime) {
    XrRuntimeCore *core = xr_runtime_get_core(runtime);
    return core ? core->sys_heap : NULL;
}

static inline struct XrCoroStructPool *xr_runtime_get_coro_pool(const XrRuntime *runtime) {
    XrSystemHeap *heap = xr_runtime_get_sys_heap(runtime);
    return heap ? heap->coro_pool : NULL;
}

static inline void *xr_scheduler_host_backend_context(const XrRuntime *runtime) {
    if (!runtime || !runtime->host.ops || !runtime->host.ops->backend_context)
        return NULL;
    return runtime->host.ops->backend_context(runtime->host.ctx);
}

static inline void xr_scheduler_host_notify_coro(XrRuntime *runtime, XrCoroutine *coro,
                                                 const char *reason) {
    if (runtime && runtime->host.ops && runtime->host.ops->notify_coro)
        runtime->host.ops->notify_coro(runtime->host.ctx, coro, reason);
}

static inline void xr_scheduler_host_coro_on_exit(XrRuntime *runtime, XrCoroutine *coro) {
    if (runtime && runtime->host.ops && runtime->host.ops->coro_on_exit)
        runtime->host.ops->coro_on_exit(runtime->host.ctx, coro);
}

static inline void xr_scheduler_host_wake_scope_waiter(XrRuntime *runtime, XrCoroutine *coro) {
    if (runtime && runtime->host.ops && runtime->host.ops->wake_scope_waiter)
        runtime->host.ops->wake_scope_waiter(runtime->host.ctx, coro);
}

static inline void xr_scheduler_host_wake_coro_waiter(XrRuntime *runtime, XrCoroutine *coro) {
    if (runtime && runtime->host.ops && runtime->host.ops->wake_coro_waiter)
        runtime->host.ops->wake_coro_waiter(runtime->host.ctx, coro);
}

XR_FUNC void xr_task_wake_waiter_runtime(XrRuntime *runtime, struct XrTask *task);

static inline void xr_scheduler_host_wake_task_waiter(XrRuntime *runtime, struct XrTask *task) {
    if (runtime && runtime->host.ops && runtime->host.ops->wake_task_waiter) {
        runtime->host.ops->wake_task_waiter(runtime->host.ctx, task);
        return;
    }
    xr_task_wake_waiter_runtime(runtime, task);
}

static inline bool xr_scheduler_host_adopt_deferred_tasks(XrRuntime *runtime, struct XrTask *tasks,
                                                          size_t count) {
    if (runtime && runtime->host.ops && runtime->host.ops->adopt_deferred_tasks) {
        runtime->host.ops->adopt_deferred_tasks(runtime->host.ctx, tasks, count);
        return true;
    }
    (void) count;
    return false;
}

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

static inline void xr_sched_metric_max(XrRuntime *runtime, _Atomic uint64_t *counter,
                                       uint64_t value) {
    if (!xr_sched_stats_enabled(runtime))
        return;
    uint64_t current = atomic_load_explicit(counter, memory_order_relaxed);
    while (current < value &&
           !atomic_compare_exchange_weak_explicit(counter, &current, value, memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
}

static inline uint64_t xr_sched_metric_load(_Atomic uint64_t *counter) {
    return atomic_load_explicit(counter, memory_order_relaxed);
}

/* ========== API ========== */

XR_FUNC XrSchedulerRuntime *xr_scheduler_runtime_new(XrRuntimeCore *core, int num_workers);
XR_FUNC void xr_scheduler_runtime_attach_host(XrSchedulerRuntime *runtime,
                                              const XrSchedulerHost *host);
XR_FUNC void xr_scheduler_runtime_clear_host(XrSchedulerRuntime *runtime);
XR_FUNC void xr_scheduler_runtime_attach_isolate(XrSchedulerRuntime *runtime, XrayIsolate *isolate);
XR_FUNC void xr_scheduler_runtime_delete(XrSchedulerRuntime *runtime);
XR_FUNC void xr_runtime_start(XrRuntime *runtime);
XR_FUNC void xr_runtime_ensure_workers(XrRuntime *runtime);
XR_FUNC void xr_runtime_stop(XrRuntime *runtime);
XR_FUNC void xr_runtime_force_stop(XrRuntime *runtime);
XR_FUNC bool xr_runtime_deterministic_mode(const XrRuntime *runtime);
XR_FUNC int64_t xr_runtime_now_ticks(XrRuntime *runtime);
XR_FUNC void xr_runtime_advance_virtual_time(XrRuntime *runtime, int64_t target_ticks);
XR_FUNC int64_t xr_runtime_current_monotonic_ms(void);
XR_FUNC int64_t xr_runtime_current_monotonic_ns(void);
XR_FUNC void xr_runtime_spawn(XrRuntime *runtime, XrCoroutine *coro);
XR_FUNC void xr_runtime_spawn_local(XrWorker *worker, XrCoroutine *coro);
XR_FUNC void xr_worker_init(XrWorker *worker, int id, XrRuntime *runtime);
XR_FUNC void xr_worker_destroy(XrWorker *worker);
XR_FUNC XrCoroutine *xr_worker_pop(XrWorker *worker);
XR_FUNC void xr_worker_push(XrWorker *worker, XrCoroutine *coro);
XR_FUNC void xr_worker_push_lifo(XrWorker *worker, XrCoroutine *coro);
XR_FUNC int xr_worker_push_lifo_batch(XrWorker *worker, XrCoroutine *first);
XR_FUNC XrCoroRunResult xr_coro_run_on_worker(XrWorker *worker, XrCoroutine *coro);

/* ========== Work Discovery (scan-based, cold path only) ==========
 *
 * Hot-path enqueue/dequeue never publishes global "has work" state; these
 * bounded read-only scans are the discovery mechanism for spinners, parkers
 * and steal candidate selection.
 */

// Any cross-worker delivery pending in some worker's MPSC inbox?
static inline bool xr_runtime_any_inbox_nonempty(XrRuntime *runtime) {
    for (int i = 0; i < runtime->worker_count; i++) {
        if (!xr_mpsc_empty(&runtime->workers[i].p.inbox))
            return true;
    }
    return false;
}

// Stealable victim bitmap: workers with a nonempty deque or continuation
// deque. LIFO slots are deliberately excluded — only the owner may consume
// its LIFO slot, exactly like Go's runnext.
static inline uint64_t xr_runtime_scan_stealable(XrRuntime *runtime, uint64_t exclude_bits) {
    uint64_t candidates = 0;
    int count = runtime->worker_count < 64 ? runtime->worker_count : 64;
    for (int i = 0; i < count; i++) {
        uint64_t bit = (uint64_t) 1ull << i;
        if (exclude_bits & bit)
            continue;
        XrProc *p = &runtime->workers[i].p;
        if (xr_steal_queue_size(&p->runq.deque) > 0 || p->runq.overflow_len > 0 ||
            xr_steal_queue_size(&p->cont_deque) > 0) {
            candidates |= bit;
        }
    }
    return candidates;
}

/* ========== Run Queue Operations (declared in xproc.h) ========== */

// xr_runq_init, xr_runq_destroy, xr_runq_enqueue, xr_runq_dequeue,
// xr_runq_steal, xr_runq_len are now declared in xproc.h

static inline int xr_worker_total_queue_len(XrWorker *worker) {
    return xr_proc_total_queue_len(&worker->p);
}

XR_FUNC XrWorker *xr_current_worker(void);

// Enqueue coro to target worker's inbox with full synchronization.
// Handles: MPSC push + Dekker fence + wake if parked.
// This is the ONLY correct way to push to a remote worker's inbox.
XR_FUNC void xr_worker_inbox_enqueue(XrRuntime *runtime, int target_id, XrCoroutine *coro);
XR_FUNC void xr_worker_inbox_enqueue_batch(XrRuntime *runtime, int target_id, XrCoroutine *first,
                                           XrCoroutine *last, int count);

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
XR_FUNC void xr_runtime_wake_worker(XrRuntime *runtime, int worker_id);

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
XR_FUNC bool xr_worker_wake_one_detached(XrWorker *worker, void *channel, bool wake_sender,
                                         XrCoroutine **ready_out);
XR_FUNC void xr_worker_wake_all(XrWorker *worker, void *channel);

/* ========== Select Support ========== */

XR_FUNC void xr_worker_block_select(XrWorker *worker, XrCoroutine *coro, void **channels,
                                    int count);
XR_FUNC XrCoroutine *xr_worker_wake_select_with_status(XrWorker *worker, void *channel,
                                                       int resume_status);
XR_FUNC XrCoroutine *xr_worker_wake_select(XrWorker *worker, void *channel);
XR_FUNC int xr_worker_wake_select_all_with_status(XrWorker *worker, void *channel,
                                                  int resume_status);
XR_FUNC void xr_worker_unblock_select(XrWorker *worker, XrCoroutine *coro);
/* Fully detach a coroutine still parked in select from its wait state on the
 * owner worker: drop every case from its channel bucket + the blocked queue,
 * mark the wait cancelled, dispose the select-owned `after` timer channel
 * (idempotent via the timer_disposed latch; design/885), and clear the
 * select-wait pointer. No-op when the coroutine holds no select wait. Shared by
 * the DONE drain path (worker_drain_inbox) and the CANCELLED completion path
 * (worker_handle_run_result). Must run on the coroutine's select-owner worker
 * (its affinity worker; see xr_coro_wake_target_id). */
XR_FUNC void xr_worker_teardown_select_wait(XrWorker *worker, XrCoroutine *coro);
XR_FUNC int xr_runtime_next_coro_id(XrRuntime *runtime);

/* ========== Syscall Enter/Exit (blocking-cfunc P status) ========== */

// Mark the current worker's P as P_SYSCALL around a non-yieldable blocking C
// call so sysmon does not treat the stalled heartbeat as a stuck coroutine. The
// M keeps its P; there is no handoff (hot I/O is yieldable, never blocks an M).
XR_FUNC void xr_worker_entersyscall(void);

// Restore P_RUNNING after the blocking C call returns (balances entersyscall,
// including nested blocking helpers).
XR_FUNC void xr_worker_exitsyscall(void);

/* ========== Diagnostics ========== */

// Print per-worker scheduling statistics to stderr
XR_FUNC void xr_runtime_print_stats(XrRuntime *runtime);

/* ========== Main Thread Entry ========== */

// Run main coroutine on calling thread using unified scheduling loop
XR_FUNC int xr_main_thread_run(XrayIsolate *X, XrCoroutine *main_coro);
XR_FUNC int xr_runtime_main_thread_run(XrRuntime *runtime, XrCoroutine *main_coro);

/* ========== Debug Support ========== */

// Resume coroutine execution after debug break, returns when next breakpoint hit or program ends
XR_FUNC int xr_debug_resume_coro(XrayIsolate *isolate, XrCoroutine *coro);

#endif  // XWORKER_H
