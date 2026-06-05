/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xworker.c - Runtime + Worker lifecycle and spawn entry points
 *
 * KEY CONCEPT:
 *   This file owns:
 *     - TLS current-worker / current-machine pointers
 *     - XrRuntime construction / destruction / start / stop
 *     - Lazy worker-thread bootstrap (xr_runtime_ensure_workers)
 *     - xr_runtime_spawn / xr_runtime_spawn_local entry points
 *     - Per-worker construct / destruct helpers
 *     - Statistics dump (xr_runtime_print_stats)
 *
 *   All hot-path logic (scheduling, stealing, execution, handoff,
 *   blocked queue, object pool, runq primitives) lives in sibling
 *   files: xworker_sched.c, xworker_exec.c, xworker_handoff.c,
 *   xworker_blocked.c, xworker_pool.c, xworker_runq.c.
 *
 * WHY THIS LAYOUT:
 *   Hot-path scheduling, stealing, execution, handoff, blocked queue,
 *   pool and runq logic each live in their own .c so this file stays
 *   the cold lifecycle boundary. Mixing them caused this file to
 *   balloon past the size limit and obscured ownership of mutable
 *   per-coro-state fields.
 */
#include "xworker_internal.h"
#include "../base/xchecks.h"
#include "../base/xlog.h"
#include "../runtime/gc/ximmix.h"
#include "../runtime/gc/xcoro_gc.h"
#include "../io/xio_runtime.h"  // xr_io_runtime_new / xr_io_runtime_free
#include "xjit_hooks.h"
#include <stdlib.h>
#include <string.h>
#include "../os/os_thread.h"

// Thread-local: current Worker and Machine pointers
XR_THREAD_LOCAL XrWorker *tls_current_worker = NULL;
XR_THREAD_LOCAL XrMachine *tls_current_machine = NULL;

static bool env_flag_enabled(const char *name) {
    const char *value = getenv(name);
    if (!value || value[0] == '\0' || value[0] == '0')
        return false;
    if (value[0] == 'f' || value[0] == 'F')
        return false;
    if (value[0] == 'n' || value[0] == 'N')
        return false;
    return true;
}

static int env_int_clamped(const char *name, int fallback, int min_value, int max_value) {
    const char *value = getenv(name);
    int parsed = fallback;
    if (value && value[0] != '\0') {
        parsed = atoi(value);
    }
    if (parsed < min_value)
        parsed = min_value;
    if (parsed > max_value)
        parsed = max_value;
    return parsed;
}

static int default_handoff_max_m(int workers) {
    int extra = workers > 4 ? workers : 4;
    int max_m = workers + extra;
    if (max_m < workers)
        max_m = workers;
    if (max_m > 256)
        max_m = 256;
    return max_m;
}

static double stats_ratio_u64(uint64_t numerator, uint64_t denominator) {
    if (denominator == 0)
        return 0.0;
    return (double) numerator / (double) denominator;
}

static double stats_percent_u64(uint64_t numerator, uint64_t denominator) {
    return 100.0 * stats_ratio_u64(numerator, denominator);
}

// Get current thread's Worker
XrWorker *xr_current_worker(void) {
    return tls_current_worker;
}

// ========== Worker Lifecycle ==========

// Initialize Worker (P fields + bind M)
void xr_worker_init(XrWorker *worker, int id, XrRuntime *runtime) {
    XR_DCHECK(worker != NULL, "worker_init: NULL worker");
    XR_DCHECK(runtime != NULL, "worker_init: NULL runtime");
    XR_DCHECK(id >= 0 && id < runtime->worker_count, "worker_init: invalid id");
    worker->p.id = id;
    worker->p.runtime = runtime;
    memset(&worker->p.stats, 0, sizeof(worker->p.stats));

    // Bind pre-allocated M (1:1 with Worker at startup)
    worker->m = &runtime->machines[id];
    xr_machine_init(worker->m, id, runtime);

    // Initialize Chase-Lev deque run queues
    for (int p = 0; p < XR_RUNQ_COUNT; p++) {
        xr_runq_init(&worker->p.runq[p]);
    }
    xr_priority_budget_init(&worker->p.prio_budget);
    atomic_store_explicit(&worker->p.local_runq_len, 0, memory_order_relaxed);

    // Initialize LIFO slot
    atomic_store_explicit(&worker->p.lifo_slot, NULL, memory_order_relaxed);
    worker->p.lifo_polls = 0;

    // Initialize random seed
    worker->p.rng_state = (uint32_t) (time(NULL) ^ (id * 0x9e3779b9));

    // Initialize local coroutine object pool
    worker->p.local_free_list = NULL;
    worker->p.local_free_count = 0;

    // Initialize Per-Worker Timer Wheel (lock-free, owner-private)
    worker->p.timer_wheel = xr_timer_wheel_create(runtime, id);
    worker->p.last_timer_tick = xr_monotonic_ticks();

    // Initialize MPSC inbox
    xr_mpsc_init(&worker->p.inbox);
    xr_chan_wake_queue_init(&worker->p.chan_wake_queue);
    worker->p.check_balance_reds = XR_CALL_CHECK_BALANCE_REDS;

    // Initialize Per-Worker local poll (kqueue/epoll fd for fast IO delivery)
    xr_local_poll_init(&worker->p.local_poll);

    // Initialize continuation stealing deque
    xr_steal_queue_init(&worker->p.cont_deque, 64);

    // Initialize Per-Worker blocked queue (lock-free)
    memset(worker->p.blocked_buckets, 0, sizeof(worker->p.blocked_buckets));
    worker->p.blocked_head = NULL;
    worker->p.blocked_tail = NULL;
    worker->p.blocked_count = 0;

    // Initialize run queue statistics
    for (int p = 0; p < XR_RUNQ_COUNT; p++) {
        worker->p.runq_reds[p] = 0;
        worker->p.runq_max_len[p] = 0;
    }

    // Create backend worker storage, if the active backend needs one.
    worker->p.backend_worker_storage = NULL;
    worker->p.backend_worker_storage_destroy = NULL;
    if (XR_JIT_AVAILABLE() && xr_jit_hooks->worker_state_create) {
        worker->p.backend_worker_storage = xr_jit_hooks->worker_state_create();
        worker->p.backend_worker_storage_destroy = xr_jit_hooks->worker_state_destroy;
    }
}

