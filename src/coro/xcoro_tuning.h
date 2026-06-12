/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_tuning.h - Centralized tuning constants for the coro module
 *
 * KEY CONCEPT:
 *   Single source of truth for cross-file tuning knobs used by the
 *   scheduler, worker, coroutine pool, balancer and work-stealing logic.
 *
 * WHY THIS FILE:
 *   - Kills scattered magic numbers inside hot-path function bodies
 *     (e.g. `#define XR_SPIN_COUNT 20` buried inside a for-loop).
 *   - Makes it trivial to tune via one header instead of grepping
 *     across xworker.c / xcoro.c / xchannel.c.
 *   - Keeps module-internal constants (e.g. XR_TW_*, XR_FDMAP_*,
 *     XR_SYSMON_*, XR_CORO_POOL_*) in their own headers — this file
 *     only holds knobs referenced by more than one translation unit
 *     or knobs that previously lived inline as magic numbers.
 *
 * EDIT GUIDELINES:
 *   - Every constant MUST document: unit, default, effect.
 *   - Group by subsystem.
 *   - If a constant only affects one .c file, define it in that .c
 *     file instead of here.
 */

#ifndef XCORO_TUNING_H
#define XCORO_TUNING_H

/* ========== Scheduler / Worker Loop ========== */

/* Worker thread stack size (bytes). Large to accommodate deep VM recursion
 * and ASan instrumentation which inflates per-frame stack usage. */
#define XR_WORKER_STACK_BYTES (8 * 1024 * 1024)

/* Max consecutive LIFO slot pops before draining the LIFO slot back into
 * the main deque. Prevents starvation in tight ping-pong workloads where
 * A resumes B resumes A ... would otherwise never yield the CPU. */
#define XR_MAX_LIFO_POLLS 3

/* Worker spin rounds before parking when no work is found.
 * Each spin does a monotonic-time check + cheap poll. Higher = less sleep
 * overhead under bursty load, lower = better responsiveness when idle. */
#define XR_WORKER_SPIN_COUNT 20

/* Maximum workers allowed to scan for steal victims at the same time.
 * The dynamic gate is worker_count / XR_SEARCHING_WORKER_DIVISOR, capped by
 * XR_SEARCHING_WORKER_MAX. This keeps idle workers from turning a small amount
 * of stealable work into an all-worker cache-line storm. */
#define XR_SEARCHING_WORKER_DIVISOR 4
#define XR_SEARCHING_WORKER_MAX 4

/* Max delay before retrying a steal search after visible victims were too
 * fresh to steal. Higher values reduce idle-worker scanning; lower values
 * improve reaction time when a short local burst turns into backlog. */
#define XR_STEAL_BACKOFF_MAX_MS 2

/* Upper bound on consecutive "BLOCKED fast-redispatch" hops inside
 * worker_exec_with_cont_stealing. Limits worst-case starvation of other
 * coroutines when A and B bounce through a channel for ms-scale bursts. */
#define XR_FAST_DISPATCH_BUDGET 64

/* In-dispatch direct coroutine switch (VM interpreter): when a coroutine
 * blocks on a channel/await op and a just-woken partner sits in the LIFO
 * slot, the dispatch loop swaps vm_ctx and continues interpreting without
 * exiting run(). 1 = enabled (default), 0 = always fall back to the worker
 * fast-redispatch path (kill switch for bisection). */
#ifndef XR_VM_DIRECT_SWITCH
#define XR_VM_DIRECT_SWITCH 1
#endif

/* Number of local enqueue timestamps that may share one monotonic clock read.
 * Submit time is used as a millisecond-scale scheduling hint for aging,
 * freshness, and diagnostics; exact per-coro clock reads only add contention
 * in spawn/yield bursts. */
#define XR_SCHED_TIME_CACHE_BUDGET 32

/* Max coroutines pulled from the global inject queue during a normal scheduler
 * poll. Larger batches reduce mutex traffic under external spawn bursts;
 * smaller batches keep per-worker queues from monopolizing global work. */
#define XR_INJECT_POP_BATCH 32

/* Smaller inject pull used inside blocked fast-dispatch. This keeps tight
 * channel ping-pong loops from starving externally injected work without
 * destroying the locality benefit of the fast path. */
#define XR_FAST_DISPATCH_INJECT_BATCH 4

/* Local ready-queue depth at which spawn stops using child-first inline
 * execution on multi-worker runtimes. This keeps fan-out bursts visible to
 * stealing workers while preserving cache locality for short spawn chains. */
#define XR_SPAWN_INLINE_LOCAL_BACKLOG 16

/* Global inject backlog at which spawn stops using child-first inline
 * execution. A batch-sized threshold avoids reacting to isolated external
 * wakes while still draining sustained global work. */
#define XR_SPAWN_INLINE_GLOBAL_BACKLOG XR_INJECT_POP_BATCH

/* Same-parent spawn burst length before the scheduler starts queueing some
 * children locally for stealing. This keeps chain_spawn child-first while
 * exposing real fan-out loops once the parent has proven it is producing a
 * batch rather than a single child to await. */
#define XR_SPAWN_SHARE_BURST_THRESHOLD 2

/* Share one child every N same-parent spawns after the burst threshold.
 * Lower values expose CPU parallelism sooner; higher values preserve DFS
 * locality and reduce local queue churn. */
#define XR_SPAWN_SHARE_BURST_INTERVAL 1

/* Maximum queued spawn-sharing backlog per worker. The effective limit is
 * clamped by the local queue capacity so sharing cannot force global spill
 * under pure spawn bursts. */
#define XR_SPAWN_SHARE_BACKLOG_PER_WORKER 4

/* ========== Coroutine Pool ==========
 *
 * Subsystem-internal sizing lives in xcoro_pool.h
 * (XR_CORO_POOL_INIT_SIZE / _GROW_SIZE / _STACK_SLOTS ...).
 * The constants below are used by xworker.c's per-worker caching logic.
 */

/* Batch size when a worker steals coroutines from the global free list
 * or returns them to it. Larger = fewer cross-thread syncs, smaller =
 * lower tail latency on batch recycle. */
#define XR_CORO_BATCH_SIZE 32

/* Batch size when a worker reserves a range of fresh slots from the
 * global pool's arena. Each worker consumes its cached range locally
 * via a single atomic fetch_add on pool->alloc_idx. */
#define XR_ARENA_BATCH_SIZE 64

/* ========== Balancer ========== */

/* Balance check interval in milliseconds. The balancer scans all workers'
 * queue lengths, computes average/max, and emigrates coroutines from
 * overloaded workers to underloaded ones if the threshold is exceeded. */
#define XR_BALANCE_CHECK_INTERVAL_MS 100

/* Emigration threshold multiplier: a worker is considered overloaded
 * when its runq length > multiplier * average. 2x is conservative,
 * lower values trigger more frequent rebalancing. */
#define XR_MIGRATION_THRESHOLD_MULTIPLIER 2

#endif /* XCORO_TUNING_H */
