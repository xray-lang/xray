/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoroutine.h - Coroutine type definitions
 *
 * KEY CONCEPT:
 *   - XrCoroutine: scheduler-owned coroutine shell plus backend payload
 *   - VM/AOT execution state lives behind backend_state
 *   - Reduction-based fair scheduling
 *
 * SCHEDULING INVARIANTS:
 *
 *   INVARIANT 1 (State exclusivity): A coroutine is in exactly ONE state
 *   at any time: READY, RUNNING, BLOCKED, or DONE. The state is encoded
 *   in atomic flags. Transitions are CAS-guarded to prevent races.
 *
 *   INVARIANT 2 (Single runner): At most one Worker executes a coroutine
 *   at any time. A coroutine moves from a run queue to RUNNING only on
 *   the Worker that dequeues it. No other Worker may touch its stack.
 *
 *   INVARIANT 3 (Queue membership): A READY coroutine is on exactly ONE
 *   queue: a Worker's local run queue, LIFO slot, MPSC inbox, or
 *   runtime injection queue.
 *   A BLOCKED coroutine is on a channel wait queue or timer wheel.
 *   RUNNING and DONE coroutines are on no queue.
 *
 *   INVARIANT 4 (Reduction fairness): Each coroutine starts a time slice
 *   with XR_CORO_REDUCTIONS. Backends decrement on backward jumps,
 *   calls, or their equivalent safepoints. When reductions <= 0, the
 *   coroutine yields to the scheduler. This prevents any single coroutine
 *   from starving others.
 *
 *   INVARIANT 5 (Affinity hint): affinity_p is a hint for wake targeting.
 *   It is NOT a hard binding. The scheduler may migrate coroutines via
 *   work-stealing. Only channel_wake_coro uses affinity_p to choose
 *   the target Worker for waking a blocked coroutine.
 *
 *   INVARIANT 6 (GC isolation): Each coroutine has its own GC heap
 *   (XrCoroGC). Cross-coroutine object transfer requires deep copy
 *   or shared storage (reference counted). A coroutine's GC never
 *   touches another coroutine's heap objects.
 *
 * COROUTINE STATE MACHINE:
 *
 *   READY ──► RUNNING ──► READY     (yield / preempt)
 *     │          │
 *     │          ├──► BLOCKED        (channel send/recv, sleep, I/O)
 *     │          │       │
 *     │          │       └──► READY  (wake: channel data, timer, I/O ready)
 *     │          │
 *     │          └──► DONE           (function returns / unhandled exception)
 *     │
 *     └── created via go statement, starts as READY
 */

#ifndef XCOROUTINE_H
#define XCOROUTINE_H

#include <stdatomic.h>
#include "../base/xconstants.h"
#include "../base/xchecks.h"
#include "../runtime/value/xvalue.h"
#include "xcoro_abi.h"
#include "xcoro_flags.h"
#include "xslot_ref.h"
#include "xtimer_wheel.h"
#include "xwait_state.h"

/* ========== Forward Declarations ========== */

struct XrCoroGC;
struct XrCoroMonitor;
struct XrCoroRegistry;
struct XrRuntime;
struct XrRuntimeCore;
typedef struct XrWaitQueue XrWaitQueue;
typedef struct XrCoroutine XrCoroutine;

/* ========== XrCoroExt - Cold fields allocated on demand ========== */

