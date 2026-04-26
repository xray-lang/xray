/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xproc.h - Processor (P) definitions for P/M split scheduler
 *
 * KEY CONCEPT:
 *   XrProc is the scheduling resource unit (analogous to Go's P).
 *   Number of P's is fixed = CPU core count.
 *   A Machine (M/OS thread) must acquire a P to execute coroutines.
 *   P owns: run queues, timer wheel, blocked queue, MPSC inbox.
 *
 * WHY THIS DESIGN:
 *   - Decouples scheduling resources from OS threads
 *   - Enables handoff: when M blocks in C code, P migrates to another M
 *   - Fixed P count bounds concurrency to CPU cores
 *
 * RELATED MODULES:
 *   - xmachine.h: OS thread (M) that executes on a P
 *   - xworker.h: Worker = P + M combined
 */

#ifndef XPROC_H
#define XPROC_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "xsteal_queue.h"
#include "xmpsc_queue.h"
#include "xcoroutine.h"
#include "xnetpoll.h"           // XrLocalPoll
#include "../base/xplatform.h"  // XR_CACHE_LINE

// Forward declarations
struct XrMachine;
struct XrTimerWheel;
struct XrRuntime;
struct XrCStackPool;
struct XrCanceledTimerNode;

/* ========== Channel Wake Command (MPSC queue, Vyukov style) ==========
 *
 * When a channel send/recv/close succeeds, remote workers that may have
 * blocked waiters need to process their own blocked buckets.  Instead of
 * directly touching the remote worker's owner-private data, the originator
 * enqueues a lightweight command here.  The owning worker drains the queue
 * in its scheduling loop and performs the local wake_one / wake_select /
 * wake_all on its own thread.
 *
 * Queue uses the same Vyukov MPSC pattern as XrTimerCancelQueue.
 */

typedef struct XrChanWakeCmd {
    void *channel;               // Channel pointer
    bool wake_sender;            // true = wake sender, false = wake receiver
    bool is_close;               // true = wake_all (channel close fan-out)
    struct XrChanWakeCmd *next;  // Intrusive MPSC link
} XrChanWakeCmd;

typedef struct XrChanWakeCmdQueue {
    _Atomic(XrChanWakeCmd *) head;  // Consumer reads here
    _Atomic(XrChanWakeCmd *) tail;  // Producers push here
    XrChanWakeCmd stub;             // Vyukov stub sentinel
} XrChanWakeCmdQueue;

/* ========== Per-P Statistics (cache-line aligned) ========== */

// XrProcStats — owner-written per-Worker counters, aggregated by
// xr_runtime_print_stats and coro leak detection.
//
// Alignment: the whole sub-struct lives on its own cache line(s) inside
// XrProc so that owner-only writes to these counters never dirty lines
// shared with lifo_slot / runq / cont_deque that other workers may be
// concurrently reading during work-stealing.
typedef struct XrProcStats {
    uint64_t executed_count;    // Coros the owner has dispatched
    uint64_t stolen_count;      // Coros the owner has stolen from peers
    uint64_t yielded_count;     // Voluntary yields
    uint64_t cont_steal_count;  // Continuations stolen (owner as stealer)
    uint64_t completed_count;   // Coros that finished (replaces global)
    uint64_t spawned_count;     // Coros spawned by this worker
} XrProcStats XR_ALIGN(XR_CACHE_LINE);

/* ========== Run Queue (Chase-Lev deque + overflow) ========== */

typedef struct XrRunQueue {
    XrStealQueue deque;           // Chase-Lev deque (lock-free)
    XrCoroutine *overflow_first;  // RESCHEDULE_LOW delayed queue
    XrCoroutine *overflow_last;
    int overflow_len;
} XrRunQueue;

XR_FUNC void xr_runq_init(XrRunQueue *rq);
XR_FUNC void xr_runq_destroy(XrRunQueue *rq);
XR_FUNC void xr_runq_enqueue(XrRunQueue *rq, XrCoroutine *coro);
XR_FUNC XrCoroutine *xr_runq_dequeue(XrRunQueue *rq);
XR_FUNC int xr_runq_steal(XrRunQueue *src, XrRunQueue *dst, int max_steal);

static inline int xr_runq_len(XrRunQueue *rq) {
    return xr_steal_queue_size(&rq->deque) + rq->overflow_len;
}

/* ========== P Status ========== */

typedef enum {
    P_IDLE,     // Not bound to any M
    P_RUNNING,  // Bound to M, actively scheduling
    P_SYSCALL,  // M blocked in syscall, P awaiting handoff
    P_HANDOFF,  // Handoff M is running this P
    P_DEAD      // Shutting down
} XrProcStatus;

/* ========== XrProc (P) — Scheduling resource, count fixed = CPU cores ========== */

typedef struct XrProc {
    /* === Identity === */
    int id;
    _Atomic uint32_t status;  // XrProcStatus

    /* === Run Queues === */
    XrRunQueue runq[XR_RUNQ_COUNT];

    /* === LIFO Slot (locality optimization) === */
    _Atomic(XrCoroutine *) lifo_slot;
    int lifo_slot_prio;  // Cached priority of lifo_slot occupant (avoids atomic flags load on
                         // eviction)
    int lifo_polls;

    /* === Per-P Timer Wheel === */
    struct XrTimerWheel *timer_wheel;
    int64_t last_timer_tick;

    /* === MPSC Inbox (cross-thread coroutine delivery) === */
    XrMPSCQueue inbox;

    /* === Channel Wake Command Queue (MPSC, Vyukov) === */
    XrChanWakeCmdQueue chan_wake_queue;

    /* === Back pointer to current M === */
    _Atomic(struct XrMachine *) current_m;

    /* === Load Balancing === */
    int check_balance_reds;
    int runq_reds[XR_RUNQ_COUNT];
    int runq_max_len[XR_RUNQ_COUNT];

    /* === Local Coroutine Cache Pool === */
    XrCoroutine *local_free_list;
    int local_free_count;

    /* === Deferred Recycle (cont-stealing: recycle fire-and-forget child on next pool_get) === */
    XrCoroutine *pending_recycle_coro;

    /* === Per-Worker Arena Cache (batch alloc from global pool, avoids per-coro atomic) === */
    uint32_t arena_cache_start;  // Next available slot in cached range
    uint32_t arena_cache_end;    // End of cached range (exclusive)
    void *arena_cache_block;     // Cached block pointer (invalidate if pool expands)

    /* === Stack Slab Free List (avoids malloc/free per coroutine) === */
    void *stack_slab_free;  // Free list of stack+frames blocks (1312B each)
    int stack_slab_count;   // Number of blocks in free list

/* === Immix Block Cache L1 (per-worker, no lock needed) === */
#define XR_BLOCK_CACHE_L1_MAX 8
    void *block_cache[XR_BLOCK_CACHE_L1_MAX];  // Cached aligned blocks
    int block_cache_count;                     // Number of cached blocks

    /* === CoroGC Free List (avoids malloc/free per coroutine that needs heap) === */
    struct XrCoroGC *gc_free_list;  // Free list of XrCoroGC structs (320B each)
    int gc_free_count;              // Number of XrCoroGC in free list

    /* === Per-P Coroutine ID Cache (avoids atomic_fetch_add per spawn) === */
    int id_cache;      // Next available coroutine ID
    int id_cache_end;  // End of cached ID range (exclusive)

    /* === Per-P Run Queue Length (avoids global atomic counter bounce) === */
    int local_runq_len;  // Total coroutines in this P's queues + lifo_slot

    /* === Blocked Queue === */
    XrBlockedBucket *blocked_buckets[XR_BLOCKED_BUCKET_SIZE];
    XrCoroutine *blocked_head;
    XrCoroutine *blocked_tail;
    int blocked_count;
    int select_waiter_count;

/* === Canceled Timer Node Freelist (avoids malloc/free churn) === */
#define XR_CANCEL_NODE_POOL_MAX 64
    struct XrCanceledTimerNode *cancel_node_free;
    int cancel_node_free_count;

    /* === Deferred Free Queue (MPSC Treiber stack for cross-worker PollDesc) === */
    _Atomic(void *) deferred_free_head;

    /* === Handoff Signaling === */
    _Atomic bool handoff_exit;  // Signal handoff M to release P
    _Atomic int handoff_sync;   // Futex word: exitsyscall waits, handoff M wakes after releasing P

    /* === Continuation Stealing === */
    XrStealQueue cont_deque;  // Chase-Lev deque for parent continuations
    int yield_streak;         // Consecutive yields without block/completion (detect compute-bound)

    /* === Per-Worker I/O Poll (kqueue/epoll fd per worker) === */
    XrLocalPoll local_poll;  // Per-worker kqueue/epoll for IO event collection

    /* === Adaptive Poll Skip (I/O load feedback) === */
    uint32_t io_poll_ewma;  // EWMA of I/O event frequency (0-256 fixed-point, 256=always busy)

    /* === Per-Worker Active Coro Counter (replaces global atomic active_coros) === */
    int local_active_coros;

    /* === Statistics (cache-line isolated) ===
     *
     * All owner-written counters are grouped into XrProcStats
     * and aligned to XR_CACHE_LINE. This moves them onto dedicated cache
     * lines away from hot per-P scheduler fields (lifo_slot / runq) and
     * the potentially cross-worker-written cont_deque, eliminating a
     * subtle false-sharing source observed at high steal rates.
     *
     * All fields are written by the owner only. Stats aggregation
     * (xr_runtime_print_stats etc.) reads across workers; reads may see
     * a torn 64-bit value on some ABIs but that is acceptable because
     * stats are best-effort display-only.
     */
    XrProcStats stats;
    uint32_t rng_state;

    /* === Per-Worker JIT Scratch Space ===
     * JIT functions don't yield, so only one JIT execution per worker.
     * Coroutines access this via coro->jit_ctx pointer. */
    XrJitScratch jit_scratch;

    /* === Runtime back pointer === */
    struct XrRuntime *runtime;

    /* === Idle P linkage === */
    struct XrProc *idle_link;  // Idle P list link
} XrProc;

/* ========== P Lifecycle API ========== */

XR_FUNC void xr_proc_init(XrProc *p, int id, struct XrRuntime *runtime);
XR_FUNC void xr_proc_destroy(XrProc *p);

/* ========== P/M Binding API ========== */

XR_FUNC void xr_acquirep(struct XrMachine *m, XrProc *p);
XR_FUNC XrProc *xr_releasep(struct XrMachine *m);
XR_FUNC void xr_handoffp(XrProc *p);

/* ========== P Run Queue Operations ========== */

XR_FUNC XrCoroutine *xr_proc_pop(XrProc *p);
XR_FUNC void xr_proc_push(XrProc *p, XrCoroutine *coro);

static inline int xr_proc_total_queue_len(XrProc *p) {
    int total = 0;
    for (int i = 0; i < XR_RUNQ_COUNT; i++) {
        total += xr_runq_len(&p->runq[i]);
    }
    return total;
}

/* ========== Idle P Management ========== */

XR_FUNC XrProc *xr_get_idle_p(struct XrRuntime *runtime);
XR_FUNC void xr_put_idle_p(struct XrRuntime *runtime, XrProc *p);

#endif  // XPROC_H