// Destroy Worker
void xr_worker_destroy(XrWorker *worker) {
    XR_DCHECK(worker != NULL, "worker_destroy: NULL worker");
    // Destroy bound M (VM context, park mutex/cond, strbuf)
    if (worker->m) {
        xr_machine_destroy(worker->m);
    }

    // Free backend worker storage through the owner backend's destructor.
    if (worker->p.backend_worker_storage && worker->p.backend_worker_storage_destroy) {
        worker->p.backend_worker_storage_destroy(worker->p.backend_worker_storage);
        worker->p.backend_worker_storage = NULL;
        worker->p.backend_worker_storage_destroy = NULL;
    }

    // Free Per-Worker local poll (kqueue/epoll fd)
    xr_local_poll_cleanup(&worker->p.local_poll);

    // Free Per-Worker Timer Wheel
    if (worker->p.timer_wheel) {
        xr_timer_wheel_destroy(worker->p.timer_wheel);
        worker->p.timer_wheel = NULL;
    }

    // Free Per-Worker Channel Wake Command Queue
    xr_chan_wake_queue_destroy(&worker->p.chan_wake_queue);

    // Free Per-Worker blocked buckets (hash table of XrBlockedBucket)
    for (int i = 0; i < XR_BLOCKED_BUCKET_SIZE; i++) {
        XrBlockedBucket *bucket = worker->p.blocked_buckets[i];
        while (bucket) {
            XrBlockedBucket *next = bucket->next;
            xr_free(bucket);
            bucket = next;
        }
        worker->p.blocked_buckets[i] = NULL;
    }

    // Flush Per-Worker Immix block cache L1 → L2
    xr_immix_flush_block_cache(worker->p.block_cache, &worker->p.block_cache_count);

    // Flush Per-Worker CoroGC free list L1 → L2 (per-isolate pool)
    XrSystemHeap *gc_heap = (worker->p.runtime && worker->p.runtime->isolate)
                                ? worker->p.runtime->isolate->sys_heap
                                : NULL;
    xr_coro_gc_flush_pool(gc_heap, &worker->p.gc_free_list, &worker->p.gc_free_count);
}

// ========== Runtime Construction / Destruction ==========

