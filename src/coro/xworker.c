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
    worker->p.stats.executed_count = 0;
    worker->p.stats.stolen_count = 0;
    worker->p.stats.yielded_count = 0;

    // Bind pre-allocated M (1:1 with Worker at startup)
    worker->m = &runtime->machines[id];
    xr_machine_init(worker->m, id, runtime);

    // Initialize Chase-Lev deque run queues
    for (int p = 0; p < XR_RUNQ_COUNT; p++) {
        xr_runq_init(&worker->p.runq[p]);
    }

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
    worker->p.stats.cont_steal_count = 0;

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

    // Allocate guard page for JIT safepoint (one per worker)
    if (XR_JIT_AVAILABLE()) {
        worker->p.jit_scratch.safepoint_page = xr_jit_hooks->guard_page_alloc();
    }
    worker->p.jit_scratch.safepoint_return_pc = NULL;
}

// Destroy Worker
void xr_worker_destroy(XrWorker *worker) {
    XR_DCHECK(worker != NULL, "worker_destroy: NULL worker");
    // Destroy bound M (VM context, park mutex/cond, strbuf)
    if (worker->m) {
        xr_machine_destroy(worker->m);
    }

    // Free guard page for JIT safepoint
    if (worker->p.jit_scratch.safepoint_page && XR_JIT_AVAILABLE()) {
        xr_jit_hooks->guard_page_free(worker->p.jit_scratch.safepoint_page);
        worker->p.jit_scratch.safepoint_page = NULL;
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

    // Free Per-Worker VM stack slab free list
    while (worker->p.stack_slab_free) {
        void *block = worker->p.stack_slab_free;
        worker->p.stack_slab_free = *(void **) block;
        xr_free(block);
    }
    worker->p.stack_slab_count = 0;
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
    atomic_store(&runtime->idle_worker_list, (XrMachine *) NULL);
    atomic_store(&runtime->idle_worker_count, 0);

    // Initialize Spinning mechanism
    atomic_store(&runtime->spinning_count, 0);
    atomic_store(&runtime->wake_spinner, 0);
    atomic_store(&runtime->needspinning, 0);
    // no global queue, use Worker local queue + MPSC inbox directly

    // active_coros/spawned now tracked per-Worker, no global init needed
    atomic_store(&runtime->total_inbox_len, 0);

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
    runtime->workers = (XrWorker *) xr_calloc(num_workers, sizeof(XrWorker));
    if (!runtime->workers) {
        xr_free(runtime->machines);
        xr_free(runtime);
        return NULL;
    }

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
        xr_async_pool_init(runtime->async_pool, runtime, XR_ASYNC_THREAD_COUNT);
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

    // Drain all worker MPSC inboxes (coroutines pushed after running=false)
    for (int i = 0; i < runtime->worker_count; i++) {
        XrCoroutine *orphan = xr_mpsc_drain(&runtime->workers[i].p.inbox);
        while (orphan) {
            XrCoroutine *next = orphan->sched_link;
            orphan->sched_link = NULL;
            orphan = next;
        }
    }

    // Destroy Workers (also destroys bound M via xr_worker_destroy)
    for (int i = 0; i < runtime->worker_count; i++) {
        xr_worker_destroy(&runtime->workers[i]);
    }
    xr_free(runtime->workers);
    xr_free(runtime->machines);

    // blocked queue cleaned up when Worker destroyed

    // Cleanup Netpoll
    xr_netpoll_cleanup(&runtime->netpoll);

    // Destroy async thread pool
    if (runtime->async_pool) {
        xr_async_pool_destroy(runtime->async_pool);
        xr_free(runtime->async_pool);
        runtime->async_pool = NULL;
    }

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
        XrCoroutine *coro = runtime->workers[i].m ? runtime->workers[i].m->current_coro : NULL;
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

    // no global queue, always enqueue via inbox
    // Select target Worker (lowest load), fallback to Worker 0 if none available
    XrWorker *target = xr_choose_target_worker(runtime, -1);
    if (!target) {
        // All Workers unavailable, fallback to Worker 0
        target = &runtime->workers[0];
    }

    atomic_store_explicit(&coro->affinity_p, target->p.id, memory_order_relaxed);

    // Enqueue via unified inbox path (MPSC push + Dekker fence + wake)
    xr_worker_inbox_enqueue(runtime, target->p.id, coro);
    XR_DBG_CORO("spawn: coro id=%d enqueued to Worker %d inbox", coro->id, target->p.id);
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
    fprintf(stderr, "\n%-8s %12s %12s %12s %12s %8s\n", "Worker", "Executed", "Stolen", "Yielded",
            "ContSteal", "Blocked");
    fprintf(stderr, "%-8s %12s %12s %12s %12s %8s\n", "------", "--------", "------", "-------",
            "---------", "-------");

    uint64_t total_exec = 0, total_steal = 0, total_yield = 0, total_cont = 0;
    for (int i = 0; i < runtime->worker_count; i++) {
        XrProc *p = &runtime->workers[i].p;
        fprintf(stderr, "W%-7d %12llu %12llu %12llu %12llu %8d\n", i,
                (unsigned long long) p->stats.executed_count,
                (unsigned long long) p->stats.stolen_count,
                (unsigned long long) p->stats.yielded_count,
                (unsigned long long) p->stats.cont_steal_count, p->blocked_count);
        total_exec += p->stats.executed_count;
        total_steal += p->stats.stolen_count;
        total_yield += p->stats.yielded_count;
        total_cont += p->stats.cont_steal_count;
    }
    fprintf(stderr, "%-8s %12llu %12llu %12llu %12llu\n", "TOTAL", (unsigned long long) total_exec,
            (unsigned long long) total_steal, (unsigned long long) total_yield,
            (unsigned long long) total_cont);
    fprintf(stderr, "===========================\n\n");
}