typedef struct XrCoroExt {
    const char *name;
    const char *source_file;
    int source_line;
    struct XrCoroutine *parent_coro;
    int spawn_line;
    const char *spawn_file;

    char *io_buf;  // I/O read buffer (reused across calls)
    size_t io_buf_cap;
    struct XrMap *locals;              // Per-coroutine dynamic locals (debug/inspect)
    struct XrCoroMonitor *watched_by;  // Monitor list head (lifecycle watchers)

    /* === Thread-lock extras (only set when Coro.lockThread() is called) === */
    _Atomic int lock_count;  // lock nesting depth (0 = unlocked)
    int locked_worker;       // Worker ID that owns the lock (-1 = none)

    /* === Await/scope wait state (cold; only blocking await/scope paths use it) === */
    XrCoroWaitState wait;

    XrValue *recv_slot;
    XrSlotRef recv_slot_ref;
    /* Resume-with-value delivery (untimed VM channel recv):
     * chan_ok_slot_ref is the delivery capability registered at block time.
     * When the waker hands over a value that needs no receive-side deep
     * copy, it stores value+ok directly into the blocked coroutine's
     * register slots and sets chan_resume_delivered; the resume fast path
     * then skips the instruction replay and continues from the next
     * instruction. Values that need a receive-side deep copy keep the
     * replay/resume protocol (the copy must run on the receiver's own
     * thread), so the waker leaves chan_resume_delivered false. */
    XrSlotRef chan_ok_slot_ref;
    bool chan_resume_delivered;

    struct XrCoroutine *chan_wait_next;
    struct XrCoroutine *chan_wait_prev;
    XrWaitQueue *chan_wait_queue;
    struct XrCoroutine *wait_link;
    struct XrCoroutine *wait_prev;
    int64_t work_queue_hint;
    /* Relaxed atomics: written by the owner worker when mirroring a timed
     * channel wait into its blocked bucket; peeked by remote wakers to route
     * the wake through the owner's inbox. Staleness only affects routing —
     * the owner-side drain re-checks under its own ownership. */
    _Atomic(XrBlockedBucket *) wait_bucket;
    _Atomic int wait_bucket_owner;
    XrChannelWaitToken chan_wait_token;
    _Atomic(void *) wait_channel;
    bool wait_send;
    XrValue send_value;
    struct XrCoroutine *pending_spawn;

    /* === Timer (only allocated on first sleep/timeout use) === */
    XrTWheelTimer timer;
    _Atomic bool timer_active;
    /* Relaxed atomic: routing hint read by cross-worker cancel; the timer
     * token CAS protocol is the authoritative arbitration. */
    _Atomic int timer_wheel_owner;  // Worker ID that owns the timer (-1 = none)
    _Atomic uintptr_t timer_seq;

    /* === Select storage (reused across blocking select operations) === */
    XrSelectStorage select_storage;

    /* === Structured concurrency membership (cold; only scope children use it) === */
    struct XrScopeContext *parent_scope;
    struct XrCoroutine *scope_sibling;
} XrCoroExt;

/* ========== XrCoroutine - Coroutine Object ========== */

struct XrCoroutine {
    /* ================================================================
     * HOT ZONE (first 64 bytes) — accessed every schedule/yield cycle
     * ================================================================ */
    XrGCHeader gc;           // 16 bytes: GC header (must be first)
    _Atomic uint32_t flags;  //  4 bytes: state flags (every dispatch)
    /* Relaxed atomic: owner decrements on back-edges; preempt/cancel pokes
     * it to 0 cross-thread (xr_coro_request_yield). A lost poke is benign —
     * the atomic CANCEL_REQUESTED flag is the authoritative signal. */
    _Atomic int32_t reductions;      //  4 bytes: remaining before yield
    struct XrCoroutine *sched_link;  //  8 bytes: MPSC/steal queue linkage
    struct XrCoroutine *next;        //  8 bytes: blocked/ready list linkage
    struct XrCoroutine *prev;        //  8 bytes: blocked/ready list linkage
    _Atomic int resume_status;       //  4 bytes: checked on every resume
    _Atomic int affinity_p;          //  4 bytes: preferred worker for wake (relaxed ok, hint only)
    int8_t schedule_count;           //  1 byte: schedule counter (max XR_RESCHEDULE_LOW=8)
    uint16_t gc_flags;               //  2 bytes: pool and backend lifetime flags
    // --- 64 bytes boundary ---

    /* === Work Stealing Freshness (set on enqueue, read on steal peek) === */
    /* Atomic because steal-side freshness scans peek this field through the
     * victim's deque without taking ownership; relaxed is enough — the value
     * is a heuristic and tolerates staleness, it only needs tear-freedom. */
    _Atomic int64_t submit_time;  //  8 bytes: monotonic ms when enqueued to run queue

    /* === Backend Execution State === */
    const XrCoroBackendVTable *backend;
    void *backend_state;

    /* ================================================================
     * WARM ZONE — GC/result hot fields and backend-owned cold state
     * ================================================================ */
    struct XrCoroGC *coro_gc;     // GC safepoint: checked every loop back-edge
    struct XrRuntimeCore *core;   // VM-neutral runtime resources for this coroutine
    struct XrRuntime *scheduler;  // owning scheduler runtime, NULL before multicore attach
    struct XrayIsolate *isolate;  // VM/AOT bridge while backend APIs still need isolate
    XrValue result;
    XrValue error;
    /* true: `error` came from the value-return channel (user `throw <enum>`);
     * false: from the panic channel (div-zero, OOB, assert, …).  Drives how
     * a linked scope re-raises a child failure into the parent. */
    bool error_is_value;
    int id;                      // coroutine ID for diagnostics/debug protocol
    uint16_t spawn_burst_count;  // Consecutive same-parent spawn results.