// Create Runtime
XrRuntime *xr_runtime_create(XrayIsolate *isolate, int num_workers) {
    XR_DCHECK(isolate != NULL, "runtime_create: NULL isolate");
    if (num_workers <= 0) {
        // Allow override via environment variable (for benchmarking)
        const char *env = getenv("XRAY_WORKERS");
        if (env && atoi(env) > 0) {
            num_workers = atoi(env);
        } else {
            // Default to CPU core count.
            num_workers = (int) xr_os_cpu_count();
            if (num_workers <= 0)
                num_workers = 1;
        }
    }
    if (num_workers > XR_MAX_WORKERS) {
        num_workers = XR_MAX_WORKERS;
    }

    XrRuntime *runtime = (XrRuntime *) xr_calloc(1, sizeof(XrRuntime));
    if (!runtime)
        return NULL;

    runtime->isolate = isolate;
    runtime->worker_count = num_workers;
    atomic_store(&runtime->running, false);
    atomic_store(&runtime->threads_started, false);
    atomic_store(&runtime->started_workers, 0);
    atomic_store(&runtime->exited_workers, 0);
    atomic_store(&runtime->active_workers, 0);

    // Initialize idle P/M management (lock-free Treiber stacks).
    // No sched_lock: all three idle lists use atomic CAS.
    atomic_store(&runtime->idle_p_head, (XrProc *) NULL);
    atomic_store(&runtime->idle_p_count, 0);
    atomic_store(&runtime->idle_m_head, (XrMachine *) NULL);
    atomic_store(&runtime->idle_m_count, 0);
    atomic_store(&runtime->m_count, num_workers);
    runtime->handoff_max_m =
        env_int_clamped("XRAY_HANDOFF_MAX_M", default_handoff_max_m(num_workers), num_workers, 256);
    atomic_store(&runtime->idle_worker_list, (XrMachine *) NULL);
    atomic_store(&runtime->idle_worker_count, 0);

    // Initialize Spinning mechanism
    atomic_store(&runtime->spinning_count, 0);
    atomic_store(&runtime->wake_spinner, 0);
    atomic_store(&runtime->needspinning, 0);
    for (int pi = 0; pi < XR_CORO_PRIORITY_COUNT; pi++) {
        atomic_store(&runtime->nonempty_p_mask[pi], 0);
        atomic_store(&runtime->stealable_p_mask[pi], 0);
    }
    atomic_store(&runtime->timer_p_mask, 0);
    atomic_store(&runtime->idle_p_mask, 0);
    atomic_store(&runtime->searching_count, 0);
    xr_injectq_init(runtime);

    // active_coros/spawned now tracked per-Worker, no global init needed
    atomic_store(&runtime->total_inbox_len, 0);
    runtime->sched_stats_enabled = env_flag_enabled("XRAY_SCHED_STATS");

    // main thread enqueues via inbox, no dedicated P needed

    // coroutine pool fully Per-Worker, no global init needed

    // Initialize Netpoll (I/O multiplexing)
    memset(&runtime->netpoll, 0, sizeof(XrNetpoll));
    if (xr_netpoll_init(&runtime->netpoll) < 0) {
        // Netpoll init failure is not fatal, continue running
    }

    // Initialize IO runtime (DNS cache today; future handle registry).
    // Heap-allocated so xworker.h only forward-declares XrIoRuntime.
    runtime->io = xr_io_runtime_new();

    // blocked queue fully Per-Worker, no global init needed
    atomic_store(&runtime->next_coro_id, 1);  // ID starts from 1
    runtime->current_scope = NULL;

    // Allocate Machines (M) array — pre-allocated 1:1 with Workers
    runtime->machines = (XrMachine *) xr_calloc(num_workers, sizeof(XrMachine));
    if (!runtime->machines) {
        xr_free(runtime);
        return NULL;
    }

    // Allocate Workers (P + M* pointer)
    // XrProc contains _Alignas(XR_CACHE_LINE) members, so the array
    // must be cache-line aligned to satisfy UBSan / hardware.
    size_t workers_size = (size_t) num_workers * sizeof(XrWorker);
    runtime->workers = (XrWorker *) xr_malloc_aligned(workers_size, XR_CACHE_LINE);
    if (!runtime->workers) {
        xr_free(runtime->machines);
        xr_free(runtime);
        return NULL;
    }

    memset(runtime->workers, 0, workers_size);

    // Initialize Workers (binds each Worker to its pre-allocated M)
    for (int i = 0; i < num_workers; i++) {
        xr_worker_init(&runtime->workers[i], i, runtime);
    }

    // Per-Worker Timer Wheel, lock-free high performance
    // Sysmon heartbeat monitoring runs on netpoll thread

    // Create async thread pool
    // For blocking syscalls (file I/O, DNS, etc.)
    runtime->async_pool = (XrAsyncPool *) xr_calloc(1, sizeof(XrAsyncPool));
    if (runtime->async_pool) {
        int async_threads = env_int_clamped("XRAY_ASYNC_THREADS", XR_ASYNC_THREAD_COUNT, 1, 64);
        int async_queue_limit =
            env_int_clamped("XRAY_ASYNC_QUEUE_LIMIT", XR_ASYNC_QUEUE_LIMIT, 1, 1 << 20);
        xr_async_pool_init(runtime->async_pool, runtime, async_threads, async_queue_limit);
    }

    // initialize load balancing module
    xr_balance_init(runtime);

    return runtime;
}

// Destroy Runtime
void xr_runtime_destroy(XrRuntime *runtime) {
    if (!runtime)
        return;

    // Stop running (internally calls xr_thread_join to wait for all Workers to exit)
    xr_runtime_stop(runtime);

    // Leak detection: check for unreleased coroutines after all workers stopped
    int active = xr_runtime_active_coros(runtime);
    if (active > 0) {
        uint64_t spawned = 0;
        for (int _wi = 0; _wi < runtime->worker_count; _wi++)
            spawned += runtime->workers[_wi].p.stats.spawned_count;
        uint64_t completed = 0;
        for (int _wi = 0; _wi < runtime->worker_count; _wi++)
            completed += runtime->workers[_wi].p.stats.completed_count;
        xr_log_warning("runtime",
                       "%d coroutine(s) leaked "
                       "(spawned=%llu, completed=%llu)",
                       active, (unsigned long long) spawned, (unsigned long long) completed);
        for (int i = 0; i < runtime->worker_count; i++) {
            XrProc *p = &runtime->workers[i].p;
            int runq_len = xr_proc_total_queue_len(p);
            if (runq_len > 0 || p->blocked_count > 0) {
                xr_log_warning("runtime", "  W%d: runq=%d blocked=%d", i, runq_len,
                               p->blocked_count);
            }
        }
    }

    // Channel leak detection: compare create vs close counts
    {
        uint64_t ch_closed = xr_channel_get_close_count(runtime->isolate);
        if (runtime->isolate && runtime->isolate->sys_heap) {
            uint64_t ch_created =
                atomic_load(&runtime->isolate->sys_heap->stats.channel_create_count);
            if (ch_created > ch_closed) {
                xr_log_warning("runtime",
                               "%llu channel(s) not closed "
                               "(created=%llu, closed=%llu)",
                               (unsigned long long) (ch_created - ch_closed),
                               (unsigned long long) ch_created, (unsigned long long) ch_closed);
            }
        }
    }

    if (runtime->sched_stats_enabled) {
        xr_runtime_print_stats(runtime);
    }

    // Drain all worker MPSC inboxes (coroutines pushed after running=false)
    for (int i = 0; i < runtime->worker_count; i++) {
        XrCoroutine *orphan = xr_mpsc_drain(&runtime->workers[i].p.inbox);
        while (orphan) {
            XrCoroutine *next = orphan->sched_link;
            orphan->sched_link = NULL;
            orphan = next;
        }
    }

    // Destroy async thread pool
    if (runtime->async_pool) {
        xr_async_pool_destroy(runtime->async_pool);
        xr_free(runtime->async_pool);
        runtime->async_pool = NULL;
    }

    // Destroy Workers (also destroys bound M via xr_worker_destroy)
    for (int i = 0; i < runtime->worker_count; i++) {
        xr_worker_destroy(&runtime->workers[i]);
    }
    xr_injectq_destroy(runtime);
    xr_free_aligned(runtime->workers, XR_CACHE_LINE);
    xr_free(runtime->machines);

    // blocked queue cleaned up when Worker destroyed

    // Cleanup Netpoll
    xr_netpoll_cleanup(&runtime->netpoll);

    // Tear down IO runtime (DNS cache and any future IO state).
    xr_io_runtime_free(runtime->io);
    runtime->io = NULL;

    xr_free(runtime);
}

