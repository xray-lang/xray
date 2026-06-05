/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xworker_internal.h - Internal shared declarations for worker implementation
 *
 * KEY CONCEPT:
 *   Shared includes, TLS variables, and internal helper declarations
 *   used by xworker.c and xworker_sysmon.c.
 */
#ifndef XWORKER_INTERNAL_H
#define XWORKER_INTERNAL_H

#include "xworker.h"
#include "../runtime/xisolate_internal.h"
#include "../runtime/xstrbuf.h"
#include "../runtime/object/xstring.h"
#include "xchannel.h"
#include "xcoro_pool.h"
#include "xcoro_tuning.h"
#include "../runtime/gc/xcoro_gc.h"
#include "../runtime/gc/xsystem_heap.h"
#include "xasync.h"
#include "xbalance.h"
#include "xtimer_wheel.h"
#include "xyieldable.h"
#include "../runtime/object/xexception.h"
#include "../runtime/xray_debug.h"
#include "xcoro_registry.h"

// TLS variables (defined in xworker.c)
extern XR_THREAD_LOCAL XrWorker *tls_current_worker;
extern XR_THREAD_LOCAL XrMachine *tls_current_machine;

// ========== Shared Helpers (inline) ==========

// Simple xorshift32 PRNG, used by worker_loop for steal target selection and
// inbox-first probabilistic drain. Single-owner use only.
static inline uint32_t xr_xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

// ========== Cross-file Internal API ==========

// Time / TLS / idle stack (xworker.c or xworker_sched.c)
XR_FUNC int64_t get_current_time_us(void);
XR_FUNC void wake_idle_worker(XrRuntime *rt);
XR_FUNC void wake_idle_workers(XrRuntime *rt, int max_wakes);
XR_FUNC void worker_unpark(XrWorker *worker);
XR_FUNC void *worker_loop(void *arg);

// Blocked queue internals (xworker_blocked.c)
XR_FUNC bool worker_blocked_list_remove(XrWorker *worker, XrCoroutine *coro);
XR_FUNC void worker_blocked_list_add(XrWorker *worker, XrCoroutine *coro);
XR_FUNC XrBlockedBucket *worker_blocked_bucket_find_or_create(XrWorker *worker, void *channel);
XR_FUNC XrBlockedBucket *worker_blocked_bucket_find(XrWorker *worker, void *channel);
XR_FUNC void worker_blocked_bucket_reclaim_if_empty(XrWorker *worker, XrBlockedBucket *bucket);
XR_FUNC void worker_clear_channel_waiter_mask(XrWorker *worker, void *channel);

// Execution core (xworker_exec.c) — shared with sched/handoff
XR_FUNC void worker_exec_with_cont_stealing(XrWorker *worker, XrCoroutine *coro);
XR_FUNC bool worker_process_blocked(XrWorker *worker, XrCoroutine *coro);

// Poll & inbox drain (xworker_sched.c) — shared with handoff
XR_FUNC void worker_drain_inbox(XrWorker *worker);
XR_FUNC int worker_pull_inject(XrWorker *worker, int max_per_priority);
// Returns a fast-path IO coroutine (affinity match, skip queue) or NULL.
XR_FUNC XrCoroutine *worker_poll_sources(XrWorker *worker);

// LIFO gate shared by the scheduler and fast dispatch.
XR_FUNC XrCoroutine *xr_worker_try_pop_lifo(XrWorker *worker, bool consume_poll_budget);

// Global injection queue (xworker_runq.c)
XR_FUNC void xr_injectq_init(XrRuntime *runtime);
XR_FUNC void xr_injectq_destroy(XrRuntime *runtime);
XR_FUNC void xr_injectq_push(XrRuntime *runtime, XrCoroutine *coro);
XR_FUNC void xr_injectq_push_batch(XrRuntime *runtime, XrCoroutine *first, XrCoroutine *last,
                                   int count, int priority);
XR_FUNC XrCoroutine *xr_injectq_pop_one(XrRuntime *runtime, int priority);
XR_FUNC int xr_injectq_pop_batch(XrRuntime *runtime, XrWorker *worker, int priority, int max_count);

// Sysmon constants
#define XR_SYSMON_WARN_US 100000
#define XR_SYSMON_CANCEL_US 5000000
#define XR_SYSMON_STEAL_US 1000

// Sysmon function (defined in xworker_sysmon.c)
XR_FUNC void *sysmon_thread_func(void *arg);

#endif  // XWORKER_INTERNAL_H