    /* === Task Handle (GC-managed user-visible handle) === */
    struct XrTask *task;  // back-pointer to associated XrTask (NULL for main coro)

    /* === Per-Coroutine Scope Tracking === */
    /* Atomic: the owner coroutine mutates current_scope as it enters/exits
     * structured-concurrency scopes (on its own worker), while a sibling
     * completion path on another worker reads it via
     * wake_waiter_scope_owner_ready() to decide whether to wake the parked
     * owner. Owner writes publish with release; the cross-worker read uses
     * acquire. Same-thread reads use relaxed. */
    _Atomic(struct XrScopeContext *) current_scope;

    /* === Cold Extension (io_buf, locals, watched_by — allocated on demand) === */
    /* Atomic: lazily allocated by the owner and published with release;
     * cross-thread observers (cancel, wakers, sysmon diagnostics) read it
     * concurrently. Plain expression access compiles to a seq_cst load,
     * which is acceptable off the hot paths; hot helpers use the relaxed
     * accessors below. */
    _Atomic(XrCoroExt *) ext;
};

static inline struct XrRuntimeCore *xr_coro_core(const XrCoroutine *coro) {
    return coro ? coro->core : NULL;
}

static inline struct XrRuntime *xr_coro_scheduler(const XrCoroutine *coro) {
    return coro ? coro->scheduler : NULL;
}

static inline bool xr_coro_backend_is_vm(const XrCoroutine *coro) {
    return coro && coro->backend && coro->backend->kind == XR_CORO_BACKEND_VM;
}

static inline void xr_coro_attach_backend(XrCoroutine *coro, const XrCoroBackendVTable *backend,
                                          void *backend_state) {
    if (!coro)
        return;
    coro->backend = backend;
    coro->backend_state = backend_state;
}

static inline bool xr_coro_backend_prepare_recycle(XrCoroutine *coro, XrWorker *worker) {
    if (!coro || !coro->backend || !coro->backend->prepare_recycle)
        return false;
    return coro->backend->prepare_recycle(coro, worker);
}

static inline bool xr_coro_backend_reset_reusable(XrCoroutine *coro) {
    if (!coro || !coro->backend || !coro->backend->reset_reusable)
        return false;
    coro->backend->reset_reusable(coro);
    return true;
}

/* ========== XrCoroExt Accessor ========== */

#include "../base/xmalloc.h"

static inline void xr_coro_ext_init(XrCoroExt *ext) {
    if (!ext)
        return;
    ext->locked_worker = -1;
    ext->work_queue_hint = -1;
    ext->wait_bucket_owner = -1;
    ext->timer.slot = XR_TW_SLOT_INACTIVE;
    ext->timer.timeout = NULL;
    ext->timer.arg = NULL;
    ext->timer.owner_worker_id = -1;
    atomic_store_explicit(&ext->timer.state, XR_TIMER_STATE_ACTIVE, memory_order_relaxed);
    atomic_store_explicit(&ext->timer.cancel_next, NULL, memory_order_relaxed);
    atomic_store_explicit(&ext->timer_active, false, memory_order_relaxed);
    ext->timer_wheel_owner = -1;
    atomic_store_explicit(&ext->timer_seq, 0, memory_order_relaxed);
}

/* Owner-context read of the extension pointer (relaxed: the owner published
 * it itself, or inherited it through a scheduling handoff that carries the
 * happens-before edge). */
static inline XrCoroExt *xr_coro_ext(const XrCoroutine *coro) {
    return atomic_load_explicit(&((XrCoroutine *) coro)->ext, memory_order_relaxed);
}

static inline XrCoroExt *xr_coro_ensure_ext(XrCoroutine *coro) {
    XrCoroExt *ext = xr_coro_ext(coro);
    if (!ext) {
        ext = (XrCoroExt *) xr_calloc(1, sizeof(XrCoroExt));
        xr_coro_ext_init(ext);
        /* Release: cross-thread observers that acquire-load ext must see
         * the initialized contents. */
        atomic_store_explicit(&coro->ext, ext, memory_order_release);
    }
    return ext;
}

static inline XrCoroWaitState *xr_coro_wait_state(XrCoroutine *coro) {
    XrCoroExt *ext = coro ? xr_coro_ext(coro) : NULL;
    return ext ? &ext->wait : NULL;
}