// ========== Runtime Start / Stop / Lazy Ensure ==========

// Start Runtime (state only, no threads created)
//
// Threads are created lazily by xr_runtime_ensure_workers() on first spawn.
// Worker 0 runs on main thread via xr_main_thread_run().
void xr_runtime_start(XrRuntime *runtime) {
    XR_DCHECK(runtime != NULL, "runtime_start: NULL runtime");
    if (atomic_load(&runtime->running))
        return;
    atomic_store(&runtime->running, true);
}

// Lazy start: create sysmon + Worker 1~N + async pool threads.
// Called on first spawn. Thread-safe via atomic CAS.
void xr_runtime_ensure_workers(XrRuntime *runtime) {
    XR_DCHECK(runtime != NULL, "runtime_ensure_workers: NULL runtime");
    // Fast path: already started
    if (atomic_load_explicit(&runtime->threads_started, memory_order_acquire)) {
        return;
    }

    // Slow path: CAS to claim the right to start
    bool expected = false;
    if (!atomic_compare_exchange_strong(&runtime->threads_started, &expected, true)) {
        // Another thread won the race, wait for them to finish.
        // Futex-based wait instead of busy sched_yield(). Each
        // worker_loop bumps started_workers + wakes this address, giving us
        // sub-millisecond wakeup and zero spinning.
        int expected_workers = runtime->worker_count - 1;
        for (;;) {
            int cur = atomic_load_explicit(&runtime->started_workers, memory_order_acquire);
            if (cur >= expected_workers)
                break;
            xr_park_futex_wait(&runtime->started_workers, cur, 1000 /* us */);
        }
        return;
    }

    // Start sysmon thread (heartbeat monitoring + stuck detection)
    xr_thread_create(&runtime->sysmon_thread, sysmon_thread_func, runtime);

    // Start async pool threads
    if (runtime->async_pool) {
        xr_async_pool_start_threads(runtime->async_pool);
    }

    // Worker threads need larger stack for nested run() calls (e.g. module import)
    // and ASan instrumentation which greatly inflates stack frame sizes.
    for (int i = 1; i < runtime->worker_count; i++) {
        xr_thread_create_ex(&runtime->workers[i].m->thread, worker_loop, &runtime->workers[i],
                            XR_WORKER_STACK_BYTES);
    }

    // Wait for Worker 1..N to become ready.
    // Futex-based wait (1 ms timeout guards against missed wake).
    int expected_workers = runtime->worker_count - 1;
    for (;;) {
        int cur = atomic_load_explicit(&runtime->started_workers, memory_order_acquire);
        if (cur >= expected_workers)
            break;
        xr_park_futex_wait(&runtime->started_workers, cur, 1000 /* us */);
    }
}

// Stop Runtime
//
// Worker 0 runs on main thread, only need to join Worker 1~N
void xr_runtime_stop(XrRuntime *runtime) {
    XR_DCHECK(runtime != NULL, "runtime_stop: NULL runtime");
    atomic_store(&runtime->running, false);

    // Fast path: no threads were ever created
    if (!atomic_load(&runtime->threads_started)) {
        return;
    }

    // Wake netpoll thread
    if (atomic_load(&runtime->netpoll.inited)) {
        xr_netpoll_break(&runtime->netpoll);
    }
    xr_thread_join(runtime->sysmon_thread, NULL);

    // Wake all Workers to check running flag and exit
    for (int i = 0; i < runtime->worker_count; i++) {
        worker_unpark(&runtime->workers[i]);
    }

    // Wake all idle M threads parked in handoff_thread_entry.
    //
    // Lock-free traversal: idle_m_head is now a Treiber stack.
    // We snapshot the head via atomic_load and walk the chain. Because M is
    // never freed and idle_link is only mutated on push/pop (which happen
    // under runtime->running), a racy load here yields a consistent
    // point-in-time snapshot sufficient for best-effort wake-on-shutdown.
    {
        XrMachine *idle = atomic_load_explicit(&runtime->idle_m_head, memory_order_acquire);
        while (idle) {
            atomic_store_explicit(&idle->park_state, XR_PARK_WOKEN, memory_order_release);
            xr_park_futex_wake(&idle->park_state);
            idle = idle->idle_link;
        }
    }

    // Join Worker 1~N (guard against NULL m during handoff)
    for (int i = 1; i < runtime->worker_count; i++) {
        XrMachine *wm = runtime->workers[i].m;
        if (wm)
            xr_thread_join(wm->thread, NULL);
    }
}