static inline const XrCoroWaitState *xr_coro_wait_state_const(const XrCoroutine *coro) {
    XrCoroExt *ext = coro ? xr_coro_ext(coro) : NULL;
    return ext ? &ext->wait : NULL;
}

static inline XrCoroWaitState *xr_coro_ensure_wait_state(XrCoroutine *coro) {
    XrCoroExt *ext = coro ? xr_coro_ensure_ext(coro) : NULL;
    return ext ? &ext->wait : NULL;
}

static inline const char *xr_coro_name(const XrCoroutine *coro) {
    return (coro && coro->ext) ? coro->ext->name : NULL;
}

static inline const char *xr_coro_source_file(const XrCoroutine *coro) {
    return (coro && coro->ext) ? coro->ext->source_file : NULL;
}

static inline int xr_coro_source_line(const XrCoroutine *coro) {
    return (coro && coro->ext) ? coro->ext->source_line : 0;
}

static inline XrCoroutine *xr_coro_parent(const XrCoroutine *coro) {
    return (coro && coro->ext) ? coro->ext->parent_coro : NULL;
}

static inline const char *xr_coro_spawn_file(const XrCoroutine *coro) {
    return (coro && coro->ext) ? coro->ext->spawn_file : NULL;
}

static inline int xr_coro_spawn_line(const XrCoroutine *coro) {
    return (coro && coro->ext) ? coro->ext->spawn_line : 0;
}

static inline void xr_coro_clear_debug_identity(XrCoroutine *coro) {
    if (!coro || !coro->ext)
        return;
    coro->ext->name = NULL;
    coro->ext->source_file = NULL;
    coro->ext->source_line = 0;
    coro->ext->parent_coro = NULL;
    coro->ext->spawn_file = NULL;
    coro->ext->spawn_line = 0;
}

static inline bool xr_coro_set_name(XrCoroutine *coro, const char *name) {
    if (!coro)
        return false;
    if (!name) {
        if (coro->ext)
            coro->ext->name = NULL;
        return true;
    }
    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext)
        return false;
    ext->name = name;
    return true;
}

static inline bool xr_coro_set_source(XrCoroutine *coro, const char *file, int line) {
    if (!coro)
        return false;
    if (!file && line == 0) {
        if (coro->ext) {
            coro->ext->source_file = NULL;
            coro->ext->source_line = 0;
        }
        return true;
    }
    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext)
        return false;
    ext->source_file = file;
    ext->source_line = line;
    return true;
}

static inline bool xr_coro_set_spawn_origin(XrCoroutine *coro, XrCoroutine *parent,
                                            const char *file, int line) {
    if (!coro)
        return false;
    if (!parent && !file && line == 0) {
        if (coro->ext) {
            coro->ext->parent_coro = NULL;
            coro->ext->spawn_file = NULL;
            coro->ext->spawn_line = 0;
        }
        return true;
    }
    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext)
        return false;
    ext->parent_coro = parent;
    ext->spawn_file = file;
    ext->spawn_line = line;
    return true;
}

/* ========== Thread-Lock Query Helpers ========== */

// Check if coroutine is pinned to a specific worker via Coro.lockThread().
static inline bool xr_coro_is_thread_locked(XrCoroutine *coro) {
    if (!coro->ext)
        return false;
    return atomic_load_explicit(&coro->ext->lock_count, memory_order_relaxed) > 0 &&
           coro->ext->locked_worker >= 0;
}

// Effective wake target: locked_worker if thread-locked, else affinity_p.
// All wake-routing paths should use this instead of reading affinity_p directly,
// so that Coro.lockThread() is respected across channel wake, sleep timeout,
// async completion, netpoll, and scope wake.
static inline int xr_coro_wake_target_id(XrCoroutine *coro) {
    if (coro->ext) {
        int lc = atomic_load_explicit(&coro->ext->lock_count, memory_order_relaxed);
        if (lc > 0 && coro->ext->locked_worker >= 0) {
            return coro->ext->locked_worker;
        }
    }
    return atomic_load_explicit(&coro->affinity_p, memory_order_relaxed);
}

/* ========== Backend Integration APIs ========== */

XR_FUNC bool xr_coro_reset_execution_state(XrCoroutine *coro, struct XrayIsolate *X);
XR_FUNC bool xr_coro_init_shell(XrCoroutine *coro, struct XrayIsolate *X, const char *name,
                                bool need_storage);
XR_FUNC void xr_coro_discard_uninitialized(XrCoroutine *coro);