// Force-stop Runtime without joining threads.
// Safe to call from external watchdog thread while main thread is in worker_loop.
// Also sets CANCEL_REQUESTED on all currently running coroutines so that
// JIT safepoints and interpreter back-edges can detect and bail out.
void xr_runtime_force_stop(XrRuntime *runtime) {
    if (!runtime)
        return;
    atomic_store(&runtime->running, false);
    for (int i = 0; i < runtime->worker_count; i++) {
        XrCoroutine *coro =
            runtime->workers[i].m
                ? atomic_load_explicit(&runtime->workers[i].m->current_coro, memory_order_relaxed)
                : NULL;
        if (coro) {
            xr_coro_flags_set(coro, XR_CORO_FLG_CANCEL_REQUESTED);
        }
        worker_unpark(&runtime->workers[i]);
    }
}

// ========== Spawn ==========

// Spawn coroutine into Runtime scheduling
//
// Affinity design:
// - New coroutines prefer creator's P local queue
// - Improves cache hit rate, reduces coroutine migration overhead
void xr_runtime_spawn(XrRuntime *runtime, XrCoroutine *coro) {
    XR_DCHECK(runtime != NULL, "runtime_spawn: NULL runtime");
    XR_DCHECK(coro != NULL, "runtime_spawn: NULL coro");
    // Lazy start: ensure worker threads are running before spawning
    xr_runtime_ensure_workers(runtime);

    // runnext: new coroutine goes to LIFO slot for DFS execution.
    // Previous occupant evicted to FIFO queue (available for work stealing).
    XrWorker *current = xr_current_worker();
    if (current && current->p.runtime == runtime) {
        current->p.stats.spawned_count++;
        atomic_store_explicit(&coro->affinity_p, current->p.id, memory_order_relaxed);
        xr_worker_push_lifo(current, coro);
        XR_DBG_CORO("spawn: coro id=%d enqueued to Worker %d", coro->id, current->p.id);
        return;
    }

    // External spawns enter the global injection queue so idle workers can
    // claim the work directly instead of waiting on one target inbox.
    XrWorker *target = xr_choose_target_worker(runtime, -1);
    if (!target) {
        // All Workers unavailable, fallback to Worker 0
        target = &runtime->workers[0];
    }

    atomic_store_explicit(&coro->affinity_p, target->p.id, memory_order_relaxed);
    xr_injectq_push(runtime, coro);
    XR_DBG_CORO("spawn: coro id=%d injected with affinity Worker %d", coro->id, target->p.id);
}

// Spawn coroutine into specified Worker's local queue
void xr_runtime_spawn_local(XrWorker *worker, XrCoroutine *coro) {
    XR_DCHECK(worker != NULL, "runtime_spawn_local: NULL worker");
    XR_DCHECK(coro != NULL, "runtime_spawn_local: NULL coro");
    worker->p.stats.spawned_count++;
    xr_worker_push(worker, coro);
}

// ========== Diagnostics ==========

// Print per-worker scheduling statistics (for performance tuning)
void xr_runtime_print_stats(XrRuntime *runtime) {
    if (!runtime)
        return;

    fprintf(stderr, "\n=== Xray Runtime Statistics ===\n");
    fprintf(stderr, "Workers: %d\n", runtime->worker_count);
    {
        uint64_t _tc = 0;
        for (int _wi = 0; _wi < runtime->worker_count; _wi++)
            _tc += runtime->workers[_wi].p.stats.completed_count;
        uint64_t _ts = 0;
        for (int _wi2 = 0; _wi2 < runtime->worker_count; _wi2++)
            _ts += runtime->workers[_wi2].p.stats.spawned_count;
        fprintf(stderr, "Total spawned: %llu, completed: %llu\n", (unsigned long long) _ts,
                (unsigned long long) _tc);
    }
    fprintf(stderr, "Active coros: %d\n", xr_runtime_active_coros(runtime));
    fprintf(stderr, "Machines: total=%d cap=%d idle=%d\n",
            atomic_load_explicit(&runtime->m_count, memory_order_relaxed), runtime->handoff_max_m,
            atomic_load_explicit(&runtime->idle_m_count, memory_order_relaxed));
    fprintf(stderr,
            "\n%-8s %10s %10s %9s %8s %10s %8s %10s %10s %10s %9s %10s %9s %8s %8s %8s "
            "%8s %8s\n",
            "Worker", "Executed", "LocalPop", "LifoHit", "Inject", "Stolen", "StealOK", "StealTry",
            "StealWait", "Yielded", "Cont", "LifoFlush", "Inbox", "Park", "Unpark", "Timer",
            "Burst", "Blocked");
    fprintf(stderr,
            "%-8s %10s %10s %9s %8s %10s %8s %10s %10s %10s %9s %10s %9s %8s %8s %8s "
            "%8s %8s\n",
            "------", "--------", "--------", "-------", "------", "------", "-------", "--------",
            "---------", "-------", "----", "---------", "-----", "----", "------", "-----",
            "-----", "-------");

    uint64_t total_exec = 0, total_steal = 0, total_steal_try = 0, total_steal_skip = 0;
    uint64_t total_steal_success = 0, total_steal_backoff = 0, total_local_pop = 0;
    uint64_t total_steal_no_candidate = 0, total_steal_fresh_reject = 0;
    uint64_t total_steal_candidate_scan = 0, total_steal_throttle_wait = 0;
    uint64_t total_inject_pull = 0;
    uint64_t total_yield = 0;
    uint64_t total_cont = 0, total_lifo_hit = 0, total_lifo_flush = 0, total_inbox = 0;
    uint64_t total_lifo_gate_budget = 0, total_lifo_gate_backlog = 0;
    uint64_t total_lifo_gate_priority = 0;
    uint64_t total_fast_dispatch = 0, total_fast_dispatch_budget_stop = 0;
    uint64_t total_fast_dispatch_empty = 0;
    uint64_t total_park = 0, total_unpark = 0, total_timer = 0, total_burst = 0;
    uint64_t total_wait_ms = 0, max_wait_ms = 0, total_prio_boost = 0;
    uint64_t total_blocked = 0;
    for (int i = 0; i < runtime->worker_count; i++) {
        XrProc *p = &runtime->workers[i].p;
        fprintf(stderr,
                "W%-7d %10llu %10llu %9llu %8llu %10llu %8llu %10llu %10llu %10llu %9llu "
                "%10llu %9llu %8llu %8llu %8llu %8llu %8d\n",
                i, (unsigned long long) p->stats.executed_count,
                (unsigned long long) p->stats.local_runq_pop_count,
                (unsigned long long) p->stats.lifo_hit_count,
                (unsigned long long) p->stats.inject_pull_count,
                (unsigned long long) p->stats.stolen_count,
                (unsigned long long) p->stats.steal_success_count,
                (unsigned long long) p->stats.steal_attempt_count,
                (unsigned long long) p->stats.steal_backoff_count,
                (unsigned long long) p->stats.yielded_count,
                (unsigned long long) p->stats.cont_steal_count,
                (unsigned long long) p->stats.lifo_flush_count,
                (unsigned long long) p->stats.inbox_drain_count,
                (unsigned long long) p->stats.park_count,
                (unsigned long long) p->stats.unpark_count,
                (unsigned long long) p->stats.timer_bump_count,
                (unsigned long long) p->stats.timer_burst_count, p->blocked_count);
        total_exec += p->stats.executed_count;
        total_local_pop += p->stats.local_runq_pop_count;
        total_inject_pull += p->stats.inject_pull_count;
        total_steal += p->stats.stolen_count;
        total_steal_success += p->stats.steal_success_count;
        total_steal_backoff += p->stats.steal_backoff_count;
        total_steal_try += p->stats.steal_attempt_count;
        total_steal_skip += p->stats.steal_skip_count;
        total_steal_no_candidate += p->stats.steal_no_candidate_count;
        total_steal_fresh_reject += p->stats.steal_fresh_reject_count;
        total_steal_candidate_scan += p->stats.steal_candidate_scan_count;
        total_steal_throttle_wait += p->stats.steal_throttle_wait_count;
        total_yield += p->stats.yielded_count;
        total_cont += p->stats.cont_steal_count;
        total_lifo_hit += p->stats.lifo_hit_count;
        total_lifo_flush += p->stats.lifo_flush_count;
        total_lifo_gate_budget += p->stats.lifo_gate_budget_count;
        total_lifo_gate_backlog += p->stats.lifo_gate_backlog_count;
        total_lifo_gate_priority += p->stats.lifo_gate_priority_count;
        total_fast_dispatch += p->stats.fast_dispatch_count;
        total_fast_dispatch_budget_stop += p->stats.fast_dispatch_budget_stop_count;
        total_fast_dispatch_empty += p->stats.fast_dispatch_empty_count;
        total_inbox += p->stats.inbox_drain_count;
        total_park += p->stats.park_count;
        total_unpark += p->stats.unpark_count;
        total_timer += p->stats.timer_bump_count;
        total_burst += p->stats.timer_burst_count;
        total_wait_ms += p->stats.runnable_wait_ms;
        if (p->stats.runnable_wait_max_ms > max_wait_ms) {
            max_wait_ms = p->stats.runnable_wait_max_ms;
        }
        total_prio_boost += p->stats.priority_boost_count;
        if (p->blocked_count > 0) {
            total_blocked += (uint64_t) p->blocked_count;
        }
    }
    fprintf(stderr,
            "%-8s %10llu %10llu %9llu %8llu %10llu %8llu %10llu %10llu %10llu %9llu "
            "%10llu %9llu %8llu %8llu %8llu %8llu %8llu\n",
            "TOTAL", (unsigned long long) total_exec, (unsigned long long) total_local_pop,
            (unsigned long long) total_lifo_hit, (unsigned long long) total_inject_pull,
            (unsigned long long) total_steal, (unsigned long long) total_steal_success,
            (unsigned long long) total_steal_try, (unsigned long long) total_steal_backoff,
            (unsigned long long) total_yield, (unsigned long long) total_cont,
            (unsigned long long) total_lifo_flush, (unsigned long long) total_inbox,
            (unsigned long long) total_park, (unsigned long long) total_unpark,
            (unsigned long long) total_timer, (unsigned long long) total_burst,
            (unsigned long long) total_blocked);

    uint64_t ready_dispatches = total_local_pop + total_lifo_hit;
    uint64_t lifo_gate_total =
        total_lifo_gate_budget + total_lifo_gate_backlog + total_lifo_gate_priority;
    fprintf(stderr,
            "Dispatch mix: local_runq=%llu lifo=%llu lifo_share=%.2f%% inject_pull=%llu "
            "stolen_items=%llu cont_steal=%llu\n",
            (unsigned long long) total_local_pop, (unsigned long long) total_lifo_hit,
            stats_percent_u64(total_lifo_hit, ready_dispatches),
            (unsigned long long) total_inject_pull, (unsigned long long) total_steal,
            (unsigned long long) total_cont);
    fprintf(stderr,
            "Steal: attempts=%llu success=%llu success_ratio=%.2f%% stolen_items=%llu "
            "items_per_success=%.2f skipped=%llu backoff=%llu no_candidate=%llu "
            "fresh_reject=%llu candidate_scans=%llu throttle_wait=%llu scans_per_attempt=%.2f "
            "defer_ratio=%.2f%% skip_ratio=%.2f%%\n",
            (unsigned long long) total_steal_try, (unsigned long long) total_steal_success,
            stats_percent_u64(total_steal_success, total_steal_try),
            (unsigned long long) total_steal, stats_ratio_u64(total_steal, total_steal_success),
            (unsigned long long) total_steal_skip, (unsigned long long) total_steal_backoff,
            (unsigned long long) total_steal_no_candidate,
            (unsigned long long) total_steal_fresh_reject,
            (unsigned long long) total_steal_candidate_scan,
            (unsigned long long) total_steal_throttle_wait,
            stats_ratio_u64(total_steal_candidate_scan, total_steal_try),
            stats_percent_u64(total_steal_backoff, total_steal_try + total_steal_backoff),
            stats_percent_u64(total_steal_skip, total_steal_try + total_steal_skip));
    fprintf(stderr,
            "Runnable wait: total_ms=%llu avg_ms=%.3f max_ms=%llu dispatches=%llu "
            "priority_boost=%llu\n",
            (unsigned long long) total_wait_ms, stats_ratio_u64(total_wait_ms, ready_dispatches),
            (unsigned long long) max_wait_ms, (unsigned long long) ready_dispatches,
            (unsigned long long) total_prio_boost);
    fprintf(
        stderr, "LIFO gate: total=%llu budget=%llu backlog=%llu priority=%llu gate_ratio=%.2f%%\n",
        (unsigned long long) lifo_gate_total, (unsigned long long) total_lifo_gate_budget,
        (unsigned long long) total_lifo_gate_backlog, (unsigned long long) total_lifo_gate_priority,
        stats_percent_u64(lifo_gate_total, lifo_gate_total + total_lifo_hit));
    fprintf(stderr, "Fast dispatch: hits=%llu budget_stop=%llu empty=%llu hit_share=%.2f%%\n",
            (unsigned long long) total_fast_dispatch,
            (unsigned long long) total_fast_dispatch_budget_stop,
            (unsigned long long) total_fast_dispatch_empty,
            stats_percent_u64(total_fast_dispatch, total_lifo_hit));

    XrSchedGlobalStats *s = &runtime->sched_stats;
    uint64_t wake_alloc = xr_sched_metric_load(&s->chan_wake_cmd_alloc_count);
    uint64_t wake_free = xr_sched_metric_load(&s->chan_wake_cmd_free_count);
    uint64_t wake_dispatch = xr_sched_metric_load(&s->chan_wake_cmd_dispatch_count);
    uint64_t wake_drain = xr_sched_metric_load(&s->chan_wake_cmd_drain_count);
    uint64_t wake_coalesce = xr_sched_metric_load(&s->chan_wake_cmd_coalesce_count);
    uint64_t wake_stale = xr_sched_metric_load(&s->chan_wake_cmd_stale_count);
    uint64_t wake_forward = xr_sched_metric_load(&s->chan_wake_cmd_forward_count);
    fprintf(stderr,
            "\nChannel wake commands: alloc=%llu free=%llu dispatch=%llu drain=%llu coalesce=%llu "
            "stale=%llu forward=%llu\n",
            (unsigned long long) wake_alloc, (unsigned long long) wake_free,
            (unsigned long long) wake_dispatch, (unsigned long long) wake_drain,
            (unsigned long long) wake_coalesce, (unsigned long long) wake_stale,
            (unsigned long long) wake_forward);
    fprintf(stderr,
            "Channel wake diagnostics: cross_worker=%llu drain_per_dispatch=%.2f "
            "coalesce_ratio=%.2f%% stale_ratio=%.2f%% forward_per_dispatch=%.2f\n",
            (unsigned long long) wake_dispatch, stats_ratio_u64(wake_drain, wake_dispatch),
            stats_percent_u64(wake_coalesce, wake_drain), stats_percent_u64(wake_stale, wake_drain),
            stats_ratio_u64(wake_forward, wake_dispatch));
    fprintf(stderr, "Select: block=%llu heap_alloc=%llu inline_alloc=%llu\n",
            (unsigned long long) xr_sched_metric_load(&s->select_block_count),
            (unsigned long long) xr_sched_metric_load(&s->select_heap_alloc_count),
            (unsigned long long) xr_sched_metric_load(&s->select_inline_alloc_count));
    fprintf(stderr, "Timeout: yield_retry=%llu event_block=%llu\n",
            (unsigned long long) xr_sched_metric_load(&s->timeout_yield_retry_count),
            (unsigned long long) xr_sched_metric_load(&s->timeout_event_block_count));
    fprintf(stderr,
            "Channel hot path: no_waiter_buffer=%llu kind_spsc=%llu kind_mpsc=%llu "
            "kind_mpmc=%llu\n",
            (unsigned long long) xr_sched_metric_load(&s->chan_buffer_no_waiter_count),
            (unsigned long long) xr_sched_metric_load(&s->chan_kind_spsc_count),
            (unsigned long long) xr_sched_metric_load(&s->chan_kind_mpsc_count),
            (unsigned long long) xr_sched_metric_load(&s->chan_kind_mpmc_count));
    fprintf(stderr, "Handoff: reuse=%llu create=%llu cap_hit=%llu create_fail=%llu\n",
            (unsigned long long) xr_sched_metric_load(&s->handoff_reuse_count),
            (unsigned long long) xr_sched_metric_load(&s->handoff_create_count),
            (unsigned long long) xr_sched_metric_load(&s->handoff_cap_hit_count),
            (unsigned long long) xr_sched_metric_load(&s->handoff_create_fail_count));
    if (runtime->async_pool) {
        XrAsyncPool *pool = runtime->async_pool;
        fprintf(
            stderr,
            "Async: threads=%d live=%d queue=%d limit=%d max_queue=%d in_flight=%d "
            "submit=%llu complete=%llu reject=%llu\n",
            pool->thread_count, atomic_load_explicit(&pool->live_threads, memory_order_relaxed),
            atomic_load_explicit(&pool->queue_depth, memory_order_relaxed), pool->queue_limit,
            atomic_load_explicit(&pool->max_queue_depth, memory_order_relaxed),
            atomic_load_explicit(&pool->in_flight, memory_order_relaxed),
            (unsigned long long) atomic_load_explicit(&pool->submit_count, memory_order_relaxed),
            (unsigned long long) atomic_load_explicit(&pool->complete_count, memory_order_relaxed),
            (unsigned long long) atomic_load_explicit(&pool->reject_count, memory_order_relaxed));
    }
    {
        int inject_len = 0;
        for (int pi = 0; pi < XR_CORO_PRIORITY_COUNT; pi++) {
            inject_len += atomic_load_explicit(&runtime->injectq[pi].len, memory_order_relaxed);
        }
        uint64_t inject_push = xr_sched_metric_load(&s->inject_push_count);
        uint64_t inject_pop = xr_sched_metric_load(&s->inject_pop_count);
        uint64_t inject_spill = xr_sched_metric_load(&s->inject_spill_count);
        long long inject_pending = (long long) inject_push - (long long) inject_pop;
        long long pull_pop_delta = (long long) total_inject_pull - (long long) inject_pop;
        fprintf(stderr, "Inject: push=%llu pop=%llu spill=%llu len=%d mask=0x%x\n",
                (unsigned long long) inject_push, (unsigned long long) inject_pop,
                (unsigned long long) inject_spill, inject_len,
                atomic_load_explicit(&runtime->nonempty_inject_mask, memory_order_relaxed));
        fprintf(stderr,
                "Inject diagnostics: pop_push_ratio=%.2f%% spill_ratio=%.2f%% pending_est=%lld "
                "worker_pull=%llu pull_pop_delta=%lld\n",
                stats_percent_u64(inject_pop, inject_push),
                stats_percent_u64(inject_spill, inject_push), inject_pending,
                (unsigned long long) total_inject_pull, pull_pop_delta);
    }
    fprintf(stderr,
            "Masks: runq=[0x%llx,0x%llx,0x%llx] stealable=[0x%llx,0x%llx,0x%llx] "
            "timer=0x%llx idle=0x%llx searching=%d\n",
            (unsigned long long) atomic_load_explicit(&runtime->nonempty_p_mask[0],
                                                      memory_order_relaxed),
            (unsigned long long) atomic_load_explicit(&runtime->nonempty_p_mask[1],
                                                      memory_order_relaxed),
            (unsigned long long) atomic_load_explicit(&runtime->nonempty_p_mask[2],
                                                      memory_order_relaxed),
            (unsigned long long) atomic_load_explicit(&runtime->stealable_p_mask[0],
                                                      memory_order_relaxed),
            (unsigned long long) atomic_load_explicit(&runtime->stealable_p_mask[1],
                                                      memory_order_relaxed),
            (unsigned long long) atomic_load_explicit(&runtime->stealable_p_mask[2],
                                                      memory_order_relaxed),
            (unsigned long long) atomic_load_explicit(&runtime->timer_p_mask, memory_order_relaxed),
            (unsigned long long) atomic_load_explicit(&runtime->idle_p_mask, memory_order_relaxed),
            atomic_load_explicit(&runtime->searching_count, memory_order_relaxed));
    fprintf(stderr, "===========================\n\n");
}