/* Relaxed accessors for the reductions counter (see field doc). */
static inline int32_t xr_coro_reds(const XrCoroutine *coro) {
    return atomic_load_explicit(&((XrCoroutine *) coro)->reductions, memory_order_relaxed);
}

static inline void xr_coro_set_reds(XrCoroutine *coro, int32_t v) {
    atomic_store_explicit(&coro->reductions, v, memory_order_relaxed);
}

/* Owner-side: subtract cost, return remaining. Plain load/store pair (not
 * RMW) — the only cross-thread writer pokes 0, and losing that poke is
 * benign. Keeps the hot back-edge free of atomic RMW cost. */
static inline int32_t xr_coro_consume_reds(XrCoroutine *coro, int32_t cost) {
    int32_t next = xr_coro_reds(coro) - cost;
    xr_coro_set_reds(coro, next);
    return next;
}

// Check if coroutine should yield (at loop back-edges).
static inline bool xr_coro_should_yield(XrCoroutine *coro) {
    return xr_coro_reds(coro) <= 0;
}

// Request yield at next safepoint (preempt, GC, cancel)
// Forces reductions to 0 so the single-check safepoint triggers
static inline void xr_coro_request_yield(XrCoroutine *coro) {
    xr_coro_set_reds(coro, 0);
}

/* ========== Coroutine State (isolate-level coroutine bookkeeping) ========== */

typedef struct XrCoroState {
    _Atomic int total_created;
    XrScopeContext *current_scope;
    struct XrCoroRegistry *coro_registry;  // Named coroutine registry (lazy init)
} XrCoroState;

/* ========== Coroutine API ========== */

struct XrayIsolate;
struct XrClosure;

// Lifecycle
XR_FUNC XrCoroutine *xr_coro_create_empty(struct XrayIsolate *X, const char *name);
XR_FUNC XrCoroutine *xr_coro_create_native(struct XrayIsolate *X, void (*func)(void *), void *arg,
                                           const char *name);
XR_FUNC void xr_coro_free(XrCoroutine *coro);
XR_FUNC void xr_coro_destroy(XrCoroutine *coro);
XR_FUNC void xr_coro_spawn(struct XrayIsolate *X, XrCoroutine *coro);
XR_FUNC struct XrScopeContext *xr_coro_parent_scope(const XrCoroutine *coro);
XR_FUNC bool xr_coro_set_parent_scope(XrCoroutine *coro, struct XrScopeContext *scope);
XR_FUNC XrCoroutine *xr_coro_scope_sibling(const XrCoroutine *coro);
XR_FUNC bool xr_coro_set_scope_sibling(XrCoroutine *coro, XrCoroutine *sibling);
XR_FUNC XrSelectWait *xr_coro_select_wait(XrCoroutine *coro);
XR_FUNC void xr_coro_clear_select_wait(XrCoroutine *coro);
XR_FUNC bool xr_coro_set_pending_spawn(XrCoroutine *coro, XrCoroutine *child);
XR_FUNC XrCoroutine *xr_coro_take_pending_spawn(XrCoroutine *coro);

// Isolate-level coroutine bookkeeping
XR_FUNC void xr_coro_state_init(XrCoroState *state);
XR_FUNC void xr_coro_state_destroy(XrCoroState *state);

// Multicore runtime
XR_FUNC void xr_multicore_init(struct XrayIsolate *X, int num_workers);
XR_FUNC void xr_multicore_destroy(struct XrayIsolate *X);

// Wake mechanism
XR_FUNC void xr_scheduler_ready(struct XrRuntime *runtime, XrCoroutine *gp, bool next);
XR_FUNC void xr_coro_ready(struct XrayIsolate *X, XrCoroutine *gp, bool next);
XR_FUNC XrCoroutine *xr_current_coro(struct XrayIsolate *X);
XR_FUNC void xr_coro_wake_waiter(struct XrayIsolate *X, XrCoroutine *coro);
XR_FUNC void xr_coro_wake_scope_waiter(struct XrayIsolate *X, XrCoroutine *coro);

// Channel wake (auto fallback to single-thread mode)
XR_FUNC XrCoroutine *xr_runtime_wake_channel(struct XrayIsolate *X, void *channel,
                                             bool wake_sender);
XR_FUNC void xr_runtime_wake_channel_all(struct XrayIsolate *X, void *channel);

// Control
XR_FUNC void xr_coro_cancel(XrCoroutine *coro);

// Scope structured concurrency
XR_FUNC void xr_scope_add_coro(XrCoroState *sched, XrCoroutine *coro, XrCoroutine *parent);

#endif  // XCOROUTINE_H
